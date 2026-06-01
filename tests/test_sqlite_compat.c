/* test_sqlite_compat.c — coverage for the SQLite-compatible feature set added
** alongside the executor (date/time, JSON, scalar/aggregate builtins, blob
** literals, JOIN USING/NATURAL, CREATE TABLE AS SELECT, last_insert_rowid,
** changes, total_changes).
*/
#include "tdb_test.h"
#include "transactiondb.h"

#include <stdio.h>
#include <string.h>

static int exec(tdb_db *db, const char *sql) {
  char *err = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &err);
  if (rc != TDB_OK) fprintf(stderr, "  exec: %s -> %s\n", sql, err ? err : tdb_errmsg(db));
  tdb_free(err);
  return rc;
}

static int64_t scalar_i(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -999;
  int64_t v = -1;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static const char *scalar_t(tdb_db *db, const char *sql, char *out, int outsz) {
  tdb_stmt *s = NULL;
  out[0] = 0;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return out;
  if (tdb_step(s) == TDB_ROW) {
    const char *t = tdb_column_text(s, 0);
    if (t) { strncpy(out, t, (size_t)(outsz - 1)); out[outsz - 1] = 0; }
  }
  tdb_finalize(s);
  return out;
}

static void test_changes_and_rowid(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");

  TDB_CHECK_EQ(tdb_last_insert_rowid(db), 0);
  TDB_CHECK_EQ(tdb_changes(db), 0);
  TDB_CHECK_EQ(tdb_total_changes(db), 0);

  exec(db, "INSERT INTO t (name) VALUES ('a'),('b'),('c')");
  TDB_CHECK_EQ(tdb_changes(db), 3);
  TDB_CHECK(tdb_last_insert_rowid(db) > 0);

  /* sqlite-style functions in SQL */
  TDB_CHECK_EQ(scalar_i(db, "SELECT changes()"), 3);
  TDB_CHECK(scalar_i(db, "SELECT last_insert_rowid()") > 0);

  exec(db, "UPDATE t SET name = 'z' WHERE id <= 2");
  TDB_CHECK_EQ(tdb_changes(db), 2);
  TDB_CHECK(tdb_total_changes(db) >= 5);

  tdb_close(db);
}

static void test_blob_literal(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE b (v BLOB)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO b VALUES (x'DEADBEEF')"), TDB_OK);
  tdb_stmt *s;
  tdb_prepare_v2(db, "SELECT v FROM b", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_type(s, 0), TDB_BLOB);
  TDB_CHECK_EQ(tdb_column_bytes(s, 0), 4);
  const unsigned char *bb = (const unsigned char *)tdb_column_blob(s, 0);
  TDB_CHECK_EQ(bb[0], 0xDE);
  TDB_CHECK_EQ(bb[1], 0xAD);
  TDB_CHECK_EQ(bb[2], 0xBE);
  TDB_CHECK_EQ(bb[3], 0xEF);
  tdb_finalize(s);
  tdb_close(db);
}

static void test_scalar_funcs(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  char buf[256];

  /* printf / format */
  scalar_t(db, "SELECT printf('hello %s #%d', 'world', 7)", buf, sizeof buf);
  TDB_CHECK_STR(buf, "hello world #7");
  scalar_t(db, "SELECT format('%05d', 42)", buf, sizeof buf);
  TDB_CHECK_STR(buf, "00042");

  /* quote: ints, text, blobs, NULL */
  scalar_t(db, "SELECT quote(42)", buf, sizeof buf);     TDB_CHECK_STR(buf, "42");
  scalar_t(db, "SELECT quote('a''b')", buf, sizeof buf); TDB_CHECK_STR(buf, "'a''b'");
  scalar_t(db, "SELECT quote(NULL)", buf, sizeof buf);   TDB_CHECK_STR(buf, "NULL");
  scalar_t(db, "SELECT quote(x'ab')", buf, sizeof buf);  TDB_CHECK_STR(buf, "X'ab'");

  /* char / unicode */
  scalar_t(db, "SELECT char(72, 105)", buf, sizeof buf); TDB_CHECK_STR(buf, "Hi");
  TDB_CHECK_EQ(scalar_i(db, "SELECT unicode('A')"), 65);
  TDB_CHECK_EQ(scalar_i(db, "SELECT unicode('A')"), 65);

  /* trim with chars */
  scalar_t(db, "SELECT trim('---hi---', '-')", buf, sizeof buf); TDB_CHECK_STR(buf, "hi");
  scalar_t(db, "SELECT ltrim('xyhi', 'xy')",  buf, sizeof buf);  TDB_CHECK_STR(buf, "hi");
  scalar_t(db, "SELECT rtrim('hixy', 'xy')",  buf, sizeof buf);  TDB_CHECK_STR(buf, "hi");

  /* version */
  scalar_t(db, "SELECT tdb_version()", buf, sizeof buf);
  TDB_CHECK(strlen(buf) > 0);

  /* random returns an integer */
  tdb_stmt *s; tdb_prepare_v2(db, "SELECT random()", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_type(s, 0), TDB_INTEGER);
  tdb_finalize(s);

  tdb_close(db);
}

static void test_date_time(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  char buf[64];

  scalar_t(db, "SELECT date('2024-03-15')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024-03-15");

  scalar_t(db, "SELECT date('2024-03-15', '+1 day')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024-03-16");

  scalar_t(db, "SELECT date('2024-03-15', '-15 day')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024-02-29");                       /* 2024 is a leap year */

  scalar_t(db, "SELECT date('2024-03-15', '+1 month')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024-04-15");

  scalar_t(db, "SELECT time('2024-03-15 10:11:12')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "10:11:12");

  scalar_t(db, "SELECT datetime('2024-03-15 10:11:12', '+1 hour')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024-03-15 11:11:12");

  scalar_t(db, "SELECT strftime('%Y/%m/%d %H:%M', '2024-03-15 10:11:12')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "2024/03/15 10:11");

  /* unixepoch: 1970-01-01 00:00 -> 0 */
  TDB_CHECK_EQ(scalar_i(db, "SELECT unixepoch('1970-01-01 00:00:00')"), 0);
  TDB_CHECK_EQ(scalar_i(db, "SELECT unixepoch('1970-01-02 00:00:00')"), 86400);

  /* julianday — 2000-01-01 12:00 UTC = 2451545.0 */
  tdb_stmt *s; tdb_prepare_v2(db, "SELECT julianday('2000-01-01 12:00:00')", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  double jd = tdb_column_double(s, 0);
  TDB_CHECK(jd > 2451544.9 && jd < 2451545.1);
  tdb_finalize(s);

  tdb_close(db);
}

static void test_group_concat(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (g INTEGER, v TEXT)");
  exec(db, "INSERT INTO t VALUES (1,'a'),(1,'b'),(1,'c'),(2,'x'),(2,'y')");

  char buf[64];
  scalar_t(db, "SELECT group_concat(v) FROM t WHERE g=1", buf, sizeof buf);
  TDB_CHECK_STR(buf, "a,b,c");
  scalar_t(db, "SELECT group_concat(v, '|') FROM t WHERE g=2", buf, sizeof buf);
  TDB_CHECK_STR(buf, "x|y");

  tdb_close(db);
}

static void test_json(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  char buf[256];

  scalar_t(db, "SELECT json_object('a', 1, 'b', 'hi')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "{\"a\":1,\"b\":\"hi\"}");

  scalar_t(db, "SELECT json_array(1, 2.5, 'x', NULL)", buf, sizeof buf);
  TDB_CHECK_STR(buf, "[1,2.5,\"x\",null]");

  scalar_t(db, "SELECT json_extract('{\"a\":{\"b\":42}}', '$.a.b')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "42");

  scalar_t(db, "SELECT json_extract('[10,20,30]', '$[1]')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "20");

  TDB_CHECK_EQ(scalar_i(db, "SELECT json_array_length('[1,2,3,4]')"), 4);
  TDB_CHECK_EQ(scalar_i(db, "SELECT json_valid('{\"a\":1}')"), 1);
  TDB_CHECK_EQ(scalar_i(db, "SELECT json_valid('{a:1}')"), 0);

  scalar_t(db, "SELECT json_type('{\"a\":1}')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "object");
  scalar_t(db, "SELECT json_type('[1,2]')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "array");
  scalar_t(db, "SELECT json_type('{\"a\":3.5}', '$.a')", buf, sizeof buf);
  TDB_CHECK_STR(buf, "real");

  tdb_close(db);
}

static void test_join_using_natural(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE a (id INTEGER, name TEXT)");
  exec(db, "CREATE TABLE b (id INTEGER, age INTEGER)");
  exec(db, "INSERT INTO a VALUES (1,'alice'),(2,'bob'),(3,'carol')");
  exec(db, "INSERT INTO b VALUES (1,30),(2,25),(3,40)");

  /* JOIN ... USING (id) */
  TDB_CHECK_EQ(scalar_i(db, "SELECT b.age FROM a JOIN b USING (id) WHERE name='bob'"), 25);

  /* NATURAL JOIN */
  TDB_CHECK_EQ(scalar_i(db, "SELECT b.age FROM a NATURAL JOIN b WHERE name='carol'"), 40);

  tdb_close(db);
}

static void test_check_constraint(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, age INTEGER CHECK (age >= 0 AND age < 150))"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (1, 30)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (2, -5)"), TDB_CONSTRAINT);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (3, 200)"), TDB_CONSTRAINT);
  TDB_CHECK_EQ(scalar_i(db, "SELECT COUNT(*) FROM t"), 1);
  tdb_close(db);
}

static void test_at_name_params(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
  tdb_stmt *s;
  TDB_CHECK_EQ(tdb_prepare_v2(db, "INSERT INTO t VALUES (@id, @nm)", -1, &s, NULL), TDB_OK);
  tdb_bind_int(s, 1, 42);
  tdb_bind_text(s, 2, "x", -1);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);
  TDB_CHECK_EQ(scalar_i(db, "SELECT id FROM t"), 42);
  tdb_close(db);
}

static void test_explain_query_plan(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
  /* EXPLAIN QUERY PLAN should parse and run (output format is tdb's). */
  TDB_CHECK_EQ(exec(db, "EXPLAIN QUERY PLAN SELECT * FROM t"), TDB_OK);
  tdb_close(db);
}

static void test_create_table_as_select(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER, name TEXT, age INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,'a',10),(2,'b',20),(3,'c',30)");

  /* derive a new table from a SELECT */
  TDB_CHECK_EQ(exec(db, "CREATE TABLE t2 AS SELECT name, age FROM t WHERE age >= 20"), TDB_OK);
  TDB_CHECK_EQ(scalar_i(db, "SELECT COUNT(*) FROM t2"), 2);
  TDB_CHECK_EQ(scalar_i(db, "SELECT age FROM t2 WHERE name='c'"), 30);

  /* full copy */
  exec(db, "CREATE TABLE t3 AS SELECT * FROM t");
  TDB_CHECK_EQ(scalar_i(db, "SELECT COUNT(*) FROM t3"), 3);

  tdb_close(db);
}

static tdb_test_case cases[] = {
  {"changes_and_rowid",     test_changes_and_rowid},
  {"blob_literal",          test_blob_literal},
  {"scalar_funcs",          test_scalar_funcs},
  {"date_time",             test_date_time},
  {"group_concat",          test_group_concat},
  {"json",                  test_json},
  {"join_using_natural",    test_join_using_natural},
  {"check_constraint",      test_check_constraint},
  {"at_name_params",        test_at_name_params},
  {"explain_query_plan",    test_explain_query_plan},
  {"create_table_as_select",test_create_table_as_select},
};

TDB_MAIN(cases)
