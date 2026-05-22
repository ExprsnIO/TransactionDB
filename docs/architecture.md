# Architecture

## Overview

TDB is a multi-model SQL database engine with 11 libraries organized in a layered architecture. The core is written in C11 for portability, with the SQL engine and catalog in C++20 for expressiveness.

```
                          +-----------+
                          |    tdb    |  Top-level facade
                          +-----+-----+
                                |
          +--------+--------+---+---+--------+-----------+
          |        |        |       |        |           |
      tdb_sql  tdb_catalog tdb_doc tdb_script tdb_persist tdb_core
          |        |                |           |           |
          |        |          +-----+-----+     |       tdb_index
          |        |          |     |     |     |
          |        |       tdb_lua tdb_jit tdb_jsc
          |        |
      (lexer, parser, AST,    (tables, views,
       optimizer, executor,    indexes, sequences,
       type coercion)          partitions, privileges)
```

## Library Details

### tdb_core (C11)

Low-level storage engine:
- **page.c** -- 8KB page management with header/slot/tuple layout
- **storage.c** -- File I/O abstraction, page read/write
- **buffer.c** -- Buffer pool with LRU eviction, pin/unpin, dirty tracking
- **wal.c** -- Write-ahead log with LSN tracking, checkpointing, crash recovery
- **txn.c** -- Transaction manager, isolation levels, lock tables
- **crypto.c** -- AES-256-GCM encryption/decryption, key management

### tdb_index (C11)

Five index implementations:
- **bptree.c** -- B+ Tree (leaf-linked, range queries, default for CREATE INDEX)
- **btree.c** -- B-Tree (traditional balanced tree)
- **rtree.c** -- R-Tree (spatial data, 2D bounding box queries)
- **rptree.c** -- R+ Tree (non-overlapping spatial index, used for bbox prefiltering)
- **hash_index.c** -- Hash index (O(1) equality lookups)

### tdb_sql (C++20)

SQL processing pipeline:
- **lexer.cpp** -- Tokenizer with 310+ keywords, string/number/blob literals
- **parser.cpp** -- Recursive descent parser producing typed AST nodes
- **ast.h / ast.cpp** -- 53 statement types, 16 expression types, full SQL grammar
- **optimizer.cpp** -- Cost-based query optimizer (predicate pushdown, join reordering)
- **executor.cpp** -- Plan-based executor with WindowExec, SortExec operators
- **typecoerce.cpp** -- Implicit/explicit type conversion (date parsing, numeric promotion)

### tdb_catalog (C++20)

Schema metadata and row storage:
- **TableInfo** -- columns, constraints, rows, partitions, columnar data
- **ViewInfo** -- stored query, re-executed on each SELECT
- **MaterializedViewInfo** -- cached query results, REFRESH support
- **IndexInfo** -- index metadata (method, columns, uniqueness)
- **SequenceInfo** -- auto-increment state (current_value, increment, bounds)
- **ScriptInfo** -- stored procedure/function source code and parameters
- **TriggerInfo** -- trigger metadata (timing, event, table, script reference)
- **Privilege** -- GRANT/REVOKE access control entries

### tdb_script (C++20)

Multi-language scripting:
- **ScriptEngine** -- Lua 5.4 interpreter with db.* API bridge
- **LuaJIT** (optional) -- Lua bytecode to LLVM IR compiler via IREmitter
- **JSEngine** (optional) -- JavaScriptCore integration with full catalog access

### tdb_persistence (C++20)

Bridge between C storage layer and C++ Database class:
- Binary serialization/deserialization of catalog objects
- .tdb file format (v1 and v2) with magic numbers, checksums

## SQL Execution Flow

