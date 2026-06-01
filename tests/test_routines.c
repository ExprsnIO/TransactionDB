/* test_routines.c — sequences and stored PL/SQL functions through SQL. */
#include "tdb_test.h"
#include "transactiondb.h"

#include <stdio.h>
#include <string.h>

static int exec(tdb_db *db, const char *sql) {
  char *err = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &err);
  if (rc != TDB_OK) fprintf(stderr, "  exec failed: %s -> %s\n", sql, err ? err : tdb_errmsg(db));
  tdb_free(err);
  return rc;
}

static int64_t scalar_int(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL; int64_t v = -999;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return v;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_sequences(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT nextval('s')"), 1);
  TDB_CHECK_EQ(scalar_int(db, "SELECT nextval('s')"), 2);
  TDB_CHECK_EQ(scalar_int(db, "SELECT nextval('s')"), 3);
  TDB_CHECK_EQ(scalar_int(db, "SELECT currval('s')"), 3);
  TDB_CHECK_EQ(scalar_int(db, "SELECT setval('s', 100)"), 100);
  TDB_CHECK_EQ(scalar_int(db, "SELECT nextval('s')"), 101);
  /* independent sequences */
  TDB_CHECK_EQ(scalar_int(db, "SELECT nextval('other')"), 1);
  tdb_close(db);
}

static void test_sequence_in_insert(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE t (id INTEGER, v INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t (id, v) VALUES (nextval('idseq'), 10)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t (id, v) VALUES (nextval('idseq'), 20)"), TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT id FROM t WHERE v = 20"), 2);
  tdb_close(db);
}

static void test_plsql_function(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION dbl(x INTEGER) RETURNS INTEGER LANGUAGE PLSQL AS "
    "$$ BEGIN RETURN x * 2; END $$"), TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT dbl(21)"), 42);
  TDB_CHECK_EQ(scalar_int(db, "SELECT dbl(dbl(5))"), 20);
  tdb_close(db);
}

static void test_plsql_control_flow(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  /* factorial via a FOR loop */
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION fact(n INTEGER) RETURNS INTEGER LANGUAGE PLSQL AS "
    "$$ DECLARE f INTEGER := 1; BEGIN FOR i IN 1 .. n LOOP f := f * i; END LOOP; RETURN f; END $$"),
    TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT fact(5)"), 120);
  TDB_CHECK_EQ(scalar_int(db, "SELECT fact(0)"), 1);

  /* classify via IF/ELSIF/ELSE */
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION sgn(n INTEGER) RETURNS INTEGER LANGUAGE PLSQL AS "
    "$$ BEGIN IF n < 0 THEN RETURN -1; ELSIF n = 0 THEN RETURN 0; ELSE RETURN 1; END IF; END $$"),
    TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT sgn(-7)"), -1);
  TDB_CHECK_EQ(scalar_int(db, "SELECT sgn(0)"), 0);
  TDB_CHECK_EQ(scalar_int(db, "SELECT sgn(99)"), 1);
  tdb_close(db);
}

static void test_plsql_uses_sql_row(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE nums (x INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO nums (x) VALUES (3),(4),(5)"), TDB_OK);
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION sq(x INTEGER) RETURNS INTEGER LANGUAGE PLSQL AS "
    "$$ BEGIN RETURN x * x; END $$"), TDB_OK);
  /* apply the PL/SQL function across a result column */
  TDB_CHECK_EQ(scalar_int(db, "SELECT SUM(sq(x)) FROM nums"), 9 + 16 + 25);
  tdb_close(db);
}

static const tdb_test_case cases[] = {
  {"sequences", test_sequences},
  {"sequence_in_insert", test_sequence_in_insert},
  {"plsql_function", test_plsql_function},
  {"plsql_control_flow", test_plsql_control_flow},
  {"plsql_uses_sql_row", test_plsql_uses_sql_row},
};
TDB_MAIN(cases)
