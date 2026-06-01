// Using the TransactionDB C++ API (tdb::Database / tdb::Statement) from
// Objective-C++.
//
// This file must have the .mm extension so the compiler treats it as
// Objective-C++ and the `requires cplusplus` CXX submodule becomes available.

#import <Foundation/Foundation.h>
#import <TransactionDB/transactiondb.hpp>

void TDBRunCxxExample(void) {
    try {
        tdb::Database db(":memory:");
        db.exec("CREATE TABLE t(id INTEGER, name TEXT)");

        tdb::Statement ins = db.prepare("INSERT INTO t(id, name) VALUES (?, ?)");
        ins.bind(1, (int64_t)99);
        ins.bind(2, "ninety-nine");
        ins.step();

        tdb::Statement q = db.prepare("SELECT id, name FROM t");
        while (q.step()) {
            NSLog(@"row: %lld %s",
                  (long long)q.getInt(0),
                  q.getText(1).c_str());
        }
    } catch (const tdb::Error &e) {
        NSLog(@"tdb error %d: %s", e.code, e.what());
    }
}