```
1. SQL Text
   |
2. Lexer (lexer.cpp)
   | tokenize into TokenType stream
3. Parser (parser.cpp)
   | recursive descent -> AST (ast.h)
4. Database::execute_stmt() (database.h)
   | dispatch by StmtType
   |
   +-- DDL: exec_create_table, exec_alter_table, exec_drop, ...
   |   modify Catalog directly
   |
   +-- DML: exec_insert, exec_select, exec_update, exec_delete, exec_merge
   |   |
   |   +-- resolve_from() -> resolve tables, views, CTEs, subqueries
   |   +-- eval_predicate() -> WHERE filtering
   |   +-- eval_expr_with_row() -> expression evaluation (200+ functions)
   |   +-- compute_aggregate() -> GROUP BY aggregation
   |   +-- window function post-processing
   |   +-- sort_results() -> ORDER BY
   |   +-- trigger firing (BEFORE/AFTER)
   |   +-- constraint checking (PK, FK, UNIQUE, CHECK, DOMAIN)
   |   +-- RETURNING clause evaluation
   |
   +-- TCL: exec_begin, exec_commit, exec_rollback, exec_savepoint
   |
   +-- Procedural: exec_call -> exec_plsql_body -> plsql_exec_block
       | PL/SQL interpreter: tokenize, IF/FOR/WHILE, variable subst
```

## Key Design Decisions

### Header-Only Database Class

`database.h` (~6600 lines) is a header-only implementation. This simplifies the build (no separate compilation unit) but means changes trigger recompilation of all consumers. The monolithic design keeps all execution logic in one place for debuggability.

### In-Memory Row Storage

Rows are stored in `std::vector<Tuple>` inside `TableInfo`. This provides:
- Zero-copy access during query execution
- Simple transaction snapshots via vector copy
- Efficient sequential scans

Partitioned tables store rows in per-partition vectors. Columnar tables store values in per-column vectors with on-demand row materialization.

### Expression Evaluation

`eval_expr_with_row()` is a single recursive function handling all expression types. Functions are dispatched via `if (upper == "NAME")` chains. This is simple but O(n) in the number of functions. The function count (~200) makes this negligible vs. I/O cost.

### Correlated Subquery Scoping

The `outer_scopes_` stack enables correlated subqueries. Each `ScopePush` RAII guard pushes the current row/schema onto the stack. Column resolution in `eval_expr_with_row` falls through to outer scopes when the column isn't found in the current schema. Table-qualified references (`t1.col`) skip unqualified fallback to prevent false matches.

### PL/SQL Variable Substitution

The `plsql_subst_vars()` function performs single-pass, word-boundary-aware replacement. Variables are sorted by name length (longest first) to prevent partial matches. STRING values are automatically quoted as `'value'` for SQL safety. The `is_null()` check on execute results prevents SQL NULLs from overwriting valid variable assignments.

## File Format

TDB uses a custom binary format (`.tdb`):

- **v1** -- Simple sequential catalog dump
- **v2** -- Optimized with page-aligned sections, checksums, and compression support

The `dbfile.cpp` module handles both formats with auto-detection on load.

## Extending TDB

### Adding a New Function

1. In `database.h`, find `eval_expr_with_row()` around the function dispatch section
2. Add: `if (upper == "MY_FUNC" && vals.size() >= N) { ... }`
3. Return the result as `sql::Value::make_int/float/string/bool(...)`

### Adding a New Statement

1. Add `MY_STMT` to `StmtType` enum in `ast.h`
2. Create `MyStmt` struct in `ast.h`
3. Add to `Statement::data` variant
4. Add name in `ast.cpp` `stmt_type_name()`
5. Add parser dispatch in `parser.cpp` `parse_statement()`
6. Implement `parse_my_stmt()` in `parser.cpp`, declare in `parser.h`
7. Add `case ST::MY_STMT:` in `database.h` `execute_stmt()`
8. Implement `exec_my_stmt()` in `database.h`

### Adding a New Index Type

1. Implement the index in `src/index/myindex/myindex.c`
2. Add `MYINDEX` to `IndexMethod` enum in `ast.h`
3. Add keyword to `token.h` and `lexer.cpp`
4. Handle in `parse_index_method()` in `parser.cpp`
5. Wire into query execution in `database.h` WHERE filtering
