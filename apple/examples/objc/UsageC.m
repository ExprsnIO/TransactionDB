// Using the TransactionDB C API from Objective-C.
//
// Build as a normal .m (Objective-C) file. Only the C API is visible here; for
// the tdb::* C++ wrapper, use an Objective-C++ (.mm) file instead — see
// ../objcpp/UsageCxx.mm.

#import <Foundation/Foundation.h>
#import <TransactionDB/TransactionDB.h>

void TDBRunCExample(void) {
    tdb_db *db = NULL;
    if (tdb_open(":memory:", &db) != TDB_OK) {
        NSLog(@"open failed");
        return;
    }

    if (tdb_exec(db, "CREATE TABLE t(id INTEGER, name TEXT)", NULL, NULL, NULL) != TDB_OK) {
        NSLog(@"create failed: %s", tdb_errmsg(db));
        tdb_close(db);
        return;
    }

    tdb_stmt *ins = NULL;
    tdb_prepare_v2(db, "INSERT INTO t(id, name) VALUES (?, ?)", -1, &ins, NULL);
    tdb_bind_int64(ins, 1, 1);          // 1-based param index
    tdb_bind_text(ins, 2, "hello", -1); // n = -1 => NUL-terminated
    tdb_step(ins);
    tdb_finalize(ins);

    tdb_stmt *q = NULL;
    tdb_prepare_v2(db, "SELECT id, name FROM t", -1, &q, NULL);
    while (tdb_step(q) == TDB_ROW) {    // 0-based column index
        int64_t id = tdb_column_int64(q, 0);
        const char *name = tdb_column_text(q, 1);
        NSLog(@"row: %lld %s", (long long)id, name);
    }
    tdb_finalize(q);

    tdb_close(db);
}
