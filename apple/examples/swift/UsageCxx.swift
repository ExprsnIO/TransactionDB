// Using the TransactionDB C++ API (tdb::Database / tdb::Statement) from Swift.
//
// Requires Swift 5.9+ with C++ interoperability enabled. The C++ wrapper lives
// in the framework's `CXX` submodule (guarded by `requires cplusplus` in the
// module map), so it only resolves when interop is on.
//
// Enable interop one of these ways:
//   * SwiftPM target setting:  swiftSettings: [.interoperabilityMode(.Cxx)]
//   * Xcode build setting:     C++ and Objective-C Interoperability = C++/ObjC++
//                              (SWIFT_OBJC_INTEROP_MODE = objcxx)
//   * swiftc flag:             -cxx-interoperability-mode=default
//
// With interop on, `import TransactionDB` also surfaces the `tdb` namespace.

import TransactionDB

func runCxxExample() {
    // tdb::Database's constructor takes a std::string; bridge from Swift.
    var db = tdb.Database(std.string(":memory:"), TDB_OPEN_READWRITE.rawValue | TDB_OPEN_CREATE.rawValue)

    db.exec(std.string("CREATE TABLE t(id INTEGER, name TEXT)"))

    var ins = db.prepare(std.string("INSERT INTO t(id, name) VALUES (?, ?)"))
    ins.bind(1, Int64(7))            // -> bind(int, int64_t)
    ins.bind(2, std.string("seven")) // -> bind(int, std::string_view)
    _ = ins.step()

    var q = db.prepare(std.string("SELECT id, name FROM t"))
    while q.step() {
        let id = q.getInt(0)                 // std::int64_t -> Int64
        let name = String(q.getText(1))      // std::string  -> String
        print("row: \(id) \(name)")
    }
}
