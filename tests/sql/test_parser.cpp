// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/sql/parser.h"
#include "tdb/sql/ast.h"

using namespace tdb::sql;

TDB_TEST(parser_select_one) {
    Parser p("SELECT 1");
    auto stmt = p.parse();
    TDB_REQUIRE(stmt != nullptr);
    TDB_REQUIRE(stmt->type == ast::StmtType::SELECT);
}

TDB_TEST(parser_create_table) {
    Parser p("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
    auto stmt = p.parse();
    TDB_REQUIRE(stmt != nullptr);
    TDB_REQUIRE(stmt->type == ast::StmtType::CREATE_TABLE);
}

TDB_TEST(parser_insert) {
    Parser p("INSERT INTO t (id, name) VALUES (1, 'a')");
    auto stmt = p.parse();
    TDB_REQUIRE(stmt != nullptr);
    TDB_REQUIRE(stmt->type == ast::StmtType::INSERT);
}

TDB_TEST_MAIN("parser")
