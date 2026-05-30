/* test_exec.c — end-to-end SQL through the public API. */
#include "tdb_test.h"
#include "transactiondb.h"

#include <stdio.h>
#include <string.h>

static int exec(tdb_db *db, const char *sql) {
  char *err = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &err);
  if (rc != TDB_OK) { fprintf(stderr, "  exec failed: %s -> %s\n", sql, err ? err : tdb_errmsg(db)); }
  tdb_free(err);
  return rc;
}

/* run a scalar SELECT returning one integer */
static int64_t scalar(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -999;
  int64_t v = -1;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_ddl_dml_select(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);

  TDB_CHECK_EQ(exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t (id,name,age) VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40)"), TDB_OK);

  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(age) FROM t"), 95);
  TDB_CHECK_EQ(scalar(db, "SELECT age FROM t WHERE name = 'bob'"), 25);

  /* projection + ordering */
  tdb_stmt *s;
  tdb_prepare_v2(db, "SELECT name, age FROM t WHERE age >= 30 ORDER BY age DESC", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "carol");
  TDB_CHECK_EQ(tdb_column_int(s, 1), 40);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "alice");
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  TDB_CHECK_EQ(tdb_column_count(s), 2);
  TDB_CHECK_STR(tdb_column_name(s, 0), "name");
  tdb_finalize(s);

  /* UPDATE + DELETE */
  TDB_CHECK_EQ(exec(db, "UPDATE t SET age = age + 1 WHERE name = 'alice'"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT age FROM t WHERE id = 1"), 31);
  TDB_CHECK_EQ(exec(db, "DELETE FROM t WHERE age < 30"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);

  tdb_close(db);
}

static void test_strict_typing(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE n (id INTEGER PRIMARY KEY, amt DECIMAL(6,2), q SMALLINT)");
  /* good insert */
  TDB_CHECK_EQ(exec(db, "INSERT INTO n VALUES (1, 12.34, 100)"), TDB_OK);
  /* out-of-range SMALLINT -> constraint */
  TDB_CHECK(exec(db, "INSERT INTO n VALUES (2, 1.0, 100000)") != TDB_OK);
  /* non-numeric into DECIMAL -> mismatch */
  TDB_CHECK(exec(db, "INSERT INTO n VALUES (3, 'abc', 1)") != TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM n"), 1);
  tdb_close(db);
}

static void test_generated_column(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE line (id INTEGER PRIMARY KEY, qty INTEGER, price INTEGER, "
           "total INTEGER GENERATED ALWAYS AS (qty * price) STORED)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO line (id,qty,price) VALUES (1, 3, 7)"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT total FROM line WHERE id=1"), 21);
  tdb_close(db);
}

static void test_join_and_group(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE dept (id INTEGER PRIMARY KEY, name TEXT)");
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, dept_id INTEGER, salary INTEGER)");
  exec(db, "INSERT INTO dept VALUES (1,'eng'),(2,'sales')");
  exec(db, "INSERT INTO emp VALUES (1,1,100),(2,1,120),(3,2,90)");

  /* inner join */
  TDB_CHECK_EQ(scalar(db,
    "SELECT e.salary FROM emp e JOIN dept d ON e.dept_id = d.id "
    "WHERE d.name = 'sales'"), 90);

  /* group by with aggregate + HAVING */
  tdb_stmt *s;
  tdb_prepare_v2(db,
    "SELECT dept_id, SUM(salary) AS s FROM emp GROUP BY dept_id HAVING SUM(salary) > 100 "
    "ORDER BY dept_id", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 1);
  TDB_CHECK_EQ(tdb_column_int(s, 1), 220);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);
  tdb_close(db);
}

static void test_txn_and_persist(void) {
  const char *path = "test_exec.db";
  remove(path); remove("test_exec.db-wal");

  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE kv (k INTEGER PRIMARY KEY, v TEXT)");

  /* explicit transaction rolled back */
  exec(db, "BEGIN");
  exec(db, "INSERT INTO kv VALUES (1, 'one')");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM kv"), 1);
  exec(db, "ROLLBACK");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM kv"), 0);

  /* committed transaction persists across reopen */
  exec(db, "BEGIN");
  exec(db, "INSERT INTO kv VALUES (2, 'two'), (3, 'three')");
  exec(db, "COMMIT");
  tdb_close(db);

  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM kv"), 2);
  tdb_stmt *s; tdb_prepare_v2(db, "SELECT v FROM kv WHERE k = 2", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "two");
  tdb_finalize(s);
  tdb_close(db);
  remove(path); remove("test_exec.db-wal");
}

static void test_bind_params(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE p (id INTEGER PRIMARY KEY, v INTEGER)");
  tdb_stmt *s;
  tdb_prepare_v2(db, "INSERT INTO p VALUES (?, ?)", -1, &s, NULL);
  tdb_bind_int(s, 1, 10); tdb_bind_int(s, 2, 42);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM p WHERE id = 10"), 42);
  tdb_close(db);
}

static tdb_test_case cases[] = {
  {"ddl_dml_select", test_ddl_dml_select},
  {"strict_typing", test_strict_typing},
  {"generated_column", test_generated_column},
  {"join_and_group", test_join_and_group},
  {"txn_and_persist", test_txn_and_persist},
  {"bind_params", test_bind_params},
};
TDB_MAIN(cases)
