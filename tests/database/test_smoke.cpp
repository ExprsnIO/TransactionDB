// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
//
// End-to-end smoke test against the public Database surface. This is the
// v1-format regression net from the V2.00 roadmap (§10 Risk #2): if a v1.x
// .tdb file ever fails to open or run a basic query, this test fails.

#include "tdb_test.h"
#include "tdb/database.h"

using namespace tdb;

TDB_TEST(database_inline_select) {
    Database db;
    db.open("");
    auto r = db.execute("SELECT 1 + 1 AS two");
    TDB_REQUIRE(r.success);
    TDB_REQUIRE_EQ(r.rows.size(), (size_t)1);
}

TDB_TEST(database_create_insert_select) {
    Database db;
    db.open("");
    TDB_REQUIRE(db.execute("CREATE TABLE t (id INTEGER, name TEXT)").success);
    TDB_REQUIRE(db.execute("INSERT INTO t VALUES (1, 'a'), (2, 'b')").success);
    auto r = db.execute("SELECT COUNT(*) FROM t");
    TDB_REQUIRE(r.success);
    TDB_REQUIRE_EQ(r.rows.size(), (size_t)1);
}

TDB_TEST(database_open_v1_example_file) {
    Database db;
    bool opened = db.open("examples/erp_sample.tdb");
    TDB_REQUIRE(opened);
    auto r = db.execute("SELECT COUNT(*) FROM information_schema.TABLES");
    TDB_REQUIRE(r.success);
    TDB_REQUIRE_EQ(r.rows.size(), (size_t)1);
}

TDB_TEST_MAIN("database_smoke")
