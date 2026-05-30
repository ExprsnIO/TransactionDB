/* test_lexer.c — tokenizer behavior. */
#include "tdb_test.h"
#include "../src/sql/tdb_lexer.h"

#include <string.h>

static void test_keywords_ids(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "SELECT a, b FROM t", 0);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_SELECT);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_ID);
  TDB_CHECK_EQ(t.n, 1); TDB_CHECK(t.z[0] == 'a');
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_COMMA);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_ID);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_FROM);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_ID);
  TDB_CHECK_EQ(tdb_lex_next(&lx, &t), TK_EOF);
}

static void test_numbers(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "1 2.5 0xFF 1e3", 0);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_INTEGER); TDB_CHECK_EQ(t.ival, 1);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_FLOAT); TDB_CHECK(t.rval > 2.4 && t.rval < 2.6);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_INTEGER); TDB_CHECK_EQ(t.ival, 255);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_FLOAT); TDB_CHECK(t.rval > 999 && t.rval < 1001);
}

static void test_operators(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "<= >= != <> || = < > + - * / %", 0);
  int want[] = {TK_LE, TK_GE, TK_NE, TK_NE, TK_CONCAT, TK_EQ, TK_LT, TK_GT,
                TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT};
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
    TDB_CHECK_EQ(tdb_lex_next(&lx, &t), want[i]);
}

static void test_strings_comments(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "'it''s' /* c */ 1 -- tail\n + 2", 0);
  tdb_lex_next(&lx, &t);
  TDB_CHECK_EQ(t.kind, TK_STRING);
  TDB_CHECK_EQ(t.n, 5);                 /* raw inner: i t ' ' s */
  TDB_CHECK(strncmp(t.z, "it''s", 5) == 0);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_INTEGER); TDB_CHECK_EQ(t.ival, 1);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_PLUS);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_INTEGER); TDB_CHECK_EQ(t.ival, 2);
}

static void test_dollar_quote(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "$$ return x*2 $$", 0);
  tdb_lex_next(&lx, &t);
  TDB_CHECK_EQ(t.kind, TK_STRING);
  TDB_CHECK(strncmp(t.z, " return x*2 ", (size_t)t.n) == 0);

  tdb_lex_init(&lx, "$body$abc$body$", 0);
  tdb_lex_next(&lx, &t);
  TDB_CHECK_EQ(t.kind, TK_STRING);
  TDB_CHECK_EQ(t.n, 3);
  TDB_CHECK(strncmp(t.z, "abc", 3) == 0);
}

static void test_params_quoted_id(void) {
  tdb_lexer lx; tdb_token t;
  tdb_lex_init(&lx, "? :name $1 \"my col\"", 0);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_PARAM);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_PARAM);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_PARAM);
  tdb_lex_next(&lx, &t); TDB_CHECK_EQ(t.kind, TK_ID); TDB_CHECK_EQ(t.n, 6);
  TDB_CHECK(strncmp(t.z, "my col", 6) == 0);
}

static void test_keyword_lookup_ci(void) {
  TDB_CHECK_EQ(tdb_keyword_lookup("select", 6), TK_SELECT);
  TDB_CHECK_EQ(tdb_keyword_lookup("FROM", 4), TK_FROM);
  TDB_CHECK_EQ(tdb_keyword_lookup("notakeyword", 11), TK_ID);
}

static tdb_test_case cases[] = {
  {"keywords_ids", test_keywords_ids},
  {"numbers", test_numbers},
  {"operators", test_operators},
  {"strings_comments", test_strings_comments},
  {"dollar_quote", test_dollar_quote},
  {"params_quoted_id", test_params_quoted_id},
  {"keyword_lookup_ci", test_keyword_lookup_ci},
};
TDB_MAIN(cases)
