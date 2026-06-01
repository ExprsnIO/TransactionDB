/*
** tdb_jit.h — LLVM-IR + LLVM-ORC JIT compiler for the procedural / query
** languages embedded in TransactionDB.
**
** Five front ends share a single numeric AST (the PL/SQL `pl_expr`/`pl_stmt`
** node types, reused so the same constant-folder and IR emitter serve them
** all) and a single back end:
**
**   ┌────────┬────────┬────────┬────────┬─────────┐
**   │   SQL  │ PL/SQL │  Lua   │ XPath  │ XQuery  │   tdb_jit_lang
**   └───┬────┴───┬────┴───┬────┴───┬────┴────┬────┘
**       │        │        │        │         │
**       ▼        ▼        ▼        ▼         ▼
**          source → AST (pl_expr / pl_stmt)
**                                │
**                                ▼
**            constant-folding + tdb_jit_emit_ir_prog
**                                │
**                                ▼      tdb_jit_emit_ir(): stops here
**                         textual LLVM IR
**                                │
**                                ▼   tdb_jit_compile_ir / tdb_jit_compile:
**                       LLVMParseIRInContext
**                                │
**                                ▼
**                  ORC LLJIT (native target, lazy)
**                                │
**                                ▼
**                  double f(const double *argv)    ← tdb_jit_fn
**
** The compiled function takes a single `const double *argv` so the caller
** does not have to vary the C calling convention per routine. Argument i
** is loaded from argv[i]. The return value is always an IEEE-754 double.
**
** When the project is built without -DTDB_BUILD_LLVM=ON the IR emitters
** still work (so you can compile and inspect IR text on a host that has
** no LLVM dev headers), but tdb_jit_open() and tdb_jit_compile_ir() return
** TDB_UNSUPPORTED. Check tdb_jit_is_available() to detect this at runtime.
*/
#ifndef TDB_JIT_H
#define TDB_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tdb_buf;

/* Opaque handle owning the underlying LLVM execution session + dylib. */
typedef struct tdb_jit tdb_jit;

/* The C calling convention every JITted routine exposes:
**   double f(const double *argv);
** where argv has the same arity as the source-language formal parameters. */
typedef double (*tdb_jit_fn)(const double *argv);

/* Source languages dispatched on by tdb_jit_emit_ir / tdb_jit_compile. */
typedef enum tdb_jit_lang {
  TDB_JIT_SQL    = 1,   /* numeric SQL scalar expression                 */
  TDB_JIT_PLSQL  = 2,   /* PL/SQL routine body (DECLARE? BEGIN ... END;) */
  TDB_JIT_LUA    = 3,   /* numeric Lua function body                     */
  TDB_JIT_XPATH  = 4,   /* XPath 1.0/2.0 numeric expression              */
  TDB_JIT_XQUERY = 5    /* XQuery FLWOR numeric subset                   */
} tdb_jit_lang;

/* Returns 1 when the library was built with TDB_BUILD_LLVM=ON, else 0. */
int tdb_jit_is_available(void);

/* Library version of the linked LLVM, or NULL when no LLVM is linked. */
const char *tdb_jit_llvm_version(void);

/* Create/destroy a JIT instance. A single instance can hold any number of
** independently compiled functions; they share an LLVM ExecutionSession. */
int  tdb_jit_open(tdb_jit **out);
void tdb_jit_close(tdb_jit *j);

/*
** Lower a source string in `lang` to textual LLVM IR and append it to
** `out`. `params[0..nparams)` give the formal parameter names; the i-th
** name maps to argv[i]. For PL/SQL this is just the normal parameter
** list; for SQL/Lua/XPath/XQuery these names become bindable variables
** in the source (`x` in SQL, `$x` in XPath/XQuery, `x` in Lua).
**
** Returns TDB_OK on success. On parse/lower error returns a non-OK status
** and writes a short description to errbuf (if non-NULL).
*/
int tdb_jit_emit_ir(tdb_jit_lang lang, const char *src,
                    const char *const *params, int nparams,
                    struct tdb_buf *out,
                    char *errbuf, int errlen);

/*
** Compile a source string and JIT it. The resulting function pointer
** *fn lives as long as `j` (until tdb_jit_close). When `lang ==
** TDB_JIT_PLSQL`, `params` is interpreted as the PL/SQL formal-parameter
** list and the function name in the generated IR is "plsql_routine".
** For all other languages the emitted function is named "jit_entry".
*/
int tdb_jit_compile(tdb_jit *j, tdb_jit_lang lang,
                    const char *src,
                    const char *const *params, int nparams,
                    tdb_jit_fn *fn,
                    char *errbuf, int errlen);

/*
** Low-level: hand the JIT a precomputed IR module (e.g. one produced by
** tdb_jit_emit_ir) and look up `fname`. Useful when you want to combine
** IR from multiple front ends or run an external `opt` pass first.
*/
int tdb_jit_compile_ir(tdb_jit *j,
                       const char *ir, size_t irlen,
                       const char *fname,
                       tdb_jit_fn *fn,
                       char *errbuf, int errlen);

#ifdef __cplusplus
}
#endif

#endif /* TDB_JIT_H */
