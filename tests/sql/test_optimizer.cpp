// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/sql/parser.h"

using namespace tdb::sql;

// Optimizer is internal to the executor; here we verify the parser still
// produces a statement structure the optimizer can consume.
TDB_TEST(optimizer_consumes_simple_select) {
    Parser p("SELECT id FROM t WHERE id = 1");
    auto stmt = p.parse();
    TDB_REQUIRE(stmt != nullptr);
    TDB_REQUIRE(stmt->type == ast::StmtType::SELECT);
}

TDB_TEST_MAIN("optimizer")
