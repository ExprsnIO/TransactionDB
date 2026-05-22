# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

TDB is a SQL database engine written in C11 (core) and C++20 (SQL/catalog/scripting layers). Apache 2.0 license, Copyright 2026 Rick Holland. ~37K lines across ~75 files.

## Build Commands

```bash
# Configure (from project root)
mkdir -p build && cd build
cmake .. -DTDB_BUILD_TESTS=OFF

# Build CLIs
cmake --build . --target tdbcli --target tdb-tui -j8

# Build a single library
cmake --build . --target tdb_sql -j8

# Optional features (off by default)
cmake .. -DTDB_WITH_LLVM=ON   # Lua-to-LLVM-IR JIT compiler (requires brew install llvm)
cmake .. -DTDB_WITH_JSC=ON    # JavaScriptCore engine (macOS only)
cmake .. -DTDB_WITH_ZSTD=ON   # zstd page compression
cmake .. -DTDB_WITH_OPENSSL=ON # OpenSSL AES-256-GCM
cmake .. -DTDB_WITH_ICU=ON    # ICU Unicode case folding

# Run the CLI
./build/tdbcli                    # in-memory interactive
./build/tdbcli path/to/db.tdb    # named database (auto-saves on exit)
./build/tdbcli -f script.sql     # execute SQL file
./build/tdbcli -e "SELECT 1"     # execute single statement
```

The build uses `-Wall -Wextra -Wpedantic -Werror` — all warnings are errors.

## Architecture

### Library Dependency Graph

```
tdb (top-level facade)
 +-- tdb_sql       (lexer, parser, AST, optimizer, executor, type coercion)
 +-- tdb_catalog   (tables, views, matviews, sequences, indexes, partitions, INFORMATION_SCHEMA)
 +-- tdb_doc       (XPath/XQuery/GraphQL on JSON/XML columns)
 +-- tdb_script    (Lua 5.4 interpreter, optional LLVM JIT, optional JSC)
 |    +-- tdb_lua  (vendored Lua 5.4.7 in third_party/lua/)
 |    +-- tdb_jit  (optional: Lua bytecode -> LLVM IR -> native, src/script/jit/)
 |    +-- tdb_jsc  (optional: JavaScriptCore via jsengine.mm, src/script/js/)
 +-- tdb_persistence (C storage <-> C++ Database bridge)
 +-- tdb_core      (C: page, storage, buffer, WAL, txn, crypto)
 +-- tdb_index     (C: B+Tree, B-Tree, R-Tree, R+Tree, Hash)
```

### Key Files

- **`include/tdb/database.h`** (~6600 lines) -- The monolithic `Database` class. Contains `execute_stmt()` (dispatches ~50 statement types), `eval_expr_with_row()` (200+ scalar functions), `compute_aggregate()`, `plsql_exec_block()` (PL/SQL interpreter), partition pruning, FK enforcement, trigger firing, cursor state, privilege checks, window function evaluation, and all DML/DDL execution. This is the file you'll modify most often.
- **`include/tdb/sql/ast.h`** (~870 lines) -- 53 `StmtType` enums, 16 `ExprType` enums, and all AST structs. The `Statement::data` variant holds ~40 concrete statement types.
- **`include/tdb/catalog.h`** -- `Catalog` class with `TableInfo` (including columnar storage), `ViewInfo`, `IndexInfo`, `SequenceInfo`, `ScriptInfo`, `TriggerInfo`, `TablespaceInfo`, `Privilege`. Row data stored inline in `TableInfo.rows` (or `PartitionInfo.rows` for partitioned tables, or `ColumnStore` for columnar tables).
- **`include/tdb/sql/executor.h`** -- `sql::Value` tagged union with 22 types (NULL, INT64, FLOAT64, STRING, BOOL, BLOB, DATE, TIMESTAMP, DECIMAL, UUID, JSON, XML, GEOMETRY, ARRAY, COMPOSITE, etc.), `ResultSet`, `Schema`.
- **`src/sql/parser/parser.cpp`** (~3800 lines) -- Recursive descent parser with ~25 parse functions. Produces AST from `ast.h`.
- **`src/cli/tdbcli.cpp`** -- CLI with PL/SQL-aware `BEGIN...END` block tracking in `-f` file mode and string-literal-aware semicolon splitting.
- **`src/script/script_engine.cpp`** -- ScriptEngine: Lua interpreter + optional JIT dispatch + JS bridge.

### How SQL Execution Flows

