# API Reference

## Embedding TDB

TDB can be embedded in C++ applications as a static library.

### C++ API

```cpp
#include "tdb/database.h"

int main() {
    tdb::Database db;

    // Open (empty string = in-memory)
    db.open("mydb.tdb");

    // Execute SQL
    auto result = db.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT)");
    if (!result.success) {
        std::cerr << result.error_message << std::endl;
        return 1;
    }

    // Insert data
    db.execute("INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')");

    // Query
    auto rs = db.execute("SELECT * FROM users ORDER BY id");
    for (auto &row : rs.rows) {
        // row is a std::vector<sql::Value>
        std::cout << row[0].int_val << ": " << row[1].str_val << std::endl;
    }

    // Batch execution (multiple statements)
    auto results = db.execute_batch(
        "INSERT INTO users VALUES (3, 'Carol');"
        "INSERT INTO users VALUES (4, 'Dave');"
    );

    // Save and close
    db.save();
    db.close();
}
```

### ResultSet

```cpp
struct ResultSet {
    bool success;
    std::string error_message;
    Schema columns;              // vector of {name, table}
    std::vector<Tuple> rows;     // Tuple = vector<Value>
    int64_t rows_affected;
};
```

### Value Type System

```cpp
struct Value {
    enum class Type {
        NULL_VAL, INT64, FLOAT64, STRING, BOOL, BLOB,
        DATE_VAL, TIME_VAL, TIMESTAMP_VAL,
        DECIMAL, UUID, VARBINARY, INTERVAL,
        ENUM_VAL, BIT_VAL, JSON_VAL, XML_VAL,
        COMPOSITE, TIMESTAMP_TZ, GEOMETRY,
        ARRAY, MULTISET
    };

    Type type;
    int64_t int_val;
    double float_val;
    std::string str_val;
    bool bool_val;
    std::shared_ptr<std::vector<Value>> composite_fields;

    // Factory methods
    static Value make_null();
    static Value make_int(int64_t v);
    static Value make_float(double v);
    static Value make_string(std::string v);
    static Value make_bool(bool v);
    static Value make_date(int year, int month, int day);
    static Value make_timestamp(int y, int m, int d, int h, int min, int sec);
    static Value make_json(std::string text);
    static Value make_array(std::vector<Value> elements);
    // ... and more for each type
};
```

### Catalog Access

```cpp
auto &catalog = db.catalog();

// List objects
auto tables = catalog.list_tables();
auto views = catalog.list_views();
auto indexes = catalog.list_indexes();

// Find objects
auto *table = catalog.find_table("users");
if (table) {
    for (auto &col : table->columns) {
        std::cout << col.name << " " << col.type.name << std::endl;
    }
}

// Sequences
int64_t next = catalog.nextval("my_sequence");
```

### Thread Safety

```cpp
#include "tdb/threadsafe.h"

tdb::ThreadSafeDatabase db;
db.open("mydb.tdb");

// Thread-safe execute (internally mutex-protected)
auto result = db.execute("SELECT * FROM users");
```

---

## Lua Scripting

### ScriptEngine API

```cpp
#include "tdb/script.h"

tdb::script::ScriptEngineOptions opts;
opts.execute_fn = [&db](const std::string &sql) {
    return db.execute(sql);
};

tdb::script::ScriptEngine engine(opts);

// Execute Lua code
auto result = engine.eval("return 1 + 2");
// result.int_val == 3

// Call a stored procedure
auto *script = catalog.find_script("my_proc");
if (script) {
    auto rv = engine.call(*script, {Value::make_int(42)});
}
```

### Lua Global: db

| Function | Signature | Description |
|----------|-----------|-------------|
| `db.execute` | `db.execute(sql) -> rows_affected` | Execute SQL, return count |
| `db.query` | `db.query(sql) -> [{col=val,...},...]` | Execute SQL, return rows |
| `db.now` | `db.now() -> milliseconds` | Current epoch time |
| `db.user` | `db.user() -> string` | Current user name |
| `db.log` | `db.log(msg)` | Log a message |

### Lua Trigger Context

```lua
-- Available in trigger scripts
trigger.timing    -- "BEFORE" or "AFTER"
trigger.event     -- "INSERT", "UPDATE", or "DELETE"
trigger.table     -- table name
trigger.new       -- new row as {column = value, ...}
trigger.old       -- old row (UPDATE/DELETE only)
trigger.columns   -- array of column names
```

### LLVM JIT Compilation

When built with `TDB_WITH_LLVM=ON`, the ScriptEngine automatically JIT-compiles Lua functions after they've been called 3 times (configurable via `JITMode::AUTO`).

```cpp
engine.set_jit_mode(tdb::script::JITMode::ON);   // always JIT
engine.set_jit_mode(tdb::script::JITMode::OFF);  // never JIT
engine.set_jit_mode(tdb::script::JITMode::AUTO); // JIT after threshold
```

Supported Lua opcodes for JIT: arithmetic, comparisons, branches, numeric for loops, bitwise operations, loads/stores, returns. Unsupported opcodes (closures, varargs, metamethods, generic for) cause graceful fallback to the interpreter.

---

## JavaScript Engine

When built with `TDB_WITH_JSC=ON` (macOS only), the JSEngine provides JavaScript scripting via Apple's JavaScriptCore framework, which includes a production-grade JIT compiler (DFG + FTL/B3).

### JavaScript Global: db

| Function | Description |
|----------|-------------|
| `db.execute(sql)` | Execute SQL, returns `{rowsAffected, success}` |
| `db.query(sql)` | Execute SQL, returns array of row objects |
| `db.now()` | Current epoch milliseconds |
| `db.user()` | Current user name |
| `db.log(msg)` | Log a message |
| `db.tables()` | List all table names |
| `db.views()` | List all view names |
| `db.indexes()` | List all index names |
| `db.sequences()` | List all sequence names |
| `db.tablespaces()` | List all tablespace names |
| `db.describe(name)` | Describe table columns |
| `db.catalog` | Full catalog object `{tables, views, indexes, ...}` |

```javascript
// JavaScript example
let users = db.query("SELECT * FROM users WHERE active = TRUE");
for (let user of users) {
    console.log(user.name + ": " + user.email);
}

let tables = db.tables();
for (let t of tables) {
    let cols = db.describe(t);
    console.log(t + ": " + cols.length + " columns");
}
```

---

## Objective-C Framework (macOS/iOS)

TDB includes an Objective-C framework for Apple platforms:

```objc
#import <TDB/TDB.h>

TDBDatabase *db = [[TDBDatabase alloc] init];
[db open:@"mydb.tdb"];

TDBResultSet *rs = [db execute:@"SELECT * FROM users"];
for (NSDictionary *row in rs.rows) {
    NSLog(@"%@ - %@", row[@"name"], row[@"email"]);
}
```

Classes: `TDBDatabase`, `TDBResultSet`, `TDBValue`, `TDBCrypto`, `TDBKeychain`.

---

## CMake Integration

```cmake
find_package(tdb REQUIRED)
target_link_libraries(myapp PRIVATE tdb)
```

Or as a subdirectory:

```cmake
add_subdirectory(vendor/tdb)
target_link_libraries(myapp PRIVATE tdb)
```

### pkg-config

```bash
pkg-config --cflags --libs tdb
```
