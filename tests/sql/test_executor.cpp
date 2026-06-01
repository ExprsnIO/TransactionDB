// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/sql/executor.h"

using namespace tdb::sql;

TDB_TEST(value_int_factory) {
    auto v = Value::make_int(42);
    TDB_REQUIRE(v.type == Value::Type::INT64);
    TDB_REQUIRE_EQ(v.int_val, (int64_t)42);
}

TDB_TEST(value_null_factory) {
    auto v = Value::make_null();
    TDB_REQUIRE(v.is_null());
}

TDB_TEST(value_string_factory) {
    auto v = Value::make_string("hello");
    TDB_REQUIRE(v.type == Value::Type::STRING);
    TDB_REQUIRE_EQ(v.str_val, std::string("hello"));
}

TDB_TEST(value_decimal_preserves_scale) {
    auto v = Value::make_decimal("123.4500", 4);
    TDB_REQUIRE(v.type == Value::Type::DECIMAL);
    TDB_REQUIRE_EQ(v.int_val, (int64_t)4);
    TDB_REQUIRE_EQ(v.str_val, std::string("123.4500"));
}

TDB_TEST_MAIN("executor_values")
