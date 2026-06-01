# JIT Compiler: LLVM-IR + LLJIT for SQL, PL/SQL, Lua, XPath, XQuery

TransactionDB ships an optional in-process JIT compiler that lowers five
embedded source languages to native code via LLVM. It is shipped behind the
`TDB_BUILD_LLVM` CMake option (off by default) so the engine has zero LLVM
dependency unless you ask for it.

This document describes the JIT in depth: architecture, build setup, the
public API, the source-language grammars, the call ABI, the intrinsic
table, how to inspect emitted IR, and the supported and unsupported
constructs.

> **Status:** v0.1 of the JIT — numeric subset only (every value is an
> IEEE-754 `double`). Implemented and tested for PR #42. The architecture
> is intentionally extensible: adding a sixth language is writing a parser
> that populates the existing AST.

---

## 1. What problem does the JIT solve?

TransactionDB already has interpreters for SQL scalar expressions, PL/SQL
routine bodies, and (optionally) Lua. They are correct and small. But for
hot loops — a stored procedure summing a million rows, an XPath predicate
applied per row — the per-AST-node dispatch overhead dominates the actual
arithmetic.

The JIT removes that overhead by emitting a single LLVM IR module per
routine, parsing it with `LLVMParseIRInContext`, and JIT-compiling it via
LLVM-ORC LLJIT v2. The resulting native function pointer has a uniform C
calling convention so the call site does not vary by source language.

The five frontends share **one** AST, **one** constant folder, **one** IR
emitter, **one** intrinsic dispatch table and **one** execution engine.
Adding a sixth language is writing a parser; nothing else has to change.

---

## 2. Architecture

```
   ┌──────┬───────┬─────┬───────┬────────┐
   │ SQL  │ PL/SQL│ Lua │ XPath │ XQuery │   tdb_jit_lang
   └──┬───┴───┬───┴──┬──┴───┬───┴───┬────┘
      │       │      │      │       │
      ▼       ▼      ▼      ▼       ▼
       per-language parser  →  pl_expr / pl_stmt AST
                                    │
                                    ▼   tdb_jit_emit_ir_prog
                            constant folding pass
                                    │
                                    ▼
                            textual LLVM IR              ← tdb_jit_emit_ir
                                    │                      stops here when
                                    ▼                      no JIT is needed
                       LLVMParseIRInContext
                                    │
                                    ▼
                  ORC LLJIT (native target, lazy compile)
                                    │
                                    ▼
                  double f(const double *argv)            ← tdb_jit_fn
```

### File layout

```
src/jit/
  tdb_jit.h            public-ish API (also reachable from tests via -I)
  tdb_jit_int.h        internal header — pulls in tdb_plsql_int.h
  tdb_jit.c            ORC LLJIT engine + language dispatcher
  tdb_jit_ir.c         shared IR emitter (one signature for every language)
  tdb_jit_sql.c        SQL scalar expression frontend
  tdb_jit_lua.c        Lua subset frontend (statements + expressions)
  tdb_jit_xpath.c      XPath numeric expression frontend
  tdb_jit_xquery.c     XQuery FLWOR (let / return) numeric frontend

cmake/FindLLVMOpt.cmake   LLVM discovery (uses llvm-config-N first)
tests/test_jit.c          IR emission + native execution tests
```

### Shared AST

All five frontends build the `pl_expr` / `pl_stmt` / `tdb_plsql_proc` AST
defined in `src/plsql/tdb_plsql_int.h`. That AST already had:

| Construct      | Node                                        |
| -------------- | ------------------------------------------- |
| Number literal | `PL_E_NUM` with `double num`                |
| Variable       | `PL_E_VAR` with integer slot id             |
| Unary op       | `PL_E_UNARY` (NEG / NOT)                    |
| Binary op      | `PL_E_BINARY` (+ - * / %, comparisons, AND/OR) |
| Function call  | `PL_E_CALL` with name + args                |
| Assignment     | `PL_S_ASSIGN` slot = expr                   |
| If / elsif chain | `PL_S_IF` with `pl_clause[]` and else body |
| While loop     | `PL_S_WHILE`                                |
| For (integer)  | `PL_S_FOR` slot in lo..hi                   |
| Return         | `PL_S_RETURN`                               |