```
SQL text -> Parser::parse() -> AST Statement -> Database::execute_stmt()
  -> dispatches by StmtType to exec_* methods
  -> SELECT: resolve FROM (tables/views/CTEs/subqueries), evaluate JOINs,
     apply WHERE (with index prefiltering), GROUP BY (supports function exprs),
     HAVING (with aggregate re-evaluation), window functions, ORDER BY, LIMIT/OFFSET
  -> INSERT: build_insert_row, check_constraints (PK/FK/UNIQUE/CHECK/DOMAIN),
     ON CONFLICT handling, trigger firing (BEFORE/AFTER), RETURNING
  -> Expression evaluation: eval_expr_with_row() handles 200+ functions,
     operators, CASE, COALESCE, CAST, correlated subqueries, EXISTS/IN
```

### PL/SQL Interpreter

The `plsql_exec_block()` function interprets procedure bodies. It tokenizes the body on semicolons via `plsql_tokenize()`, then processes each token:
- `DECLARE var type := value` -- variable declaration with literal parsing
- `var := expr` -- assignment via SQL evaluation with `plsql_subst_vars()`
- `IF cond THEN ... ELSIF cond THEN ... ELSE ... END IF` -- conditional with inline body extraction
- `FOR var IN start..end LOOP ... END LOOP` -- numeric FOR with inline body
- `WHILE cond LOOP ... END LOOP` -- while loops with condition re-evaluation
- `RETURN expr` -- return with variable substitution
- `RAISE EXCEPTION 'msg'` -- exception raising
- Anything else is executed as embedded SQL with variable substitution

**Key detail**: `plsql_subst_vars()` performs single-pass word-boundary-aware replacement, formatting STRING values as `'quoted'` SQL literals and numeric values as-is. Assignment handler checks `!r.rows[0][0].is_null()` to distinguish SQL NULL results from valid values (falls back to string literal interpretation).

### Patterns for Common Changes

**Adding a new scalar function:**
In `database.h`, find `eval_expr_with_row()`, locate the `if (upper == "...")` chain, add your function. Functions receive `vals` (vector of evaluated arguments).

**Adding a new aggregate:**
1. Register in `src/sql/parser/parser.cpp` at the `is_agg` check (~line 2380)
2. Implement in `database.h` at `compute_aggregate()`

**Adding a new SQL keyword:**
1. Add to `TokenType` enum in `include/tdb/sql/token.h`
2. Add to the keyword map in `src/sql/lexer/lexer.cpp`
3. If usable as an identifier, add to `is_identifier_or_keyword()` in `parser.cpp`
4. If usable as a function name (like YEAR, LEFT, IF), also add to the keyword-as-function check in `parse_primary()` (~line 2253)

**Adding a new statement type:**
1. Add to `StmtType` enum and create AST struct in `include/tdb/sql/ast.h`
2. Add the struct to `Statement::data` variant
3. Add the name in `src/sql/ast/ast.cpp` `stmt_type_name()`
4. Parse in `src/sql/parser/parser.cpp` (add to `parse_statement()` dispatch + declare in `parser.h`)
5. Execute in `database.h` `execute_stmt()` switch

### Known Engine Limitations

- `FULL OUTER JOIN` and `CROSS JOIN` have aliasing issues; use implicit join syntax for cross products
- Encrypted tables (`CREATE TABLE ... ENCRYPTED`) cannot participate in JOINs
- GRANT enforcement only applied when `current_user_` is set (empty = superuser bypass)
- PL/SQL `BEGIN...END` blocks in multi-line files require the `AS BEGIN` to appear on a line containing `AS`; string-body (`AS '...'`) is more reliable for complex procedures
- Columnar tables (`CREATE TABLE ... COLUMNAR`) store data per-column; queries auto-materialize to rows

### Scripting Architecture

The `ScriptEngine` (script.h) breaks the dependency cycle with `Database` via a callback (`SqlExecuteFn`). Scripts access the database through a `db` global table:
- **Lua**: `db.execute(sql)`, `db.query(sql)`, `db.now()`, `db.user()`, `db.log(msg)`
- **JavaScript** (TDB_WITH_JSC): Same `db.*` plus `db.tables()`, `db.views()`, `db.indexes()`, `db.tablespaces()`, `db.describe(table)`, `db.catalog`

The LLVM JIT (TDB_WITH_LLVM) compiles Lua 5.4 bytecodes to LLVM IR via `IREmitter`, then to native code via ORC LLJIT. Tiered execution: interpreter first, JIT after 3 calls (AUTO mode). Falls back to interpreter for unsupported opcodes (closures, varargs, metamethods, generic for).
