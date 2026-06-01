// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/sql/lexer.h"

using namespace tdb::sql;

TDB_TEST(lexer_select_one) {
    Lexer lex("SELECT 1");
    auto t1 = lex.next_token();
    TDB_REQUIRE(t1.type == TokenType::KW_SELECT);
    auto t2 = lex.next_token();
    TDB_REQUIRE(t2.type == TokenType::INTEGER_LITERAL);
}

TDB_TEST(lexer_string_literal) {
    Lexer lex("'hello'");
    auto t = lex.next_token();
    TDB_REQUIRE(t.type == TokenType::STRING_LITERAL);
}

TDB_TEST(lexer_identifier_after_keyword) {
    Lexer lex("FROM users");
    TDB_REQUIRE(lex.next_token().type == TokenType::KW_FROM);
    auto ident = lex.next_token();
    TDB_REQUIRE(ident.type == TokenType::IDENTIFIER);
}

TDB_TEST_MAIN("lexer")