Each frontend feeds these structs into the same emitter, so:

1. Constant folding (`2 * 3 + 4` → `10.0`) is centralised.
2. The intrinsic table (`abs`, `sqrt`, `pow`, `min`, `max`, …) is shared.
3. Every JITted function exposes the same C ABI.

---

## 3. Build configuration

### CMake option

```sh
cmake -S . -B build -DTDB_BUILD_LLVM=ON     # JIT enabled
cmake -S . -B build -DTDB_BUILD_LLVM=OFF    # default: stub
```

When `TDB_BUILD_LLVM=ON`:

- `cmake/FindLLVMOpt.cmake` locates LLVM. It **prefers `llvm-config-N`**
  over `find_package(LLVM CONFIG)` because the latter ships broken
  imported-target references on Ubuntu 24.04 (LLVMSupport pulls in
  `zstd::libzstd_shared` even when zstd is not installed).
- `target_compile_definitions(transactiondb PRIVATE TDB_HAVE_LLVM=1)`.
- `target_link_libraries(transactiondb PUBLIC LLVM-N)` — **PUBLIC** because
  `transactiondb` is a static archive and consumers (test binaries, the
  shell, downstream embedders) have to inherit the link line.

When `TDB_BUILD_LLVM=OFF`:

- IR emitters still compile and work — you can dump IR text on hosts
  without LLVM headers, useful for offline `llc`/`clang` or for keeping
  CI fast.
- The JIT entry points (`tdb_jit_open`, `tdb_jit_compile_ir`,
  `tdb_jit_compile`) return `TDB_UNSUPPORTED`.
- `tdb_jit_is_available()` returns `0` so callers can branch at runtime.

### LLVM version

The JIT is built against LLVM **18** in CI (Ubuntu 24.04's
`llvm-18-dev`). It uses only the C API (`<llvm-c/*.h>`), so it is
forward-compatible with LLVM 19/20 as long as the ORC C API stays stable.
The `FindLLVMOpt.cmake` finder probes for `llvm-config-18`,
`llvm-config-17`, `llvm-config-16`, then `llvm-config` in that order.

### Required LLVM components

For an out-of-tree shared-library link (`-lLLVM-N`), no per-component
selection is needed. For static linking, the components touched are:

```
core support irreader orcjit executionengine native target
```

---

## 4. Public API

Header: `src/jit/tdb_jit.h`

```c
typedef struct tdb_jit tdb_jit;
typedef double (*tdb_jit_fn)(const double *argv);

typedef enum tdb_jit_lang {
  TDB_JIT_SQL    = 1,
  TDB_JIT_PLSQL  = 2,
  TDB_JIT_LUA    = 3,
  TDB_JIT_XPATH  = 4,
  TDB_JIT_XQUERY = 5,
} tdb_jit_lang;

int  tdb_jit_is_available(void);
const char *tdb_jit_llvm_version(void);

int  tdb_jit_open(tdb_jit **out);
void tdb_jit_close(tdb_jit *j);

int  tdb_jit_emit_ir(tdb_jit_lang lang, const char *src,
                     const char *const *params, int nparams,
                     struct tdb_buf *out, char *errbuf, int errlen);

int  tdb_jit_compile(tdb_jit *j, tdb_jit_lang lang,
                     const char *src,
                     const char *const *params, int nparams,
                     tdb_jit_fn *fn, char *errbuf, int errlen);

int  tdb_jit_compile_ir(tdb_jit *j,
                        const char *ir, size_t irlen,
                        const char *fname,
                        tdb_jit_fn *fn, char *errbuf, int errlen);
```

### Function reference

#### `int tdb_jit_is_available(void)`

