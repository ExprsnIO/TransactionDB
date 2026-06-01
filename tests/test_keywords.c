/* test_keywords.c — SQLite-style keyword introspection and non-reserved use. */
#include "tdb_test.h"
#include "transactiondb.h"

#include <string.h>

static void test_count_and_names(void) {
  int n = tdb_keyword_count();
  TDB_CHECK(n > 100);   /* comparable to SQLite's ~140 keywords */
  /* every reported keyword round-trips through tdb_keyword_check */
  for (int i = 0; i < n; i++) {
    const char *z = NULL; int len = 0;
    TDB_CHECK_EQ(tdb_keyword_name(i, &z, &len), TDB_OK);
    TDB_CHECK(z != NULL && len > 0 && (int)strlen(z) == len);
    TDB_CHECK_EQ(tdb_keyword_check(z, len), 1);
  }
  /* out-of-range */
  const char *z; int len;
  TDB_CHECK_EQ(tdb_keyword_name(-1, &z, &len), TDB_RANGE);
  TDB_CHECK_EQ(tdb_keyword_name(n, &z, &len), TDB_RANGE);
}

static void test_check(void) {
  TDB_CHECK_EQ(tdb_keyword_check("SELECT", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("select", -1), 1);  /* case-insensitive */
  TDB_CHECK_EQ(tdb_keyword_check("NATURAL", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("window", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("current_timestamp", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("RECURSIVE", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("row", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("xyzzy", -1), 0);
  TDB_CHECK_EQ(tdb_keyword_check("notakeyword", -1), 0);
  /* length-bounded: only the first 6 chars are "SELECT" */
  TDB_CHECK_EQ(tdb_keyword_check("SELECTED", 6), 1);
  TDB_CHECK_EQ(tdb_keyword_check("SELECTED", 8), 0);
}

static int64_t scalar_int(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL; int64_t v = -999;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return v;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

/* Non-reserved keywords (like SQLite's) remain usable as identifiers. */
static void test_nonreserved_identifier(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char *err = NULL;
  int rc = tdb_exec(db, "CREATE TABLE t (row INTEGER, match TEXT)", NULL, NULL, &err);
  TDB_CHECK_EQ(rc, TDB_OK);
  tdb_free(err); err = NULL;
  TDB_CHECK_EQ(tdb_exec(db, "INSERT INTO t (row, match) VALUES (5, 'x')", NULL, NULL, &err), TDB_OK);
  tdb_free(err);
  TDB_CHECK_EQ(scalar_int(db, "SELECT row FROM t"), 5);
  /* the very same words are still reported as keywords */
  TDB_CHECK_EQ(tdb_keyword_check("row", -1), 1);
  TDB_CHECK_EQ(tdb_keyword_check("match", -1), 1);
  tdb_close(db);
}

static const tdb_test_case cases[] = {
  {"count_and_names", test_count_and_names},
  {"check", test_check},
  {"nonreserved_identifier", test_nonreserved_identifier},
};
TDB_MAIN(cases)
