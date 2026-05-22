# Getting Started

## Requirements

- CMake 3.20+
- C11 compiler (gcc, clang)
- C++20 compiler (g++ 10+, clang++ 13+, Apple Clang 14+)
- POSIX-compatible OS (macOS, Linux)

### Optional Dependencies

| Dependency | CMake Flag | Purpose |
|-----------|-----------|---------|
| LLVM 18+ | `TDB_WITH_LLVM=ON` | Lua-to-native JIT compiler |
| JavaScriptCore | `TDB_WITH_JSC=ON` | JavaScript scripting (macOS only) |
| zstd | `TDB_WITH_ZSTD=ON` | Page-level compression |
| OpenSSL | `TDB_WITH_OPENSSL=ON` | Hardware-accelerated AES-256-GCM |
| ICU | `TDB_WITH_ICU=ON` | Full Unicode case folding |

## Building

```bash
git clone <repository-url>
cd tdb
mkdir build && cd build
cmake .. -DTDB_BUILD_TESTS=OFF
cmake --build . --target tdbcli --target tdb-tui -j8
```

### Build Configurations

```bash
# Debug (default)
cmake .. -DCMAKE_BUILD_TYPE=Debug    # -g -O0 -DTDB_DEBUG

# Release
cmake .. -DCMAKE_BUILD_TYPE=Release  # -O2 -DNDEBUG

# With all optional features (macOS)
cmake .. -DTDB_WITH_LLVM=ON -DTDB_WITH_JSC=ON -DTDB_WITH_ZSTD=ON
```

## First Steps

### Interactive Mode

```bash
./tdbcli
```

```sql
tdb> CREATE TABLE users (
  id INTEGER PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(100) NOT NULL,
  email VARCHAR(255) UNIQUE,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

tdb> INSERT INTO users (name, email) VALUES
  ('Alice', 'alice@example.com'),
  ('Bob', 'bob@example.com');

tdb> SELECT * FROM users;
id | name  | email            | created_at
---+-------+------------------+-------------------
1  | Alice | alice@example.com | 2026-05-22 10:30:00
2  | Bob   | bob@example.com  | 2026-05-22 10:30:00
(2 rows)
```

### File Mode

```bash
# Execute a SQL file
./tdbcli -f script.sql

# Execute a single statement
./tdbcli -e "SELECT 1 + 1 AS result"

# Named database (auto-saves on exit)
./tdbcli mydb.tdb
./tdbcli mydb.tdb -f setup.sql
```

### Sample Database

TDB ships with a comprehensive ERP/CRM sample database:

```bash
./tdbcli -f examples/erp_sample.sql
```

This creates 22 tables, 5 views, 3 materialized views, 16 indexes, 3 sequences, and 330+ rows demonstrating all major features.

## Persistence

TDB uses a binary `.tdb` file format for persistence:

```bash
# Start with a named database
./tdbcli mydb.tdb

# Save interactively
tdb> \save mydb.tdb

# Pack into optimized format
tdb> \pack mydb.tdb optimized.tdb
```

When started with a path argument, the database auto-saves on exit.