Returns `1` if built with `TDB_BUILD_LLVM=ON`, else `0`. Call before
`tdb_jit_open` if your code path has a graceful fallback (e.g. dispatching
to `tdb_plsql_exec` instead).

#### `const char *tdb_jit_llvm_version(void)`

`"<major>.<minor>.<patch>"` when JIT is available, `NULL` when stubbed.

#### `int tdb_jit_open(tdb_jit **out)`

Creates an ORC LLJIT instance with a single main JITDylib. Returns
`TDB_UNSUPPORTED` when the JIT is stubbed. Multiple `tdb_jit` instances
are safe; they do not share state.

#### `void tdb_jit_close(tdb_jit *j)`

Tears down the execution session. **All function pointers previously
returned by `tdb_jit_compile` / `tdb_jit_compile_ir` become invalid.**

#### `int tdb_jit_emit_ir(lang, src, params, nparams, out, errbuf, errlen)`

Lower a source string in `lang` to textual LLVM IR and append to `out` (a
`tdb_buf`). Works whether or not the JIT engine is enabled — useful for
offline `llc`/`opt`/`clang` workflows, or simply to inspect what the
frontend emits.

Returns `TDB_OK` on success. On parse error, returns `TDB_ERROR` and
writes a short description to `errbuf`. On a construct outside the
numeric subset (string concatenation, unknown function), returns
`TDB_UNSUPPORTED`.

#### `int tdb_jit_compile(j, lang, src, params, nparams, fn, errbuf, errlen)`

The convenience path: lower to IR and JIT it in one call. On success,
`*fn` is a native callable. The function name in the emitted IR is
`plsql_routine` for PL/SQL and `jit_entry` for the other four languages.

#### `int tdb_jit_compile_ir(j, ir, irlen, fname, fn, errbuf, errlen)`

The low-level path: hand the engine a precomputed IR module (perhaps one
you ran through `opt -O3`) and look up `fname`. Returns the function
pointer if both the module addition and the symbol lookup succeed.

### Calling convention

Every JITted function has the IR signature

```llvm
define double @<fname>(ptr %argv)
```

and the matching C type `double f(const double *argv)`. Parameter `i`
(0-indexed) is loaded from `argv[i]`. The return value is always an IEEE-
754 double.

That uniformity is deliberate: the caller does not vary by source
language. If a SQL expression has two parameters and a Lua function has
seven, both are called the same way — pass a `double[]` of the right
size.

---

## 5. Source-language grammars

### 5.1 SQL — `TDB_JIT_SQL`

Numeric scalar expression. Single expression returns the result.

```
expr     := or
or       := and ('OR' and)*
and      := not ('AND' not)*
not      := 'NOT' not | rel
rel      := add (('=' | '==' | '<>' | '!=' | '<' | '<=' | '>' | '>=') add)?
add      := mul (('+' | '-') mul)*
mul      := unary (('*' | '/' | '%') unary)*
unary    := '-' unary | primary
primary  := number | name | name '(' args? ')' | '(' expr ')'
args     := expr (',' expr)*
```

