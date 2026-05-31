# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

TransactionDB (`tdb`) is a lightweight, embeddable, single-file SQL database engine written in
**C11**, with a header-only **C++17** RAII wrapper. The public C API deliberately mirrors SQLite's
ergonomics (`open` / `prepare` / `step` / `bind` / `column` / `finalize`) — when in doubt about
intended semantics of a public function, the SQLite function of the same name is the reference.

The two public headers are the contract:
- `include/transactiondb.h` — the C API (status codes `TDB_*`, opaque `tdb_db`/`tdb_stmt`/`tdb_value`).
- `include/transactiondb.hpp` — `tdb::Database` / `tdb::Statement`, move-only, throws `tdb::Error`.

## Build / test / run

CMake (≥ 3.16), out-of-source build in `build/` (gitignored). Default build type is `RelWithDebInfo`.

```sh
cmake -S . -B build              # configure
cmake --build build -j           # build lib + CLI + tests

ctest --test-dir build --output-on-failure   # run all tests
ctest --test-dir build -R mvcc               # run one test (by CTest name, see below)
./build/test_mvcc                            # run a test binary directly (most verbose)

./build/tdb_shell [path|:memory:]            # interactive REPL (defaults to :memory:)
```

Useful configure options (all default `ON`): `-DTDB_BUILD_TESTS=OFF`, `-DTDB_BUILD_CLI=OFF`,
`-DTDB_BUILD_LUA=OFF`. **`TDB_BUILD_LUA=ON` fetches Lua 5.4.7 over the network** via
`FetchContent` (see `cmake/FetchLua.cmake`); for offline builds either drop a copy under
`third_party/lua/` or configure with `-DTDB_BUILD_LUA=OFF`. Lua-gated code is guarded by the
`TDB_HAVE_LUA` macro.

There is **no separate lint step and no `.clang-format`/`.clang-tidy`** — the "lint" is a strict
warning set (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`, MSVC `/W4`)
applied to first-party targets only via `tdb_set_warnings()` (`cmake/CompilerWarnings.cmake`).
Treat new warnings as failures; vendored Lua is intentionally built with warnings off.

## Layered architecture

The engine is strictly layered; higher layers go through the layer below them, never around it.
Bottom to top:

- **`src/common/`** — foundation: arena/heap allocation (`tdb_mem`), growable buffers, mutexes,
  status strings, and shared typedefs (`tdb_txnid`, `tdb_rowid`, …) in `tdb_internal.h`.
- **`src/storage/`** — the on-disk substrate. `tdb_format.h` fixes the file layout (single file,
  4096-byte big-endian pages, 100-byte header on page 1, catalog B-tree root at page 1). `pager`
  caches pages; `wal` provides durability (committed page images appended, later checkpointed);
  `btree` is the index structure. **All table data access goes through the storage vtable in
  `tdb_storage.h`** (`tdb_storage_vtab`), implemented today by the row engine `engine_row.c`.
  The executor talks only to this vtable — never to the b-tree or pager directly — so an
  alternative (e.g. columnar) engine can be dropped in. Scans/seeks return only MVCC-visible rows.
- **`src/txn/`** — concurrency. `tdb_txn` is the transaction manager (monotonic, persisted xids;
  **single-writer model**: one read-write txn mutates the pager while any number of snapshots read
  concurrently). `tdb_mvcc` decides version visibility from `(xmin, xmax)` against a snapshot and
  is deliberately decoupled from the txn manager (commit-state supplied by callback, so it's unit
  testable in isolation). `tdb_lock` is the lock manager. Isolation levels: `READ_COMMITTED`,
  `SNAPSHOT` (default), `SERIALIZABLE`.
- **`src/catalog/`** — the system catalog (SQLite's `sqlite_master` analogue): schema objects are
  serialized as rows in a dedicated catalog b-tree and cached in memory on open. `tdb_schema.h`
  defines the in-memory `tdb_table`/`tdb_col`/`tdb_index` structs.
- **`src/value/`** — the dynamic value type (`tdb_value`), row/record encoding (`tdb_record`), and
  SQL type ids/names (`tdb_sqltype`).
- **`src/sql/`** — the query pipeline: `tdb_lexer` → `tdb_parser` → AST (`tdb_ast`) →
  `tdb_analyze` (name resolution / semantic analysis) → `tdb_exec` (executor). DML and SELECT
  execution bodies live in `tdb_exec_dml.inc` / `tdb_exec_select.inc`, `#include`d into
  `tdb_exec.c` (they are not separately compiled). Most SELECTs **materialize** their result set
  (full cross-product join → filter → group → project → sort → limit). A single base-table scan
  with an optional WHERE / ORDER BY / projection / LIMIT instead runs through a pull-based **volcano
  operator tree** (`tdb_exec_stream.inc`: Scan → Filter → Sort → Project → Limit) that `tdb_step()`
  pulls one row at a time, holding a statement-owned read snapshot open across steps; the Sort
  operator becomes a bounded top-N heap when ORDER BY is paired with LIMIT. Anything more complex
  falls back to materialization. Correlated subqueries are supported (unbound columns resolve outward
  through enclosing query contexts, re-run per outer row).
- **`src/api/`** — `tdb_api.c` implements the public C API over everything above. `tdb_db.h`
  (internal) defines `struct tdb_db` (the connection: pager, catalog, lockmgr, txnmgr, storage
  engine, optional lua, current txn, autocommit flag) and `struct tdb_stmt` (a prepared statement
  with its arena, AST, bound params, and materialized result set).
- **`src/lua/`** — optional embedded Lua integration (compiled only when `TDB_BUILD_LUA`).
- **`cli/shell.c`** — the `tdb_shell` REPL. As a first-party tool it intentionally reaches into
  internal headers (e.g. for `.tables` / `.schema` introspection).

## Conventions

- All public symbols are prefixed `tdb_`; status codes are `TDB_*` with `TDB_OK == 0`, and
  `tdb_step()` returns `TDB_ROW` (101) / `TDB_DONE` (100). Functions return a status `int` rather
  than throwing/erroring; the C++ wrapper converts non-OK codes into `tdb::Error`.
- Headers are included by basename (e.g. `#include "tdb_db.h"`) because every `src/` subdirectory
  is placed on the include path per target in `CMakeLists.txt`. When you add a new target or a new
  subdirectory of sources, add the matching `target_include_directories` entries.
- Library sources are auto-discovered via `file(GLOB_RECURSE … CONFIGURE_DEPENDS src/*.c)`, so a
  new `src/**/foo.c` is picked up on the next configure with no CMake edit. **Tests are not
  globbed**: to add `tests/test_<name>.c`, append `<name>` to the `TDB_TESTS` list in
  `CMakeLists.txt` (the CTest case is registered as `<name>`, the binary as `test_<name>`).
- Tests use the tiny built-in harness in `tests/tdb_test.h` (no GoogleTest/Catch2): write
  `TDB_CHECK` / `TDB_CHECK_EQ` / `TDB_CHECK_STR` assertions, list cases in a table, and end with
  `TDB_MAIN(cases)`. Failures print `file:line` and continue; non-zero exit means a failure.
- Code is organized around an explicit, phased roadmap — comments reference "Phase N" and call out
  features left for later. Prefer extending the established layer boundaries (especially the
  storage vtable and the MVCC/visibility split) over short-cutting across them.
