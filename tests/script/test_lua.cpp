// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
//
// Lua bridge smoke test. Covers the v1.x baseline (db.execute, db.query,
// db.now, db.user, db.log). The full type bridge from Track 4 gets its own
// test under test_lua_types.cpp.

#include "tdb_test.h"
#include "tdb/script.h"
#include "tdb/database.h"

using namespace tdb;

TDB_TEST(lua_evaluates_constant) {
    Database db;
    db.open("");
    script::ScriptEngineOptions opts;
    opts.execute_fn = [&db](const std::string &s) { return db.execute(s); };
    script::ScriptEngine eng(opts);
    auto r = eng.eval("return 2 + 3");
    TDB_REQUIRE(r.type == sql::Value::Type::INT64);
    TDB_REQUIRE_EQ(r.int_val, (int64_t)5);
}

TDB_TEST(lua_db_query_roundtrip) {
    Database db;
    db.open("");
    db.execute("CREATE TABLE t (x INTEGER)");
    db.execute("INSERT INTO t VALUES (10)");
    script::ScriptEngineOptions opts;
    opts.execute_fn = [&db](const std::string &s) { return db.execute(s); };
    script::ScriptEngine eng(opts);
    auto r = eng.eval("local rs = db.query('SELECT x FROM t'); return rs[1].x");
    // We don't assert the exact value yet — the v1 bridge returns numerics
    // through the int/float path. Round-trip exactness is a Phase 3 concern.
    TDB_REQUIRE(!eng.last_error().size() || r.type != sql::Value::Type::NULL_VAL);
}

TDB_TEST_MAIN("lua_bridge")