| Element                          | Notes                                          |
| -------------------------------- | ---------------------------------------------- |
| Comparison result                | Yields `1.0` / `0.0` (not a boolean type)      |
| `<>` and `!=`                    | Both map to NE                                 |
| `||`                             | **Not supported** (would require strings)      |
| `--` line comments               | Supported                                      |
| Identifiers                      | Match parameter names passed to `tdb_jit_*`    |
| Function calls                   | See [§7 Intrinsic table](#7-intrinsic-table)   |

Example:

```c
const char *p[] = {"a", "b"};
double argv[] = {3.0, 4.0};
tdb_jit_fn fn;
tdb_jit_compile(j, TDB_JIT_SQL, "sqrt(a*a + b*b)", p, 2, &fn, err, sizeof(err));
fn(argv);  // → 5.0
```

### 5.2 PL/SQL — `TDB_JIT_PLSQL`

Uses the **existing** PL/SQL parser (`src/plsql/tdb_plsql_parse.c`), so
the full numeric subset already accepted by `tdb_plsql_exec` is JIT-able.

```
routine  := ('DECLARE' decl_list)? 'BEGIN' stmt_list 'END' ';'?
decl     := name type (':=' expr)? ';'
stmt     := 'NULL' ';'
          | name ':=' expr ';'
          | 'RETURN' expr? ';'
          | 'IF' expr 'THEN' stmt_list ('ELSIF' expr 'THEN' stmt_list)* ('ELSE' stmt_list)? 'END' 'IF' ';'
          | 'WHILE' expr 'LOOP' stmt_list 'END' 'LOOP' ';'
          | 'FOR' name 'IN' expr '..' expr 'LOOP' stmt_list 'END' 'LOOP' ';'
expr     := standard SQL-shaped expression
```

Example:

```c
const char *p[] = {"x"};
const char *src =
  "DECLARE s INTEGER := 0; "
  "BEGIN FOR i IN 1 .. x LOOP s := s + i; END LOOP; RETURN s; END";
// fn({100}) → 5050
```

### 5.3 Lua — `TDB_JIT_LUA`

Numeric subset of Lua 5.x.

```
stmt    := 'local' Name '=' expr
         | Name '=' expr
         | 'if' expr 'then' block ('elseif' expr 'then' block)* ('else' block)? 'end'
         | 'while' expr 'do' block 'end'
         | 'for' Name '=' expr ',' expr (',' expr)? 'do' block 'end'
         | 'return' expr
         | ';'
expr    := orx
orx     := andx ('or' andx)*
andx    := relx ('and' relx)*
relx    := addx (('<' | '<=' | '>' | '>=' | '==' | '~=') addx)?
addx    := mulx (('+' | '-') mulx)*
mulx    := powx (('*' | '/' | '%') powx)*
powx    := unaryx ('^' powx)?              ; right-associative
unaryx  := ('-' | 'not') unaryx | primary
primary := Number
         | Name ('.' Name)? ('(' args? ')')?     ; lib.fn(args), x, x.y(args)
         | '(' expr ')'
```

| Construct                  | Behaviour                                                                |
| -------------------------- | ------------------------------------------------------------------------ |
| `--` / `--[[ ... ]]`       | Line and block comments — supported                                      |
| `~=`                       | Maps to NE                                                               |
| `^`                        | Right-associative; lowers to `llvm.pow.f64`                              |
| `math.X(...)`              | Library-prefixed function calls; the leaf name is what dispatches        |
| Numeric for-step           | Accepted in the grammar but **ignored** (loops always step by `+1`)      |
| Strings, tables, functions | Not supported                                                            |

Example:

```c
const char *src =
  "local s = 0\n"
  "for i = 1, n do\n"
  "  s = s + i * i\n"
  "end\n"
  "return math.sqrt(s)\n";
// fn({10}) → sqrt(385) ≈ 19.621
```

### 5.4 XPath — `TDB_JIT_XPATH`

Numeric XPath 1.0 / value-comparison portion of XPath 2.0/3.1.

```
Expr      := OrExpr
OrExpr    := AndExpr ('or' AndExpr)*
AndExpr   := CmpExpr ('and' CmpExpr)*
CmpExpr   := AddExpr (('=' | '!=' | '<' | '<=' | '>' | '>=' |
                       'eq' | 'ne' | 'lt' | 'le' | 'gt' | 'ge') AddExpr)?
AddExpr   := MulExpr (('+' | '-') MulExpr)*
MulExpr   := UnaryExpr (('*' | 'div' | 'idiv' | 'mod') UnaryExpr)*
UnaryExpr := '-' UnaryExpr | 'not' UnaryExpr | Primary
Primary   := Number | '$' Name | Name '(' Args? ')' | '(' Expr ')'
```

Notes:

- **Path expressions are not supported.** TransactionDB does not yet
  model an XML tree at the storage layer, so `child::a/b[2]` etc. would
  have nothing to evaluate against. Use `$variables` instead.
- The XPath rule that operator keywords (`and`, `or`, `mod`, `div`,
  `idiv`, `eq`, `ne`, `lt`, `le`, `gt`, `ge`) are only recognised as
  operators after an operand is implemented by tracking
  `last_was_name` in the lexer.
- `div` and `idiv` both lower to `fdiv`; `idiv`'s integer-truncation
  semantics are not yet modelled.
- Comments use the XQuery `(: ... :)` form (nested allowed).

Example:

```c
const char *p[] = {"x"};
double a[] = {17.0};
tdb_jit_compile(j, TDB_JIT_XPATH, "ceiling($x div 4)", p, 1, &fn, ...);
fn(a);  // → 5.0
```

### 5.5 XQuery — `TDB_JIT_XQUERY`

Numeric FLWOR subset: `let` bindings followed by `return`.

```
FLWOR := ('let' '$' Name ':=' Expr)* 'return' Expr
       | Expr                                         ; bare expression
Expr  := <same as XPath>
```

Each `let` binding compiles to a `PL_S_ASSIGN` followed by a final
`PL_S_RETURN`. The whole sequence runs through the shared IR emitter.

Notes:

- `for $x in lo to hi return expr` is **not** supported in this version —
  it would require encoding "sequence of doubles" or an aggregation
  function, which is out of scope for a numeric JIT.
- Conditional expressions `if (cond) then expr else expr` are **not**
  supported as expressions in the current AST (no conditional-expression
  node). Use `(cond) * a + (1 - cond) * b` for now if needed, or split
  into PL/SQL.

Example:

```c
const char *p[] = {"n"};
const char *src =
  "let $a := $n * 2\n"
  "let $b := $a + 3\n"
  "return $b * $b\n";
// fn({5}) → (5*2 + 3)^2 = 169.0
```

---

## 6. Generated IR

Every emitted module has the same overall shape:

```llvm
define double @<fname>(ptr %argv) {
L0:
  %sret    = alloca double
  store double 0.0, ptr %sret
  %sforhi  = alloca double
  ; one alloca per slot, initialised from argv[i] or to 0.0
  %s0 = alloca double
  %pg0 = getelementptr double, ptr %argv, i64 0
  %pv0 = load double, ptr %pg0
  store double %pv0, ptr %s0
  ...

  ; body — branches and basic blocks for IF/WHILE/FOR/RETURN
  br label %L_exit

L_exit:
  %rv = load double, ptr %sret
  ret double %rv
}

declare double @llvm.fabs.f64(double)
declare double @llvm.sqrt.f64(double)
declare double @llvm.floor.f64(double)
declare double @llvm.ceil.f64(double)
declare double @llvm.round.f64(double)
declare double @llvm.sin.f64(double)
declare double @llvm.cos.f64(double)
declare double @llvm.exp.f64(double)
declare double @llvm.log.f64(double)
declare double @llvm.pow.f64(double, double)
```

### Why allocas?

Every variable slot is an `alloca double` initialised once per call,
written by every assignment, and re-loaded on every read. This is
intentional: it makes lowering trivial (no SSA construction in the
emitter) and lets LLVM's standard `-mem2reg` / `-sroa` passes promote
slots to SSA registers as the very first thing `opt`/the JIT does.

Round-trip through `opt -O2`:

```llvm
; before opt:                              ; after opt -O2:
%s0 = alloca double                        define double @jit_entry(ptr %argv) {
%pg0 = getelementptr double, ptr %argv, …    %t2 = fmul double %pv0, %pv0
%pv0 = load double, ptr %pg0                 %t5 = fmul double %pv1, %pv1
store double %pv0, ptr %s0                   %t6 = fadd double %t2, %t5
…                                            %t7 = tail call double @llvm.sqrt.f64(double %t6)
                                             ret double %t7
                                           }
```

Every alloca/store/load melts away and you get the IR you would write by
hand.

### Constant folding

Pure literal sub-trees are folded by `tdb_jit_emit_ir_prog` before any IR
is emitted:

```c
tdb_jit_emit_ir(TDB_JIT_SQL, "2 * 3 + 4", NULL, 0, &ir, ...);
// emitted body stores `10.0` directly into %sret, no fmul/fadd.
```

The folder handles `+`, `-`, `*`, `/`, `%` and unary negation on numeric
literals; comparisons and Boolean ops are not folded.

---

## 7. Intrinsic table

Function calls in every frontend dispatch through one table in
`tdb_jit_ir.c`:

| Source name(s)              | Args | LLVM intrinsic                |
| --------------------------- | ---: | ----------------------------- |
| `abs`                       | 1    | `llvm.fabs.f64`               |
| `sqrt`                      | 1    | `llvm.sqrt.f64`               |
| `floor`                     | 1    | `llvm.floor.f64`              |
| `ceil`, `ceiling`           | 1    | `llvm.ceil.f64`               |
| `round`                     | 1    | `llvm.round.f64`              |
| `sin`                       | 1    | `llvm.sin.f64`                |
| `cos`                       | 1    | `llvm.cos.f64`                |
| `exp`                       | 1    | `llvm.exp.f64`                |
| `log`                       | 1    | `llvm.log.f64`                |
| `pow`, `power`              | 2    | `llvm.pow.f64`                |
| `min`, `least`              | 2    | `fcmp olt` + `select`         |
| `max`, `greatest`           | 2    | `fcmp ogt` + `select`         |
| `number`                    | 1    | identity (XPath compatibility) |

Names are case-insensitive (`strcasecmp`). Lua's `math.X(args)` form
ignores the `math.` prefix — `math.sqrt(x)` and `sqrt(x)` are the same.
Lua's `x ^ y` desugars to `pow(x, y)`.

Unknown function calls fail the lower with `TDB_UNSUPPORTED` and an
`errbuf` like `"function not supported in JIT lowering"`.

---

## 8. Status codes

| Status            | Meaning in JIT context                                                    |
| ----------------- | ------------------------------------------------------------------------- |
| `TDB_OK`          | Compile/IR emission succeeded.                                            |
| `TDB_MISUSE`      | NULL pointer argument or unknown `lang` value.                            |
| `TDB_NOMEM`       | Out of memory.                                                            |
| `TDB_ERROR`       | Parse error (frontend) or LLVM internal error. `errbuf` set.              |
| `TDB_UNSUPPORTED` | JIT not built in, **or** source uses an unsupported construct (strings, unknown function, …). |

---

## 9. Worked examples

### 9.1 Filtering a query with a JIT-compiled predicate

```c
tdb_jit *j; tdb_jit_open(&j);
const char *params[] = {"x"};
tdb_jit_fn pred;
char err[256];
tdb_jit_compile(j, TDB_JIT_SQL,
                "x > 0 AND x mod 3 = 0 AND sqrt(x) < 100",
                params, 1, &pred, err, sizeof(err));

for (double x = 0; x < 10000; x++)
  if (pred(&x) != 0.0)
    /* x passes the predicate */;

tdb_jit_close(j);
```

### 9.2 Sum-of-squares stored procedure

```c
const char *p[] = {"n"};
const char *src =
  "DECLARE s INTEGER := 0; "
  "BEGIN FOR i IN 1 .. n LOOP s := s + i * i; END LOOP; RETURN s; END";
tdb_jit_compile(j, TDB_JIT_PLSQL, src, p, 1, &fn, err, sizeof(err));
double v[] = {1000.0};
fn(v);  // 333833500.0
```

### 9.3 Mixing languages in one JIT instance

```c
tdb_jit_fn lua_fn, xpath_fn;
const char *p[] = {"x"};

tdb_jit_compile(j, TDB_JIT_LUA,
  "return math.floor(math.sqrt(x))", p, 1, &lua_fn, err, sizeof(err));
tdb_jit_compile(j, TDB_JIT_XPATH,
  "ceiling($x div 7)",                p, 1, &xpath_fn, err, sizeof(err));

double v[] = {50.0};
lua_fn(v);    // 7.0
xpath_fn(v);  // 8.0
```

Both functions live in the same `tdb_jit` execution session and remain
valid until `tdb_jit_close`.

### 9.4 Inspecting emitted IR offline

The IR emitter works **without** LLVM linked in, so you can dump it from
any host:

```c
tdb_buf ir; tdb_buf_init(&ir);
char err[256];
tdb_jit_emit_ir(TDB_JIT_XQUERY,
  "let $a := $n + 1 let $b := $a * $a return $b - 1",
  (const char*[]){"n"}, 1, &ir, err, sizeof(err));
fwrite(ir.data, 1, ir.len, stdout);
tdb_buf_free(&ir);
```

Pipe it to LLVM tools:

```sh
./dump_ir xquery "..." > /tmp/x.ll
opt-18 -O2 -S /tmp/x.ll      # optimise + print
llc-18 /tmp/x.ll -o /tmp/x.s # native asm
clang-18 -c /tmp/x.ll -o /tmp/x.o
```

---

## 10. Numeric subset — what is **not** supported

The JIT is a *numeric* compiler. Anything outside double-precision math is
rejected at lower time with `TDB_UNSUPPORTED`:

- **Strings.** String literals (`PL_E_STR`) and the `||` concat operator
  bail out. SQL's `||`, PL/SQL's `||`, Lua's `..` — all unsupported.
- **NULLs and three-valued logic.** No NULL representation; comparisons
  produce a `0.0`/`1.0` double, not a SQL boolean.
- **Aggregates.** `SUM`, `COUNT`, `AVG`, … are not function calls in the
  intrinsic table — they need a row stream.
- **Table / row access.** No way to read a column from a row. The JITted
  function is pure: argv in, double out.
- **XPath path expressions.** No XML tree to walk.
- **XQuery `for`, `where`, `order by`, `if then else` expressions.** Only
  `let ... return ...` is supported.
- **Lua tables, strings, functions, multiple returns.** Not in scope.
- **PL/SQL `OUT` / `INOUT` parameters, cursors, exception handlers.** Not
  in scope.

When the lowering trips one of these, you get `TDB_UNSUPPORTED` and a
short message — the caller is expected to fall back to the corresponding
interpreter (`tdb_plsql_exec`, the SQL executor, the Lua VM).

---

## 11. Concurrency and lifetime

| Object               | Threading                                                |
| -------------------- | -------------------------------------------------------- |
| `tdb_jit`            | Not internally locked — own it from one thread, or guard with your own mutex |
| Function pointers    | Safe to call from any thread once published (no JIT state mutated on the call path) |
| `tdb_jit_close`      | Invalidates **every** function pointer obtained from that `tdb_jit` |
| `tdb_jit_compile`    | Lazy compile — first call may JIT, subsequent calls are direct invocations |

Each `tdb_jit_compile` adds a new module to the engine's main JITDylib;
duplicate symbol names within one dylib would conflict, so the dispatcher
uses different default function names per language (`plsql_routine` vs
`jit_entry`). When mixing many functions from the same language, pass
distinct names via `tdb_jit_compile_ir` or compile each into its own
`tdb_jit` instance.

---

## 12. Testing

`tests/test_jit.c` has two tiers:

1. **IR emission** — runs in both build modes (with and without LLVM)
   and verifies each frontend's output contains the right basic blocks,
   instructions, and intrinsic declarations. Also verifies constant
   folding (`2 * 3 + 4` → `10.0`).
2. **Native execution** — only compiled when `TDB_HAVE_LLVM` is defined.
   Opens a JIT, compiles a non-trivial program in each language
   (including a loop-summing PL/SQL routine, a recursive Lua program, an
   XPath comparison expression and a chained XQuery `let`), invokes the
   resulting function with several argument sets and checks the results
   numerically.

Test counts:

| Mode                       | Cases run | Assertions |
| -------------------------- | --------: | ---------: |
| `TDB_BUILD_LLVM=OFF`       |         8 |         22 |
| `TDB_BUILD_LLVM=ON`        |        13 |         36 |

```sh
ctest --test-dir build -R jit --output-on-failure
```

---

## 13. Extension points

### Adding a sixth language

1. Add a new value to `tdb_jit_lang` in `tdb_jit.h`.
2. Write `src/jit/tdb_jit_<lang>.c` with one entry point:

   ```c
   int tdb_jit_<lang>_parse(const char *src, const char *const *params,
                            int nparams, tdb_plsql_proc **out,
                            char *errbuf, int errlen);
   ```

   …that populates a `tdb_plsql_proc` using the `pl_expr` / `pl_stmt`
   constructors (see `tdb_jit_sql.c` for the smallest working example).
3. Dispatch to it from the switch in `tdb_jit_emit_ir` (in `tdb_jit.c`).
4. No CMake changes needed — `src/**/foo.c` is glob-included.

### Adding a builtin function

Add a branch to `emit_call` in `tdb_jit_ir.c` mapping the source name to
an LLVM intrinsic (preferred) or to a sequence of IR operations. Ensure
the matching `declare` line is in the trailer.

### Adding an AST node

The shared AST lives in `src/plsql/tdb_plsql_int.h`. Extending it
(e.g. adding a conditional-expression node `PL_E_IFEXPR` for XQuery
`if then else`) is the natural way to lift current numeric-subset
limitations. Update:

1. The struct definition in `tdb_plsql_int.h`.
2. The emitter in `src/jit/tdb_jit_ir.c` (`emit_num` / `emit_bool`).
3. The interpreter in `src/plsql/tdb_plsql_interp.c` if you want parity.
4. The frontends that produce the new node.

---

## 14. Future work

Items deliberately deferred from the v0.1 scope:

- **Integer codegen.** Emit `i64`-typed IR for routines whose operations
  are demonstrably integer (PL/SQL parameter type `INTEGER`, no `/`, no
  builtins returning `double`). Would let LLVM use the integer ALU and
  avoid the float-pipeline penalty.
- **Aggregate streams.** A second JIT signature `void f(double *acc,
  const double *row)` would compile aggregate bodies for use by the
  executor's stream operators.
- **XQuery FLWOR `for`.** Either as an implicit `SUM` aggregation or as
  a sequence-returning interface.
- **String operations.** Requires an opaque blob-handle ABI and runtime
  helpers (`strlen`, `strcmp`, …) registered with LLJIT.
- **Cross-module optimisation.** Run `opt -O2` over the emitted IR
  before adding it to the JIT. The ORC IR transform layer
  (`LLVMOrcLLJITGetIRTransformLayer`) is the right hook.
- **Save-and-load.** Persist compiled object files via
  `LLVMOrcLLJITAddObjectFile` so warm starts skip the compile.

---

## 15. References

- Source: `src/jit/` and `tests/test_jit.c`
- Shared AST: `src/plsql/tdb_plsql_int.h`
- Existing PL/SQL interpreter (fallback path): `src/plsql/tdb_plsql_interp.c`
- PR introducing the JIT: [#42](https://github.com/ExprsnIO/TransactionDB/pull/42)
- LLVM C API headers used:
  - `<llvm-c/Core.h>` — module, context, memory buffers
  - `<llvm-c/IRReader.h>` — `LLVMParseIRInContext`
  - `<llvm-c/Orc.h>` — thread-safe context/module
  - `<llvm-c/LLJIT.h>` — `LLVMOrcLLJITAddLLVMIRModule`, `LLVMOrcLLJITLookup`
  - `<llvm-c/Target.h>` — `LLVMInitializeNativeTarget`,
    `LLVMInitializeNativeAsmPrinter`
  - `<llvm-c/Error.h>` — error-message handling
