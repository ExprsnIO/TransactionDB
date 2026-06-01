// Using the TransactionDB C API from Swift.
//
// Plain Swift (no C++ interop needed): `import TransactionDB` brings in the C
// API declared in transactiondb.h. Symbols come through as their C names
// (tdb_open, tdb_prepare_v2, ...).

import TransactionDB

enum DBError: Error { case message(String) }

func runCExample() throws {
    var db: OpaquePointer? = nil
    guard tdb_open(":memory:", &db) == TDB_OK.rawValue else {
        throw DBError.message("open failed")
    }
    defer { tdb_close(db) }

    if tdb_exec(db, "CREATE TABLE t(id INTEGER, name TEXT)", nil, nil, nil) != TDB_OK.rawValue {
        throw DBError.message(String(cString: tdb_errmsg(db)))
    }

    // Parameterised insert (bind params are 1-based).
    var stmt: OpaquePointer? = nil
    guard tdb_prepare_v2(db, "INSERT INTO t(id, name) VALUES (?, ?)", -1, &stmt, nil) == TDB_OK.rawValue else {
        throw DBError.message(String(cString: tdb_errmsg(db)))
    }
    tdb_bind_int64(stmt, 1, 42)
    "answer".withCString { c in
        // n = -1 means "NUL-terminated".
        _ = tdb_bind_text(stmt, 2, c, -1)
    }
    _ = tdb_step(stmt)
    tdb_finalize(stmt)

    // Query back (result columns are 0-based).
    var q: OpaquePointer? = nil
    _ = tdb_prepare_v2(db, "SELECT id, name FROM t", -1, &q, nil)
    defer { tdb_finalize(q) }
    while tdb_step(q) == TDB_ROW.rawValue {
        let id = tdb_column_int64(q, 0)
        let name = String(cString: tdb_column_text(q, 1))
        print("row: \(id) \(name)")
    }
}
