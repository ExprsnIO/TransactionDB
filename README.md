# TransactionDB

**TransactionDB** (`tdb`) is a lightweight, embeddable, single-file SQL database engine
written in **C11**, with a header-only **C++17** RAII wrapper. Its public C API
deliberately mirrors [SQLite](https://sqlite.org)'s ergonomics
(`open` / `prepare` / `step` / `bind` / `column` / `finalize`), so if you've used SQLite
before you already know most of the surface.

At its core, TransactionDB is a **multi-version concurrency control (MVCC)** engine: a
single read-write transaction can mutate the database while any number of snapshot
readers proceed concurrently without blocking.

> **Status:** early development (version 0.1.0). The engine is organized around an
> explicit, phased roadmap; expect rough edges and an evolving feature set.

## Features

- **Single-file database** — one file on disk (4096-byte pages, big-endian), plus an
  in-memory mode (`:memory:`).
- **SQLite-like C API** — opaque `tdb_db` / `tdb_stmt` / `tdb_value` handles, integer
  status codes (`TDB_OK == 0`), and the familiar prepare/step/finalize lifecycle.
- **Header-only C++17 wrapper** — move-only `tdb::Database` / `tdb::Statement` types that
  convert error codes into `tdb::Error` exceptions (RAII cleanup).
- **MVCC & transactions** — single-writer / multi-reader concurrency with snapshot
  visibility decided from per-row `(xmin, xmax)` versions.
- **Configurable isolation** — `READ COMMITTED`, `SNAPSHOT` (default), and `SERIALIZABLE`.
- **Durability** — a write-ahead log (WAL) appends committed page images and later
  checkpoints them back into the main file.
- **Pluggable storage** — all table access goes through a storage vtable, so an
  alternative engine (e.g. columnar) can be dropped in behind the same executor.
- **Streaming execution** — simple single-table scans run through a pull-based
  (volcano) operator tree; more complex queries fall back to materialization.
- **Optional embedded Lua** — Lua 5.4 scripting support, gated behind a build option.
- **Interactive shell** — a small REPL (`tdb_shell`) for ad-hoc SQL.

### SQL support

The SQL surface is intentionally a subset and growing. Currently recognized constructs
include:

- `CREATE TABLE` / `DROP TABLE`, `CREATE INDEX`
- `INSERT INTO ... VALUES`, `UPDATE ... SET`, `DELETE`
- `SELECT` with `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`, `DISTINCT`, joins,
  and the `COUNT` / `SUM` / `AVG` / `MIN` / `MAX` aggregates
- Correlated subqueries
- `BEGIN` / `COMMIT` / `ROLLBACK`
- Types: `INTEGER`, `REAL`, `TEXT`, `BLOB`, with `PRIMARY KEY`

For a feature-by-feature comparison against SQLite — including what's supported,
what's parsed-but-not-yet-enforced, and tdb-only extensions — see
[`docs/SQL_COMPATIBILITY.md`](docs/SQL_COMPATIBILITY.md).

## Building

TransactionDB uses **CMake (≥ 3.16)** with an out-of-source build. The default build
type is `RelWithDebInfo`.

```sh
cmake -S . -B build              # configure
cmake --build build -j           # build the library, CLI, and tests
```

### Configure options

All default to `ON`:

| Option             | Description                                              |
| ------------------ | -------------------------------------------------------- |
| `TDB_BUILD_TESTS`  | Build the test suite.                                    |
| `TDB_BUILD_CLI`    | Build the `tdb_shell` REPL.                              |
| `TDB_BUILD_LUA`    | Build with embedded Lua scripting support.               |

> ⚠️ **`TDB_BUILD_LUA=ON` fetches Lua 5.4.7 over the network** via CMake `FetchContent`.
> For offline builds, drop a copy under `third_party/lua/` or configure with
> `-DTDB_BUILD_LUA=OFF`.

Example offline configure:

```sh
cmake -S . -B build -DTDB_BUILD_LUA=OFF
```

## Testing

Tests run via CTest:

```sh
ctest --test-dir build --output-on-failure   # run everything
ctest --test-dir build -R mvcc               # run one test by name
./build/test_mvcc                            # run a test binary directly (most verbose)
```

Tests use a tiny built-in harness (`tests/tdb_test.h`) — no GoogleTest/Catch2.

## The shell

```sh
./build/tdb_shell [path|:memory:]   # interactive REPL (defaults to :memory:)
```

Dot-commands: `.tables`, `.schema`, `.help`, `.quit` / `.exit`.

## Usage

### C

```c
#include "transactiondb.h"
#include <stdio.h>

int main(void) {
    tdb_db *db;
    tdb_open(":memory:", &db);

    tdb_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)",
             NULL, NULL, NULL);
    tdb_exec(db, "INSERT INTO users VALUES (1, 'Ada')", NULL, NULL, NULL);

    tdb_stmt *stmt;
    tdb_prepare_v2(db, "SELECT id, name FROM users WHERE id = ?", -1, &stmt, NULL);
    tdb_bind_int(stmt, 1, 1);          /* parameters are 1-based */

    while (tdb_step(stmt) == TDB_ROW) {
        printf("%lld -> %s\n",
               (long long)tdb_column_int(stmt, 0),   /* columns are 0-based */
               tdb_column_text(stmt, 1));
    }

    tdb_finalize(stmt);
    tdb_close(db);
    return 0;
}
```

### C++

```cpp
#include "transactiondb.hpp"
#include <iostream>

int main() {
    tdb::Database db(":memory:");
    db.exec("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)");
    db.exec("INSERT INTO users VALUES (1, 'Ada')");

    auto stmt = db.prepare("SELECT id, name FROM users WHERE id = ?");
    stmt.bind(1, 1);   // binds are chainable and 1-based
    while (stmt.step()) {
        std::cout << stmt.getInt(0) << " -> " << stmt.getText(1) << "\n";
    }
}
```

Errors surface as `tdb::Error` (carrying the underlying `TDB_*` code via `.code()`);
handles are cleaned up automatically by RAII.

## Architecture

The engine is **strictly layered** — higher layers always go through the layer below,
never around it. Bottom to top:

| Layer          | Directory       | Responsibility                                                              |
| -------------- | --------------- | -------------------------------------------------------------------------- |
| **Foundation** | `src/common/`   | Arena/heap allocation, growable buffers, mutexes, status strings, typedefs. |
| **Storage**    | `src/storage/`  | On-disk format, pager (page cache), WAL (durability), B-tree, storage vtable. |
| **Concurrency**| `src/txn/`      | Transaction manager, MVCC visibility, lock manager.                         |
| **Catalog**    | `src/catalog/`  | System catalog (schema objects serialized into a dedicated B-tree).         |
| **Value**      | `src/value/`    | Dynamic value type, row/record encoding, SQL type ids.                      |
| **SQL**        | `src/sql/`      | Lexer → parser → AST → analyzer → executor.                                 |
| **API**        | `src/api/`      | The public C API tying everything together (the connection object).        |
| **Lua**        | `src/lua/`      | Optional embedded Lua integration (compiled only with `TDB_BUILD_LUA`).     |
| **CLI**        | `cli/`          | The `tdb_shell` REPL.                                                       |

The two public headers in `include/` are the contract:

- `include/transactiondb.h` — the C API.
- `include/transactiondb.hpp` — the header-only C++17 wrapper.

For a deeper tour of the internals and conventions, see [`CLAUDE.md`](CLAUDE.md).

## License

Licensed under the [Apache License 2.0](LICENSE).
