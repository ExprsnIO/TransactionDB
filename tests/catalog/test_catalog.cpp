// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/catalog.h"

using namespace tdb::catalog;

TDB_TEST(catalog_empty_on_construction) {
    Catalog c;
    TDB_REQUIRE_EQ(c.list_tables().size(), (size_t)0);
}

TDB_TEST(catalog_add_find_table) {
    Catalog c;
    TableInfo t;
    t.name = "users";
    ColumnInfo col;
    col.name = "id";
    col.type.name = "INTEGER";
    col.primary_key = true;
    t.columns.push_back(col);
    c.add_table("users", t);
    TDB_REQUIRE_EQ(c.list_tables().size(), (size_t)1);
    TDB_REQUIRE(c.find_table("users") != nullptr);
    TDB_REQUIRE_EQ(c.find_table("users")->columns.size(), (size_t)1);
}

TDB_TEST(catalog_drop_table) {
    Catalog c;
    TableInfo t;
    t.name = "drop_me";
    c.add_table("drop_me", t);
    TDB_REQUIRE(c.find_table("drop_me") != nullptr);
    TDB_REQUIRE(c.drop_table("drop_me"));
    TDB_REQUIRE(c.find_table("drop_me") == nullptr);
}

TDB_TEST_MAIN("catalog")
