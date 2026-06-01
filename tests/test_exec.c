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

static int rowcount(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -1;
  int n = 0;
  while (tdb_step(s) == TDB_ROW) n++;
  tdb_finalize(s);
  return n;
}
static void check_text(tdb_db *db, const char *sql, const char *expect) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) { TDB_CHECK(0); return; }
  if (tdb_step(s) == TDB_ROW) { const char *t = tdb_column_text(s, 0); TDB_CHECK_STR(t ? t : "(null)", expect); }
  else TDB_CHECK(0);
  tdb_finalize(s);
}

static void test_like_distinct(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, cat TEXT)");
  exec(db, "INSERT INTO t VALUES (1,'apple','fruit'),(2,'apricot','fruit'),"
           "(3,'beet','veg'),(4,'banana','fruit')");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE name LIKE 'ap%'"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE name LIKE '_eet'"), 1);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE name NOT LIKE 'b%'"), 2);
  TDB_CHECK_EQ(rowcount(db, "SELECT DISTINCT cat FROM t"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(DISTINCT cat) FROM t"), 2);
  tdb_close(db);
}

static void test_bitwise(void) {
  tdb_db *db; tdb_open(":memory:", &db);

  TDB_CHECK_EQ(scalar(db, "SELECT 12 & 10"), 8);
  TDB_CHECK_EQ(scalar(db, "SELECT 12 | 10"), 14);
  TDB_CHECK_EQ(scalar(db, "SELECT 1 << 4"), 16);
  TDB_CHECK_EQ(scalar(db, "SELECT 256 >> 2"), 64);
  TDB_CHECK_EQ(scalar(db, "SELECT ~0"), -1);
  TDB_CHECK_EQ(scalar(db, "SELECT ~5"), -6);

  /* precedence: '+'/'-' bind tighter than bitwise, which binds tighter than
  ** comparisons */
  TDB_CHECK_EQ(scalar(db, "SELECT 6 & 3 + 1"), 4);          /* 6 & (3+1) */
  TDB_CHECK_EQ(scalar(db, "SELECT (5 & 3) = 1"), 1);
  TDB_CHECK_EQ(scalar(db, "SELECT 5 & 3 = 1"), 1);          /* 5 & (3=1)? no: (5&3)=1 */
  TDB_CHECK_EQ(scalar(db, "SELECT 1 | 2 & 3"), 3);          /* left-assoc: (1|2)&3 */

  /* NULL propagates */
  TDB_CHECK_EQ(rowcount(db, "SELECT 1 WHERE (NULL & 1) IS NULL"), 1);

  /* on stored columns + as a WHERE predicate (bit flags) */
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, flags INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,5),(2,2),(3,7),(4,8)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE flags & 1"), 2);   /* 5,7 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE (flags & 2) <> 0"), 2); /* 2,7 */
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(flags | 1) FROM t"), 5 + 3 + 7 + 9);
  tdb_close(db);
}

static void test_glob_escape(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
  exec(db, "INSERT INTO t VALUES (1,'Apple'),(2,'banana'),(3,'50% off'),"
           "(4,'a_b'),(5,'Cherry'),(6,'apple2')");

  /* GLOB is case-sensitive with Unix wildcards */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB 'A*'"), 1);     /* Apple */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB 'a*'"), 2);     /* a_b, apple2 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB '?pple'"), 1);  /* Apple */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB '[a-c]*'"), 3); /* banana, a_b, apple2 */
  /* char class + negation */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB '[A-Z]*'"), 2); /* Apple, Cherry */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s NOT GLOB '[A-Z]*'"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s GLOB '[^A-Z]*'"), 4);

  /* LIKE ... ESCAPE: the escape char makes %/_ literal */
  check_text(db, "SELECT s FROM t WHERE s LIKE '50!% off' ESCAPE '!'", "50% off");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s LIKE '%!%%' ESCAPE '!'"), 1); /* contains a % */
  check_text(db, "SELECT s FROM t WHERE s LIKE 'a!_b' ESCAPE '!'", "a_b");
  /* without ESCAPE, _ is a wildcard so a_b and (no others of len 3) match */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE s LIKE 'a_b'"), 1);
  tdb_close(db);
}

static void test_builtins(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  check_text(db, "SELECT upper('ab') || substr('hello', 2, 3)", "ABell");
  TDB_CHECK_EQ(scalar(db, "SELECT length(trim('  hi  '))"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT ifnull(NULL, 5)"), 5);
  check_text(db, "SELECT typeof(1)", "integer");
  check_text(db, "SELECT typeof('x')", "text");
  check_text(db, "SELECT replace('aXbXc', 'X', '-')", "a-b-c");
  TDB_CHECK_EQ(scalar(db, "SELECT instr('hello', 'll')"), 3);
  TDB_CHECK_EQ(scalar(db, "SELECT round(3.14159, 2) = 3.14"), 1);
  tdb_close(db);
}

static void test_subqueries(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, cat TEXT)");
  exec(db, "INSERT INTO t VALUES (1,'fruit'),(2,'fruit'),(3,'veg'),(4,'fruit')");
  TDB_CHECK_EQ(scalar(db, "SELECT (SELECT COUNT(*) FROM t)"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE id IN (SELECT id FROM t WHERE cat='fruit')"), 3);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE id NOT IN (SELECT id FROM t WHERE cat='fruit')"), 1);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE EXISTS (SELECT 1 FROM t WHERE cat='veg')"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE EXISTS (SELECT 1 FROM t WHERE cat='nope')"), 0);
  tdb_close(db);
}

static void test_insert_select(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE src (id INTEGER PRIMARY KEY, name TEXT, cat TEXT)");
  exec(db, "INSERT INTO src VALUES (1,'a','x'),(2,'b','y'),(3,'c','x')");
  exec(db, "CREATE TABLE dst (id INTEGER PRIMARY KEY, name TEXT)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO dst (id, name) SELECT id, name FROM src WHERE cat='x'"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM dst"), 2);
  check_text(db, "SELECT name FROM dst WHERE id = 3", "c");
  tdb_close(db);
}

static void test_derived_and_view(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20),(3,30)");
  /* derived table (subquery in FROM) */
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(x) FROM (SELECT v AS x FROM t WHERE v >= 20) s"), 50);
  TDB_CHECK_EQ(rowcount(db, "SELECT * FROM (SELECT v FROM t) d"), 3);
  /* view expansion in FROM */
  exec(db, "CREATE VIEW bigv AS SELECT id, v FROM t WHERE v >= 20");
  TDB_CHECK_EQ(rowcount(db, "SELECT * FROM bigv"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(v) FROM bigv"), 50);
  tdb_close(db);
}

static void test_outer_joins(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE dept (id INTEGER PRIMARY KEY, name TEXT)");
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, dept_id INTEGER, name TEXT)");
  exec(db, "INSERT INTO dept VALUES (1,'eng'),(2,'sales'),(3,'empty')");
  exec(db, "INSERT INTO emp VALUES (1,1,'a'),(2,1,'b'),(3,2,'c'),(4,99,'orphan')");

  /* LEFT: all 4 emp rows kept; orphan has NULL dept */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp LEFT JOIN dept ON emp.dept_id = dept.id"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp LEFT JOIN dept ON emp.dept_id = dept.id WHERE dept.id IS NULL"), 1);
  /* RIGHT: 3 matches + 'empty' dept with NULL emp = 4 (orphan dropped) */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp RIGHT JOIN dept ON emp.dept_id = dept.id"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp RIGHT JOIN dept ON emp.dept_id = dept.id WHERE emp.id IS NULL"), 1);
  /* FULL: 3 matches + orphan + empty = 5 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp FULL JOIN dept ON emp.dept_id = dept.id"), 5);
  /* inner join still correct */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp JOIN dept ON emp.dept_id = dept.id"), 3);
  tdb_close(db);
}

static void test_index_scan(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, age INTEGER, name TEXT)");
  exec(db, "CREATE INDEX idx_age ON t(age)");
  char sql[128];
  for (int i = 1; i <= 50; i++) {
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, 'n')", i, i % 10);
    exec(db, sql);
  }
  /* equality via index */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age = 3"), 5);
  /* range via index */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age >= 8"), 10);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age >= 3 AND age <= 4"), 10);
  /* MVCC recheck: a stale index entry must not produce a wrong row */
  exec(db, "UPDATE t SET age = 100 WHERE id = 3");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age = 3"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age = 100"), 1);
  tdb_close(db);
}

static void test_index_persist(void) {
  const char *path = "test_idx.db";
  remove(path); remove("test_idx.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, age INTEGER)");
  exec(db, "CREATE INDEX idx_age ON t(age)");   /* added after table creation */
  exec(db, "INSERT INTO t VALUES (1,5),(2,5),(3,9)");
  tdb_close(db);
  /* reopen: index definition must have been persisted (catalog row rewrite) */
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age = 5"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age = 9"), 1);
  tdb_close(db);
  remove(path); remove("test_idx.db-wal");
}

static void test_savepoints(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1, 10)");

  exec(db, "BEGIN");
  exec(db, "INSERT INTO t VALUES (2, 20)");
  exec(db, "SAVEPOINT a");
  exec(db, "INSERT INTO t VALUES (3, 30)");
  exec(db, "UPDATE t SET v = 99 WHERE id = 1");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM t WHERE id = 1"), 99);

  /* roll back to savepoint a: undoes insert(3) and the update */
  TDB_CHECK_EQ(exec(db, "ROLLBACK TO a"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM t WHERE id = 1"), 10);

  /* savepoint a still active: continue, release, commit */
  exec(db, "INSERT INTO t VALUES (4, 40)");
  TDB_CHECK_EQ(exec(db, "RELEASE a"), TDB_OK);
  exec(db, "COMMIT");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);          /* 1,2,4 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE id = 3"), 0);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE id = 4"), 1);
  tdb_close(db);
}

static void test_nested_savepoints(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY)");
  exec(db, "BEGIN");
  exec(db, "SAVEPOINT a");
  exec(db, "INSERT INTO t VALUES (1)");
  exec(db, "SAVEPOINT b");
  exec(db, "INSERT INTO t VALUES (2)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);
  /* rolling back to the OUTER savepoint undoes both inserts */
  TDB_CHECK_EQ(exec(db, "ROLLBACK TO a"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 0);
  exec(db, "COMMIT");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 0);
  /* unknown savepoint errors */
  exec(db, "BEGIN");
  TDB_CHECK(exec(db, "ROLLBACK TO nope") != TDB_OK);
  exec(db, "ROLLBACK");
  tdb_close(db);
}

static void test_temporal(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER) WITH SYSTEM VERSIONING");
  TDB_CHECK_EQ(exec(db, "INSERT INTO t (id, v) VALUES (1, 10)"), TDB_OK);
  int64_t v1 = scalar(db, "SELECT current_version()");   /* system time after insert */
  exec(db, "UPDATE t SET v = 20 WHERE id = 1");
  exec(db, "INSERT INTO t (id, v) VALUES (2, 99)");       /* didn't exist at v1 */

  /* current state */
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM t WHERE id = 1"), 20);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);

  /* AS OF v1: the row's old value, and row 2 absent */
  char sql[160];
  snprintf(sql, sizeof(sql), "SELECT v FROM t FOR SYSTEM_TIME AS OF %lld WHERE id = 1", (long long)v1);
  TDB_CHECK_EQ(scalar(db, sql), 10);
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM t FOR SYSTEM_TIME AS OF %lld", (long long)v1);
  TDB_CHECK_EQ(scalar(db, sql), 1);
  tdb_close(db);
}

static void test_columnar(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  /* a columnar table behaves identically through SQL */
  exec(db, "CREATE TABLE c (id INTEGER PRIMARY KEY, name TEXT, age INTEGER) WITH COLUMNAR");
  exec(db, "INSERT INTO c VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM c"), 3);
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(age) FROM c"), 95);
  TDB_CHECK_EQ(scalar(db, "SELECT age FROM c WHERE name = 'bob'"), 25);
  check_text(db, "SELECT name FROM c WHERE id = 3", "carol");

  exec(db, "UPDATE c SET age = 31 WHERE id = 1");
  TDB_CHECK_EQ(scalar(db, "SELECT age FROM c WHERE id = 1"), 31);
  exec(db, "DELETE FROM c WHERE age < 30");           /* bob removed */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM c"), 2);

  /* secondary index on a columnar table */
  exec(db, "CREATE INDEX cage ON c(age)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM c WHERE age = 40"), 1);

  /* join a columnar table with a row table */
  exec(db, "CREATE TABLE r (id INTEGER PRIMARY KEY, tag TEXT)");
  exec(db, "INSERT INTO r VALUES (1,'X'),(3,'Y')");
  check_text(db, "SELECT r.tag FROM c JOIN r ON c.id = r.id WHERE c.id = 3", "Y");
  tdb_close(db);
}

static void test_columnar_temporal_persist(void) {
  const char *path = "test_col.db";
  remove(path); remove("test_col.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE cv (id INTEGER PRIMARY KEY, v INTEGER) WITH SYSTEM VERSIONING WITH COLUMNAR");
  exec(db, "INSERT INTO cv (id, v) VALUES (1, 10)");
  int64_t v1 = scalar(db, "SELECT current_version()");
  exec(db, "UPDATE cv SET v = 20 WHERE id = 1");
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM cv WHERE id = 1"), 20);
  char sql[160];
  snprintf(sql, sizeof(sql), "SELECT v FROM cv FOR SYSTEM_TIME AS OF %lld WHERE id = 1", (long long)v1);
  TDB_CHECK_EQ(scalar(db, sql), 10);                  /* columnar history via AS OF */
  tdb_close(db);

  /* reopen: columnar layout (col_roots) must persist */
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM cv WHERE id = 1"), 20);
  tdb_close(db);
  remove(path); remove("test_col.db-wal");
}

static void test_setops(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE a (x INTEGER PRIMARY KEY)");
  exec(db, "CREATE TABLE b (x INTEGER PRIMARY KEY)");
  exec(db, "INSERT INTO a VALUES (1),(2),(3)");
  exec(db, "INSERT INTO b VALUES (2),(3),(4)");

  /* via a derived table so we can count rows */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT x FROM a UNION SELECT x FROM b) u"), 4);       /* 1,2,3,4 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT x FROM a UNION ALL SELECT x FROM b) u"), 6);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT x FROM a INTERSECT SELECT x FROM b) u"), 2);   /* 2,3 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT x FROM a EXCEPT SELECT x FROM b) u"), 1);      /* 1 */

  /* trailing ORDER BY/LIMIT binds to the whole union */
  tdb_stmt *s;
  tdb_prepare_v2(db, "SELECT x FROM a UNION SELECT x FROM b ORDER BY x DESC LIMIT 2", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW); TDB_CHECK_EQ(tdb_column_int(s, 0), 4);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW); TDB_CHECK_EQ(tdb_column_int(s, 0), 3);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);
  tdb_close(db);
}

static void test_explain_vacuum(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, age INTEGER)");
  exec(db, "CREATE INDEX ia ON t(age)");
  /* EXPLAIN returns a 'plan' result set */
  tdb_stmt *s;
  tdb_prepare_v2(db, "EXPLAIN SELECT * FROM t WHERE age = 5", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_name(s, 0), "plan");
  TDB_CHECK(strstr(tdb_column_text(s, 0), "INDEX") != NULL);  /* should choose ia */
  tdb_finalize(s);
  /* VACUUM runs (force checkpoint) */
  TDB_CHECK_EQ(exec(db, "VACUUM"), TDB_OK);
  tdb_close(db);
}

static void test_projection_pushdown(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  /* columnar: queries that touch a subset of columns read only those column
  ** b-trees (projection pushdown); results must remain correct */
  exec(db, "CREATE TABLE c (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, note TEXT) WITH COLUMNAR");
  exec(db, "INSERT INTO c VALUES (1,10,100,'x'),(2,20,200,'y'),(3,30,300,'z')");
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(a) FROM c"), 60);              /* only column a */
  TDB_CHECK_EQ(scalar(db, "SELECT b FROM c WHERE a = 20"), 200);     /* columns a,b */
  check_text(db, "SELECT note FROM c WHERE id = 3", "z");            /* columns id,note */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM c"), 3);             /* no columns */
  tdb_close(db);
}

static void test_alter(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20)");

  /* ADD COLUMN: existing rows read NULL for it */
  TDB_CHECK_EQ(exec(db, "ALTER TABLE t ADD COLUMN b TEXT"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE b IS NULL"), 2);
  exec(db, "INSERT INTO t (id, a, b) VALUES (3, 30, 'hi')");
  check_text(db, "SELECT b FROM t WHERE id = 3", "hi");

  /* RENAME COLUMN */
  TDB_CHECK_EQ(exec(db, "ALTER TABLE t RENAME COLUMN a TO age"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT age FROM t WHERE id = 1"), 10);
  TDB_CHECK(exec(db, "SELECT a FROM t") != TDB_OK);     /* old name gone */

  /* RENAME TABLE */
  TDB_CHECK_EQ(exec(db, "ALTER TABLE t RENAME TO tt"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM tt"), 3);
  TDB_CHECK(exec(db, "SELECT * FROM t") != TDB_OK);

  /* ADD COLUMN on a columnar table (provisions a new column b-tree) */
  exec(db, "CREATE TABLE c (id INTEGER PRIMARY KEY, x INTEGER) WITH COLUMNAR");
  exec(db, "INSERT INTO c VALUES (1, 100)");
  TDB_CHECK_EQ(exec(db, "ALTER TABLE c ADD COLUMN y INTEGER"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM c WHERE y IS NULL"), 1);
  exec(db, "INSERT INTO c (id, x, y) VALUES (2, 200, 222)");
  TDB_CHECK_EQ(scalar(db, "SELECT y FROM c WHERE id = 2"), 222);
  tdb_close(db);
}

static void test_drop_column(void) {
  tdb_db *db; tdb_open(":memory:", &db);

  /* drop a middle column: remaining columns keep their values */
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10,'x',100),(2,20,'y',200),(3,30,'z',300)");
  TDB_CHECK_EQ(exec(db, "ALTER TABLE t DROP COLUMN b"), TDB_OK);
  {
    tdb_stmt *cs;
    TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT * FROM t WHERE id=1", -1, &cs, NULL), TDB_OK);
    TDB_CHECK_EQ(tdb_step(cs), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_count(cs), 3);     /* id, a, c (b dropped) */
    tdb_finalize(cs);
  }
  TDB_CHECK_EQ(scalar(db, "SELECT a FROM t WHERE id=2"), 20);
  TDB_CHECK_EQ(scalar(db, "SELECT c FROM t WHERE id=3"), 300);
  TDB_CHECK(exec(db, "SELECT b FROM t") != TDB_OK);            /* gone */
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(c) FROM t"), 600);

  /* dropping a column that some rows never stored (added later) keeps NULLs */
  exec(db, "ALTER TABLE t ADD COLUMN d INTEGER");
  exec(db, "INSERT INTO t (id,a,c,d) VALUES (4,40,400,4)");
  TDB_CHECK_EQ(exec(db, "ALTER TABLE t DROP COLUMN a"), TDB_OK);   /* a is middle */
  TDB_CHECK_EQ(scalar(db, "SELECT c FROM t WHERE id=4"), 400);
  TDB_CHECK_EQ(scalar(db, "SELECT d FROM t WHERE id=4"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE d IS NULL"), 3); /* rows 1-3 */

  /* guards */
  TDB_CHECK(exec(db, "ALTER TABLE t DROP COLUMN id") != TDB_OK);   /* PK */
  TDB_CHECK(exec(db, "ALTER TABLE t DROP COLUMN nope") != TDB_OK); /* missing */
  exec(db, "CREATE INDEX ic ON t(c)");
  TDB_CHECK(exec(db, "ALTER TABLE t DROP COLUMN c") != TDB_OK);    /* indexed */

  /* columnar drop */
  exec(db, "CREATE TABLE cc (id INTEGER PRIMARY KEY, p INTEGER, q INTEGER, r INTEGER) WITH COLUMNAR");
  exec(db, "INSERT INTO cc VALUES (1,1,2,3),(2,4,5,6)");
  TDB_CHECK_EQ(exec(db, "ALTER TABLE cc DROP COLUMN q"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT p FROM cc WHERE id=2"), 4);
  TDB_CHECK_EQ(scalar(db, "SELECT r FROM cc WHERE id=2"), 6);
  TDB_CHECK(exec(db, "SELECT q FROM cc") != TDB_OK);
  tdb_close(db);
}

static long file_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

static void test_vacuum_shrinks(void) {
  const char *path = "test_vacuum.db";
  remove(path); remove("test_vacuum.db-wal");
  tdb_db *db; tdb_open(path, &db);

  exec(db, "CREATE TABLE keep (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "CREATE TABLE big (id INTEGER PRIMARY KEY, s TEXT)");
  exec(db, "INSERT INTO keep VALUES (1,111),(2,222)");
  for (int i = 0; i < 1500; i++) {
    char sql[160];
    snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d,'%0100d')", i, i);
    exec(db, sql);
  }
  exec(db, "VACUUM");
  tdb_close(db);
  long full = file_size(path);

  /* drop the big table and compact */
  tdb_open(path, &db);
  exec(db, "DROP TABLE big");
  TDB_CHECK_EQ(exec(db, "VACUUM"), TDB_OK);
  tdb_close(db);
  long shrunk = file_size(path);

  TDB_CHECK(shrunk < full);                 /* file actually got smaller */
  TDB_CHECK(shrunk * 2 < full);             /* and substantially so */

  /* the surviving table is intact and the db is usable after reopen */
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM keep WHERE id = 2"), 222);
  exec(db, "CREATE TABLE more (id INTEGER PRIMARY KEY, n INTEGER)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO more VALUES (1,7)"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT n FROM more WHERE id=1"), 7);
  tdb_close(db);
  remove(path); remove("test_vacuum.db-wal");
}

/* EXPLAIN text helper: does the plan for `sql` mention an INDEX search? */
static int uses_index(tdb_db *db, const char *sql) {
  char buf[256];
  snprintf(buf, sizeof(buf), "EXPLAIN %s", sql);
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, buf, -1, &s, NULL) != TDB_OK) return -1;
  int found = 0;
  while (tdb_step(s) == TDB_ROW) {
    const char *t = tdb_column_text(s, 0);
    if (t && strstr(t, "INDEX")) found = 1;
  }
  tdb_finalize(s);
  return found;
}

static void test_drop_index(void) {
  const char *path = "test_dropidx.db";
  remove(path); remove("test_dropidx.db-wal");
  tdb_db *db; tdb_open(path, &db);

  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, age INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,30),(2,30),(3,40),(4,30)");
  exec(db, "CREATE INDEX ia ON t(age)");
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=30"), 1);

  /* drop it: the planner falls back to a scan, results unchanged */
  TDB_CHECK_EQ(exec(db, "DROP INDEX ia"), TDB_OK);
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=30"), 0);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age=30"), 3);

  /* error + IF EXISTS handling */
  TDB_CHECK(exec(db, "DROP INDEX nope") != TDB_OK);
  TDB_CHECK_EQ(exec(db, "DROP INDEX IF EXISTS nope"), TDB_OK);

  /* recreate and confirm it is used again */
  exec(db, "CREATE INDEX ia ON t(age)");
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=30"), 1);
  tdb_close(db);

  /* the dropped/recreated index set persists across reopen */
  tdb_open(path, &db);
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=40"), 1);
  TDB_CHECK_EQ(exec(db, "DROP INDEX ia"), TDB_OK);
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=40"), 0);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age=40"), 1);
  tdb_close(db);

  /* and the drop persisted too */
  tdb_open(path, &db);
  TDB_CHECK_EQ(uses_index(db, "SELECT * FROM t WHERE age=30"), 0);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE age=30"), 3);
  tdb_close(db);
  remove(path); remove("test_dropidx.db-wal");
}

static void test_drop_view(void) {
  const char *path = "test_dropview.db";
  remove(path); remove("test_dropview.db-wal");
  tdb_db *db; tdb_open(path, &db);

  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20),(3,30)");
  exec(db, "CREATE VIEW hi AS SELECT v FROM t WHERE v >= 20");
  TDB_CHECK_EQ(rowcount(db, "SELECT * FROM hi"), 2);

  /* drop it: the view name no longer resolves */
  TDB_CHECK_EQ(exec(db, "DROP VIEW hi"), TDB_OK);
  TDB_CHECK(exec(db, "SELECT * FROM hi") != TDB_OK);

  /* error + IF EXISTS */
  TDB_CHECK(exec(db, "DROP VIEW nope") != TDB_OK);
  TDB_CHECK_EQ(exec(db, "DROP VIEW IF EXISTS nope"), TDB_OK);

  /* recreate with a different definition */
  exec(db, "CREATE VIEW hi AS SELECT v FROM t WHERE v >= 10");
  TDB_CHECK_EQ(rowcount(db, "SELECT * FROM hi"), 3);
  tdb_close(db);

  /* the recreated view persists across reopen, and the drop persists too */
  tdb_open(path, &db);
  TDB_CHECK_EQ(rowcount(db, "SELECT * FROM hi"), 3);
  TDB_CHECK_EQ(exec(db, "DROP VIEW hi"), TDB_OK);
  tdb_close(db);
  tdb_open(path, &db);
  TDB_CHECK(exec(db, "SELECT * FROM hi") != TDB_OK);   /* stays dropped */
  tdb_close(db);
  remove(path); remove("test_dropview.db-wal");
}

static void test_drop_table_reclaim(void) {
  const char *path = "test_droptab.db";
  remove(path); remove("test_droptab.db-wal");
  tdb_db *db; tdb_open(path, &db);

  /* two tables; dropping one must not disturb the other even as its freed
  ** pages are recycled by subsequent inserts */
  exec(db, "CREATE TABLE keep (id INTEGER PRIMARY KEY, s TEXT)");
  exec(db, "CREATE TABLE gone (id INTEGER PRIMARY KEY, s TEXT)");
  for (int i = 0; i < 500; i++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO keep VALUES (%d,'keep-%d')", i, i);
    exec(db, sql);
    snprintf(sql, sizeof(sql), "INSERT INTO gone VALUES (%d,'gone-%d')", i, i);
    exec(db, sql);
  }
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM keep"), 500);

  TDB_CHECK_EQ(exec(db, "DROP TABLE gone"), TDB_OK);
  TDB_CHECK(exec(db, "SELECT COUNT(*) FROM gone") != TDB_OK);   /* gone */

  /* recreate with the same name (reuses freed pages) and refill */
  exec(db, "CREATE TABLE gone (id INTEGER PRIMARY KEY, s TEXT)");
  for (int i = 0; i < 500; i++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO gone VALUES (%d,'new-%d')", i, i);
    exec(db, sql);
  }

  /* the surviving table is still fully intact */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM keep"), 500);
  check_text(db, "SELECT s FROM keep WHERE id = 250", "keep-250");
  check_text(db, "SELECT s FROM gone WHERE id = 250", "new-250");

  /* persists across reopen */
  tdb_close(db);
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM keep"), 500);
  check_text(db, "SELECT s FROM keep WHERE id = 100", "keep-100");
  tdb_close(db);
  remove(path); remove("test_droptab.db-wal");
}

/* A DROP TABLE (no recreate) must stay dropped after reopen: the persisted
** catalog row is removed, so the table is not resurrected pointing at pages
** that were freed (and possibly reused) when it was dropped. */
static void test_drop_table_persist(void) {
  const char *path = "test_droptab2.db";
  remove(path); remove("test_droptab2.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE keep (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "CREATE TABLE gone (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO keep VALUES (1,11),(2,22)");
  exec(db, "INSERT INTO gone VALUES (1,99)");
  TDB_CHECK_EQ(exec(db, "DROP TABLE gone"), TDB_OK);
  tdb_close(db);

  tdb_open(path, &db);
  TDB_CHECK(exec(db, "SELECT * FROM gone") != TDB_OK);     /* stays dropped */
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM keep WHERE id = 2"), 22);  /* survivor intact */
  /* the name is free to reuse */
  TDB_CHECK_EQ(exec(db, "CREATE TABLE gone (id INTEGER PRIMARY KEY, v INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM gone"), 0);
  tdb_close(db);
  remove(path); remove("test_droptab2.db-wal");
}

static void test_drop_column_persist(void) {
  const char *path = "test_dropcol.db";
  remove(path); remove("test_dropcol.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10,'x',100),(2,20,'y',200)");
  exec(db, "ALTER TABLE t DROP COLUMN b");
  tdb_close(db);
  /* reopen: the rewritten records and shrunken schema must persist */
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT c FROM t WHERE id=1"), 100);
  TDB_CHECK(exec(db, "SELECT b FROM t") != TDB_OK);
  exec(db, "INSERT INTO t (id,a,c) VALUES (3,30,300)");
  TDB_CHECK_EQ(scalar(db, "SELECT a FROM t WHERE id=3"), 30);
  tdb_close(db);
  remove(path); remove("test_dropcol.db-wal");
}

static void test_alter_persist(void) {
  const char *path = "test_alter.db";
  remove(path); remove("test_alter.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
  exec(db, "INSERT INTO t VALUES (1, 10)");
  exec(db, "ALTER TABLE t ADD COLUMN note TEXT");
  exec(db, "ALTER TABLE t RENAME TO renamed");
  tdb_close(db);
  /* reopen: the new column and table rename must have persisted */
  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM renamed"), 1);
  exec(db, "INSERT INTO renamed (id, a, note) VALUES (2, 20, 'kept')");
  check_text(db, "SELECT note FROM renamed WHERE id = 2", "kept");
  tdb_close(db);
  remove(path); remove("test_alter.db-wal");
}

static void test_upsert(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, hits INTEGER)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (1,'a',1),(2,'b',1)"), TDB_OK);

  /* default behaviour: duplicate PK is an error, table unchanged */
  TDB_CHECK(exec(db, "INSERT INTO t VALUES (1,'dup',9)") != TDB_OK);
  check_text(db, "SELECT name FROM t WHERE id = 1", "a");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);

  /* INSERT OR IGNORE: existing row kept, new row added */
  TDB_CHECK_EQ(exec(db, "INSERT OR IGNORE INTO t VALUES (1,'ign',9),(3,'c',1)"), TDB_OK);
  check_text(db, "SELECT name FROM t WHERE id = 1", "a");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);

  /* INSERT OR REPLACE: existing row overwritten */
  TDB_CHECK_EQ(exec(db, "INSERT OR REPLACE INTO t VALUES (1,'repl',5)"), TDB_OK);
  check_text(db, "SELECT name FROM t WHERE id = 1", "repl");
  TDB_CHECK_EQ(scalar(db, "SELECT hits FROM t WHERE id = 1"), 5);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);

  /* ON CONFLICT DO NOTHING */
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (2,'x',9) ON CONFLICT DO NOTHING"), TDB_OK);
  check_text(db, "SELECT name FROM t WHERE id = 2", "b");

  /* ON CONFLICT DO UPDATE SET ... (no conflict path: plain insert) */
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (4,'d',1) ON CONFLICT DO UPDATE SET hits = hits + 1"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT hits FROM t WHERE id = 4"), 1);

  /* ON CONFLICT DO UPDATE SET ... (conflict path: bump existing counter) */
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (4,'d',1) ON CONFLICT DO UPDATE SET hits = hits + 10"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT hits FROM t WHERE id = 4"), 11);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 4);

  /* ON CONFLICT DO UPDATE ... WHERE: predicate false suppresses the update */
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (4,'d',1) ON CONFLICT DO UPDATE SET hits = 999 WHERE hits < 5"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT hits FROM t WHERE id = 4"), 11);  /* 11 >= 5, unchanged */

  tdb_close(db);
}

static void test_returning(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL, hits INTEGER)");

  /* INSERT ... RETURNING: a single-row projection with an expression + alias */
  tdb_stmt *s;
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "INSERT INTO t VALUES (1,'a',10) RETURNING id, hits + 1 AS next", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_count(s), 2);
  TDB_CHECK_STR(tdb_column_name(s, 0), "id");
  TDB_CHECK_STR(tdb_column_name(s, 1), "next");
  TDB_CHECK_EQ(tdb_column_int(s, 0), 1);
  TDB_CHECK_EQ(tdb_column_int(s, 1), 11);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  /* multi-row INSERT ... RETURNING * yields one row per inserted row */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "INSERT INTO t VALUES (2,'b',20),(3,'c',30) RETURNING *", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_count(s), 3);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 2);
  TDB_CHECK_STR(tdb_column_text(s, 1), "b");
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 3);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  /* the rows really landed */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 3);

  /* UPDATE ... RETURNING reflects the NEW row values */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "UPDATE t SET hits = hits + 100 WHERE id = 1 RETURNING id, hits", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 1);
  TDB_CHECK_EQ(tdb_column_int(s, 1), 110);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  /* DELETE ... RETURNING reflects the row as it was before removal */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "DELETE FROM t WHERE id = 3 RETURNING name", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "c");
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);

  /* INSERT OR REPLACE ... RETURNING surfaces the replacement row */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "INSERT OR REPLACE INTO t VALUES (1,'a2',5) RETURNING name, hits", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "a2");
  TDB_CHECK_EQ(tdb_column_int(s, 1), 5);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  tdb_close(db);
}

static void test_cte(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20),(3,30)");

  /* basic single CTE used in FROM */
  TDB_CHECK_EQ(scalar(db, "WITH big AS (SELECT v FROM t WHERE v >= 20) SELECT COUNT(*) FROM big"), 2);
  TDB_CHECK_EQ(scalar(db, "WITH big AS (SELECT v FROM t WHERE v >= 20) SELECT SUM(v) FROM big"), 50);

  /* a later CTE may reference an earlier sibling */
  TDB_CHECK_EQ(scalar(db,
    "WITH a AS (SELECT v FROM t), b AS (SELECT v*2 AS w FROM a) SELECT SUM(w) FROM b"), 120);

  /* WITH name(col, ...) column aliases */
  TDB_CHECK_EQ(scalar(db, "WITH r(x,y) AS (SELECT id, v FROM t) SELECT y FROM r WHERE x = 2"), 20);

  /* CTE joined against a base table */
  TDB_CHECK_EQ(scalar(db,
    "WITH hi AS (SELECT id FROM t WHERE v >= 20) "
    "SELECT COUNT(*) FROM t JOIN hi ON t.id = hi.id"), 2);

  /* CTE referenced inside a subquery / IN list */
  TDB_CHECK_EQ(scalar(db,
    "WITH hi AS (SELECT id FROM t WHERE v >= 20) "
    "SELECT COUNT(*) FROM t WHERE id IN (SELECT id FROM hi)"), 2);

  /* the same CTE referenced twice */
  TDB_CHECK_EQ(scalar(db,
    "WITH c AS (SELECT v FROM t) "
    "SELECT (SELECT SUM(v) FROM c) + (SELECT COUNT(*) FROM c)"), 63);

  /* a CTE name shadows a real table of the same name within the query */
  TDB_CHECK_EQ(scalar(db, "WITH t AS (SELECT 99 AS v) SELECT v FROM t"), 99);

  /* WITH ... SELECT used as a statement start (no surrounding SELECT) */
  tdb_stmt *s;
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "WITH e AS (SELECT id, v FROM t WHERE v = 30) SELECT id, v FROM e", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 3);
  TDB_CHECK_EQ(tdb_column_int(s, 1), 30);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  tdb_close(db);
}

/* scalar SELECT returning one double (rounded compare via the caller) */
static double scalar_real(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -1e300;
  double v = -1e300;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_double(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_geospatial(void) {
  const char *path = "test_geo.db";
  remove(path); remove("test_geo.db-wal");
  tdb_db *db; TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);

  TDB_CHECK_EQ(exec(db, "CREATE TABLE places (id INTEGER PRIMARY KEY, name TEXT, geom GEOMETRY)"), TDB_OK);
  exec(db, "INSERT INTO places VALUES (1,'origin','POINT(0 0)')");
  exec(db, "INSERT INTO places VALUES (2,'far','POINT(3 4)')");
  exec(db, "INSERT INTO places VALUES (3,'line','LINESTRING(0 0, 2 2, 4 0)')");
  exec(db, "INSERT INTO places VALUES (4,'sq','POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))')");

  /* WKT round-trips through the binary encoding */
  check_text(db, "SELECT ST_AsText(geom) FROM places WHERE id=1", "POINT(0 0)");
  check_text(db, "SELECT ST_AsText(geom) FROM places WHERE id=3", "LINESTRING(0 0, 2 2, 4 0)");
  check_text(db, "SELECT ST_AsText(geom) FROM places WHERE id=4", "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");

  /* coordinate accessors */
  TDB_CHECK_EQ((int)(scalar_real(db, "SELECT ST_X(geom) FROM places WHERE id=2") + 0.5), 3);
  TDB_CHECK_EQ((int)(scalar_real(db, "SELECT ST_Y(geom) FROM places WHERE id=2") + 0.5), 4);

  /* distance: 3-4-5 right triangle */
  TDB_CHECK_EQ((int)(scalar_real(db,
    "SELECT ST_Distance((SELECT geom FROM places WHERE id=1),"
    "(SELECT geom FROM places WHERE id=2))") + 0.5), 5);

  /* ST_Point constructor + ST_DWithin as a WHERE predicate */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM places WHERE ST_DWithin(geom, ST_Point(0,0), 1.0)"), 1); /* only origin */
  /* reps: origin(0,0) d=0; far(3,4) d=5; line center(2,1) d~2.24; square
  ** center(5,5) d~7.07. Within 6.0 -> origin, far, line = 3. */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM places WHERE ST_DWithin(geom, ST_Point(0,0), 6.0)"), 3);

  /* polygon containment */
  TDB_CHECK_EQ(scalar(db, "SELECT ST_Contains((SELECT geom FROM places WHERE id=4), ST_Point(5,5))"), 1);
  TDB_CHECK_EQ(scalar(db, "SELECT ST_Contains((SELECT geom FROM places WHERE id=4), ST_Point(50,50))"), 0);
  /* ST_Within is the inverse argument order */
  TDB_CHECK_EQ(scalar(db, "SELECT ST_Within(ST_Point(5,5), (SELECT geom FROM places WHERE id=4))"), 1);

  /* bounding-box intersection */
  TDB_CHECK_EQ(scalar(db,
    "SELECT ST_Intersects((SELECT geom FROM places WHERE id=3),"
    "(SELECT geom FROM places WHERE id=4))"), 1);

  /* strict typing: a POINT column rejects a non-point and bad WKT */
  exec(db, "CREATE TABLE pts (id INTEGER PRIMARY KEY, p POINT)");
  TDB_CHECK_EQ(exec(db, "INSERT INTO pts VALUES (1,'POINT(1 2)')"), TDB_OK);
  TDB_CHECK(exec(db, "INSERT INTO pts VALUES (2,'LINESTRING(0 0,1 1)')") != TDB_OK);
  TDB_CHECK(exec(db, "INSERT INTO pts VALUES (3,'POINT(nope)')") != TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM pts"), 1);
  tdb_close(db);

  /* geometry persists across reopen */
  tdb_open(path, &db);
  check_text(db, "SELECT ST_AsText(geom) FROM places WHERE id=2", "POINT(3 4)");
  TDB_CHECK_EQ((int)(scalar_real(db, "SELECT ST_X(p) FROM pts WHERE id=1") + 0.5), 1);
  tdb_close(db);
  remove(path); remove("test_geo.db-wal");
}

static void test_spatial_index(void) {
  const char *path = "test_spidx.db";
  remove(path); remove("test_spidx.db-wal");
  tdb_db *db; TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);

  TDB_CHECK_EQ(exec(db, "CREATE TABLE pts (id INTEGER PRIMARY KEY, g GEOMETRY)"), TDB_OK);
  /* a 30x30 grid of points -> 900 rows, enough to split the R-tree */
  exec(db, "BEGIN");
  for (int x = 0; x < 30; x++)
    for (int y = 0; y < 30; y++) {
      char sql[96];
      snprintf(sql, sizeof(sql), "INSERT INTO pts VALUES (%d, 'POINT(%d %d)')", x * 30 + y + 1, x, y);
      TDB_CHECK_EQ(exec(db, sql), TDB_OK);
    }
  exec(db, "COMMIT");

  /* build the spatial index AFTER the data (exercises bulk population) */
  TDB_CHECK_EQ(exec(db, "CREATE INDEX ix ON pts USING RTREE (g)"), TDB_OK);

  /* the plan must choose the spatial index */
  check_text(db,
    "EXPLAIN SELECT id FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))",
    "SEARCH pts USING INDEX ix");

  /* window [5,5]..[9,9] -> the 5x5 = 25 grid points in that block */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))"), 25);

  /* ST_DWithin window: points within 1.0 of (15,15) -> the point and its 4 axis neighbours = 5 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM pts WHERE ST_DWithin(g, ST_Point(15,15), 1.0)"), 5);

  /* a window matching nothing */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((100 100,110 100,110 110,100 110,100 100))'))"), 0);

  /* MVCC: move a point out of a window, ensure index reflects it */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM pts WHERE ST_DWithin(g, ST_Point(0,0), 0.5)"), 1); /* (0,0) */
  exec(db, "UPDATE pts SET g = 'POINT(100 100)' WHERE id = 1");                                    /* id 1 == (0,0) */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM pts WHERE ST_DWithin(g, ST_Point(0,0), 0.5)"), 0); /* moved away */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM pts WHERE ST_DWithin(g, ST_Point(100,100), 0.5)"), 1);

  /* DELETE a point inside the [5,9] block -> count drops by one.
  ** (7,7) was inserted with id = 7*30 + 7 + 1 = 218. */
  exec(db, "DELETE FROM pts WHERE id = 218");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))"), 24);
  tdb_close(db);

  /* persistence: reopen and re-query through the index */
  tdb_open(path, &db);
  check_text(db,
    "EXPLAIN SELECT id FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))",
    "SEARCH pts USING INDEX ix");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))"), 24);

  /* DROP INDEX reclaims it; queries still work via full scan */
  TDB_CHECK_EQ(exec(db, "DROP INDEX ix"), TDB_OK);
  check_text(db,
    "EXPLAIN SELECT id FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))",
    "SCAN pts");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM pts WHERE ST_Intersects(g, ST_GeomFromText('POLYGON((5 5,9 5,9 9,5 9,5 5))'))"), 24);

  /* a spatial index requires a single geometry column */
  TDB_CHECK(exec(db, "CREATE INDEX bad ON pts USING RTREE (id)") != TDB_OK);
  tdb_close(db);
  remove(path); remove("test_spidx.db-wal");
}

static void test_correlated(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE emp (id INTEGER, name TEXT, dept INTEGER, salary INTEGER)");
  exec(db, "INSERT INTO emp VALUES (1,'a',10,100),(2,'b',10,200),(3,'c',20,150),"
           "(4,'d',20,300),(5,'e',30,50)");
  exec(db, "CREATE TABLE dept (id INTEGER, name TEXT)");
  exec(db, "INSERT INTO dept VALUES (10,'eng'),(20,'sales'),(30,'hr')");

  /* correlated scalar subquery: each employee's dept average */
  TDB_CHECK_EQ(scalar(db,
    "SELECT (SELECT AVG(salary) FROM emp e2 WHERE e2.dept = e1.dept) "
    "FROM emp e1 WHERE id = 1"), 150);   /* dept 10: (100+200)/2 */
  TDB_CHECK_EQ(scalar(db,
    "SELECT (SELECT AVG(salary) FROM emp e2 WHERE e2.dept = e1.dept) "
    "FROM emp e1 WHERE id = 4"), 225);   /* dept 20: (150+300)/2 */

  /* correlated EXISTS: depts with an employee earning > 250 -> only sales */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM dept d WHERE EXISTS "
    "(SELECT 1 FROM emp e WHERE e.dept = d.id AND e.salary > 250)"), 1);
  check_text(db,
    "SELECT name FROM dept d WHERE EXISTS "
    "(SELECT 1 FROM emp e WHERE e.dept = d.id AND e.salary > 250)", "sales");

  /* correlated NOT EXISTS: the complement -> eng, hr */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM dept d WHERE NOT EXISTS "
    "(SELECT 1 FROM emp e WHERE e.dept = d.id AND e.salary > 250)"), 2);

  /* employees earning above their own dept average -> b (200>150), d (300>225) */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp e1 WHERE salary > "
    "(SELECT AVG(salary) FROM emp e2 WHERE e2.dept = e1.dept)"), 2);

  /* correlated IN: employees whose dept matches a correlated, filtered set */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp e1 WHERE dept IN "
    "(SELECT d.id FROM dept d WHERE d.id = e1.dept AND d.name = 'eng')"), 2);

  /* outer column inside a correlated subquery's SELECT list (not just WHERE) */
  check_text(db,
    "SELECT (SELECT d.name || ':' || e1.name FROM dept d WHERE d.id = e1.dept) "
    "FROM emp e1 WHERE id = 3", "sales:c");

  /* uncorrelated subquery still works unchanged */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp WHERE salary > (SELECT AVG(salary) FROM emp)"), 2);

  tdb_close(db);
}

static void test_streaming(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER, name TEXT)");
  exec(db, "BEGIN");
  for (int i = 1; i <= 100; i++) {
    char sql[96];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, 'n%d')", i, i * 10, i);
    exec(db, sql);
  }
  exec(db, "COMMIT");

  check_text(db, "SELECT name FROM t WHERE id = 7", "n7");           /* PK index pick, streamed */

  /* expression projection through the project operator */
  TDB_CHECK_EQ(scalar(db, "SELECT v * 2 FROM t WHERE id = 5"), 100);

  /* LIMIT / OFFSET through the limit operator */
  {
    tdb_stmt *s = NULL;
    TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT id FROM t WHERE v >= 200 LIMIT 3 OFFSET 2", -1, &s, NULL), TDB_OK);
    int ids[8], n = 0;
    while (tdb_step(s) == TDB_ROW && n < 8) ids[n++] = (int)tdb_column_int(s, 0);
    TDB_CHECK_EQ(n, 3);
    /* v>=200 -> id>=20; offset 2 -> start at id 22; limit 3 -> 22,23,24 */
    TDB_CHECK_EQ(ids[0], 22); TDB_CHECK_EQ(ids[1], 23); TDB_CHECK_EQ(ids[2], 24);
    tdb_finalize(s);
  }

  /* empty filter result */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT id FROM t WHERE v > 100000", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
    tdb_finalize(s);
  }

  /* SELECT * streams all columns */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT * FROM t WHERE id = 42", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_int(s, 0), 42);
    TDB_CHECK_EQ(tdb_column_int(s, 1), 420);
    TDB_CHECK_STR(tdb_column_text(s, 2), "n42");
    TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
    tdb_finalize(s);
  }

  /* re-step after reset still works (Stage 1 materializes the streamed output) */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT id FROM t WHERE id <= 3", -1, &s, NULL);
    int n1 = 0; while (tdb_step(s) == TDB_ROW) n1++;
    tdb_reset(s);
    int n2 = 0; while (tdb_step(s) == TDB_ROW) n2++;
    TDB_CHECK_EQ(n1, 3); TDB_CHECK_EQ(n2, 3);
    tdb_finalize(s);
  }

  /* columnar table streams with projection pushdown */
  exec(db, "CREATE TABLE cc (id INTEGER PRIMARY KEY, a INTEGER, b TEXT) WITH COLUMNAR");
  exec(db, "INSERT INTO cc VALUES (1,100,'x'),(2,200,'y'),(3,300,'z')");
  check_text(db, "SELECT b FROM cc WHERE id = 3", "z");

  tdb_close(db);
}

static void test_stream_snapshot(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20),(3,30)");

  /* A lazily streamed SELECT holds a read snapshot open across steps. A write
  ** committed on the same connection mid-stream must NOT become visible to the
  ** in-flight scan (snapshot isolation). */
  tdb_stmt *s = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT id FROM t", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 1);          /* first row pulled */

  /* commit an insert on the same connection while the stream is paused */
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (4,40)"), TDB_OK);

  /* the stream continues on its original snapshot: 2, 3, then DONE (never 4) */
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW); TDB_CHECK_EQ(tdb_column_int(s, 0), 2);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW); TDB_CHECK_EQ(tdb_column_int(s, 0), 3);
  TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
  tdb_finalize(s);

  /* a fresh statement sees the committed row */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 4);

  /* finalizing a partially-consumed stream releases its snapshot cleanly */
  tdb_stmt *s2 = NULL;
  tdb_prepare_v2(db, "SELECT id FROM t", -1, &s2, NULL);
  TDB_CHECK_EQ(tdb_step(s2), TDB_ROW);            /* pull one, then abandon */
  tdb_finalize(s2);

  /* reset mid-stream, then re-run to completion on a new snapshot */
  tdb_stmt *s3 = NULL;
  tdb_prepare_v2(db, "SELECT id FROM t WHERE id <= 2", -1, &s3, NULL);
  TDB_CHECK_EQ(tdb_step(s3), TDB_ROW);
  tdb_reset(s3);
  int n = 0; while (tdb_step(s3) == TDB_ROW) n++;
  TDB_CHECK_EQ(n, 2);
  tdb_finalize(s3);

  tdb_close(db);
}

/* read the single int column of a query into ids[], returning the row count */
static int collect_ints(tdb_db *db, const char *sql, int *ids, int cap) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -1;
  int n = 0;
  while (tdb_step(s) == TDB_ROW && n < cap) ids[n++] = (int)tdb_column_int(s, 0);
  tdb_finalize(s);
  return n;
}

static void test_stream_sort(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, grp INTEGER, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,1,50),(2,1,30),(3,2,30),(4,2,10),(5,1,40),(6,3,20)");
  int ids[16], n;

  /* ORDER BY ascending (streamed via the Sort operator) */
  n = collect_ints(db, "SELECT id FROM t ORDER BY v", ids, 16);
  TDB_CHECK_EQ(n, 6);
  /* v: 10,20,30,30,40,50 -> ids 4,6,(2|3),(2|3),5,1 */
  TDB_CHECK_EQ(ids[0], 4); TDB_CHECK_EQ(ids[1], 6);
  TDB_CHECK_EQ(ids[4], 5); TDB_CHECK_EQ(ids[5], 1);

  /* top-N (bounded heap): ORDER BY v DESC LIMIT 3 -> 50,40,30 = ids 1,5, then a 30 */
  n = collect_ints(db, "SELECT id FROM t ORDER BY v DESC LIMIT 3", ids, 16);
  TDB_CHECK_EQ(n, 3);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 5);
  TDB_CHECK(ids[2] == 2 || ids[2] == 3);

  /* multi-key: grp ASC, v DESC */
  n = collect_ints(db, "SELECT id FROM t ORDER BY grp, v DESC", ids, 16);
  TDB_CHECK_EQ(n, 6);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 5); TDB_CHECK_EQ(ids[2], 2);  /* grp1: 50,40,30 */
  TDB_CHECK_EQ(ids[3], 3); TDB_CHECK_EQ(ids[4], 4);                            /* grp2: 30,10 */
  TDB_CHECK_EQ(ids[5], 6);                                                     /* grp3: 20 */

  /* LIMIT + OFFSET over a sorted stream */
  n = collect_ints(db, "SELECT id FROM t ORDER BY v LIMIT 2 OFFSET 2", ids, 16);
  TDB_CHECK_EQ(n, 2);   /* asc 10,20,30,30,... offset 2 -> the two v=30 rows */
  TDB_CHECK((ids[0] == 2 || ids[0] == 3) && (ids[1] == 2 || ids[1] == 3) && ids[0] != ids[1]);

  /* filter + sort together */
  n = collect_ints(db, "SELECT id FROM t WHERE grp = 1 ORDER BY v DESC", ids, 16);
  TDB_CHECK_EQ(n, 3);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 5); TDB_CHECK_EQ(ids[2], 2);

  /* LIMIT 0 with ORDER BY yields nothing; empty filter yields nothing */
  TDB_CHECK_EQ(collect_ints(db, "SELECT id FROM t ORDER BY v LIMIT 0", ids, 16), 0);
  TDB_CHECK_EQ(collect_ints(db, "SELECT id FROM t WHERE v > 1000 ORDER BY v", ids, 16), 0);

  /* top-N larger than the row count returns all rows sorted */
  TDB_CHECK_EQ(collect_ints(db, "SELECT id FROM t ORDER BY v DESC LIMIT 100", ids, 16), 6);

  /* abandon a sorted stream after one row: the buffered heap must be freed */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT id FROM t ORDER BY v", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    tdb_finalize(s);   /* heap built, only one row emitted -> close frees the rest */
  }

  tdb_close(db);
}

static void test_stream_join(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, name TEXT, dept INTEGER)");
  exec(db, "INSERT INTO emp VALUES (1,'a',10),(2,'b',20),(3,'c',10),(4,'d',99)");
  exec(db, "CREATE TABLE dept (id INTEGER PRIMARY KEY, dname TEXT)");
  exec(db, "INSERT INTO dept VALUES (10,'eng'),(20,'sales'),(30,'hr')");
  int ids[64], n;

  /* INNER JOIN ... ON: dept 99 has no match -> 3 rows */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept = d.id"), 3);
  n = collect_ints(db,
    "SELECT e.id FROM emp e JOIN dept d ON e.dept = d.id ORDER BY e.id", ids, 64);
  TDB_CHECK_EQ(n, 3);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 2); TDB_CHECK_EQ(ids[2], 3);

  /* projected column from the inner table + ORDER BY across the join */
  check_text(db,
    "SELECT d.dname FROM emp e JOIN dept d ON e.dept = d.id ORDER BY e.id", "eng");

  /* comma join + WHERE acting as the join predicate, with an extra filter */
  n = collect_ints(db,
    "SELECT e.id FROM emp e, dept d WHERE e.dept = d.id AND d.dname = 'eng' ORDER BY e.id", ids, 64);
  TDB_CHECK_EQ(n, 2);   /* a (id1) and c (id3), both dept 10 */
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 3);

  /* CROSS JOIN: full cross product 4 x 3 = 12 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp CROSS JOIN dept"), 12);

  /* join + LIMIT streams with early termination */
  n = collect_ints(db,
    "SELECT e.id FROM emp e JOIN dept d ON e.dept = d.id ORDER BY e.id LIMIT 2", ids, 64);
  TDB_CHECK_EQ(n, 2);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 2);

  /* SELECT * across both tables yields all columns of both */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT * FROM emp e JOIN dept d ON e.dept = d.id WHERE e.id = 1", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_count(s), 5);            /* emp(3) + dept(2) */
    TDB_CHECK_EQ(tdb_column_int(s, 0), 1);           /* emp.id */
    TDB_CHECK_STR(tdb_column_text(s, 1), "a");       /* emp.name */
    TDB_CHECK_EQ(tdb_column_int(s, 3), 10);          /* dept.id */
    TDB_CHECK_STR(tdb_column_text(s, 4), "eng");     /* dept.dname */
    TDB_CHECK_EQ(tdb_step(s), TDB_DONE);
    tdb_finalize(s);
  }

  /* three-table left-deep join: a(10),c(10) each match p1,p2 (4 rows);
  ** b(20) matches p3 (1 row) -> 5 total */
  exec(db, "CREATE TABLE proj (dept INTEGER, pname TEXT)");
  exec(db, "INSERT INTO proj VALUES (10,'p1'),(10,'p2'),(20,'p3')");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept = d.id "
    "JOIN proj p ON p.dept = d.id"), 5);

  /* snapshot isolation holds across a paused join stream */
  tdb_stmt *s = NULL;
  tdb_prepare_v2(db, "SELECT e.id FROM emp e JOIN dept d ON e.dept = d.id ORDER BY e.id", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 1);
  exec(db, "INSERT INTO dept VALUES (99,'late')");   /* would make emp 4 match */
  int more = 0; while (tdb_step(s) == TDB_ROW) more++;
  TDB_CHECK_EQ(more, 2);   /* still only 2,3 — the late dept is invisible */
  tdb_finalize(s);

  tdb_close(db);
}

static void test_stream_join_index(void) {
  /* An index on the inner join column turns the per-outer full scan into a
  ** seek; results must match the unindexed plan exactly. The ON predicate is
  ** re-checked per row, so duplicate index keys and NULL probes are handled. */
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, dept INTEGER)");
  exec(db, "INSERT INTO emp VALUES (1,10),(2,20),(3,10),(4,99),(5,NULL)");
  exec(db, "CREATE TABLE dept (did INTEGER, dname TEXT)");
  exec(db, "INSERT INTO dept VALUES (10,'eng'),(20,'sales'),(10,'eng2')");
  int ids[64], n;

  /* baseline (no index): emp 1->{eng,eng2}, 2->{sales}, 3->{eng,eng2},
  ** 4->none, 5(NULL)->none  =>  5 rows */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept = d.did"), 5);

  /* now index the inner column and re-run: identical result via the seek path */
  exec(db, "CREATE INDEX dept_did ON dept(did)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept = d.did"), 5);

  /* row-level check: emp 1 matches both dept rows with did=10 */
  n = collect_ints(db,
    "SELECT e.id FROM emp e JOIN dept d ON e.dept = d.did ORDER BY e.id, d.dname", ids, 64);
  TDB_CHECK_EQ(n, 5);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 1);   /* eng, eng2 */
  TDB_CHECK_EQ(ids[2], 2);                            /* sales */
  TDB_CHECK_EQ(ids[3], 3); TDB_CHECK_EQ(ids[4], 3);   /* eng, eng2 */

  /* reversed ON (inner column on the left) still uses the index */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp e JOIN dept d ON d.did = e.dept"), 5);

  /* an extra non-equijoin conjunct: index seeks on the equijoin, ON re-checks */
  n = collect_ints(db,
    "SELECT e.id FROM emp e JOIN dept d ON e.dept = d.did AND d.dname = 'eng' ORDER BY e.id", ids, 64);
  TDB_CHECK_EQ(n, 2);   /* only the 'eng' dept row; emp 1 and 3 */
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 3);

  /* three-table join with the middle table indexed on its join key */
  exec(db, "CREATE TABLE loc (dept INTEGER, city TEXT)");
  exec(db, "CREATE INDEX loc_dept ON loc(dept)");
  exec(db, "INSERT INTO loc VALUES (10,'NYC'),(20,'SF')");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp e JOIN dept d ON e.dept = d.did "
    "JOIN loc l ON l.dept = d.did"), 5);   /* same 5 matched emp/dept rows, each one loc */

  tdb_close(db);
}

static void test_stream_left_join(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, dept INTEGER)");
  exec(db, "INSERT INTO emp VALUES (1,10),(2,20),(3,99),(4,NULL)");
  exec(db, "CREATE TABLE dept (did INTEGER, dname TEXT)");
  exec(db, "INSERT INTO dept VALUES (10,'eng'),(20,'sales')");
  int ids[16], n;

  /* every outer row is preserved: 4 emp rows, two unmatched -> still 4 */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM emp e LEFT JOIN dept d ON e.dept = d.did"), 4);

  /* unmatched outer rows carry NULL inner columns: COUNT(d.did) skips them */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(d.did) FROM emp e LEFT JOIN dept d ON e.dept = d.did"), 2);

  /* anti-join idiom: outer rows with no match (dept 99 and NULL) -> ids 3,4 */
  n = collect_ints(db,
    "SELECT e.id FROM emp e LEFT JOIN dept d ON e.dept = d.did WHERE d.did IS NULL ORDER BY e.id", ids, 16);
  TDB_CHECK_EQ(n, 2);
  TDB_CHECK_EQ(ids[0], 3); TDB_CHECK_EQ(ids[1], 4);

  /* matched rows still project the inner value */
  check_text(db,
    "SELECT d.dname FROM emp e LEFT JOIN dept d ON e.dept = d.did WHERE e.id = 1", "eng");

  /* index-driven LEFT JOIN with duplicate inner keys: emp 1 (dept 10) matches
  ** two dept rows, the unmatched/NULL-dept emps each emit one NULL row */
  exec(db, "CREATE TABLE d2 (did INTEGER, dname TEXT)");
  exec(db, "CREATE INDEX d2i ON d2(did)");
  exec(db, "INSERT INTO d2 VALUES (10,'eng'),(10,'eng2'),(20,'sales')");
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM emp e LEFT JOIN d2 d ON e.dept = d.did"), 5);  /* 2+1+1+1 */

  /* LEFT JOIN feeding GROUP BY: the unmatched group counts 0 inner rows */
  TDB_CHECK_EQ(scalar(db,
    "SELECT c FROM (SELECT e.dept dp, COUNT(d.did) c FROM emp e LEFT JOIN dept d ON e.dept=d.did "
    "GROUP BY e.dept) WHERE dp = 99"), 0);

  /* chained LEFT JOIN: a missing middle row NULLs the rest of the chain */
  exec(db, "CREATE TABLE c3 (cid INTEGER, v TEXT)");
  exec(db, "INSERT INTO c3 VALUES (10,'hit')");
  n = collect_ints(db,
    "SELECT e.id FROM emp e LEFT JOIN dept d ON e.dept=d.did "
    "LEFT JOIN c3 c ON d.did=c.cid ORDER BY e.id", ids, 16);
  TDB_CHECK_EQ(n, 4);   /* all four emp rows survive both left joins */

  tdb_close(db);
}

static void test_stream_agg(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, grp INTEGER, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,1,10),(2,1,20),(3,2,30),(4,2,40),(5,3,50)");
  int ids[16], n;

  /* scalar aggregates with no GROUP BY -> one row */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 5);
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(v) FROM t"), 150);
  TDB_CHECK_EQ(scalar(db, "SELECT MIN(v) FROM t"), 10);
  TDB_CHECK_EQ(scalar(db, "SELECT MAX(v) FROM t"), 50);
  TDB_CHECK_EQ((int)(scalar_real(db, "SELECT AVG(v) FROM t") + 0.5), 30);

  /* scalar aggregate over an empty input still yields one row */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE v > 1000"), 0);

  /* WHERE applies before grouping */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t WHERE v > 25"), 3);

  /* GROUP BY: one row per group */
  n = collect_ints(db, "SELECT grp FROM t GROUP BY grp ORDER BY grp", ids, 16);
  TDB_CHECK_EQ(n, 3);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 2); TDB_CHECK_EQ(ids[2], 3);
  /* per-group SUM via the grp=2 row */
  TDB_CHECK_EQ(scalar(db, "SELECT s FROM (SELECT grp, SUM(v) AS s FROM t GROUP BY grp) WHERE grp = 2"), 70);

  /* HAVING filters grouped rows */
  n = collect_ints(db,
    "SELECT grp FROM t GROUP BY grp HAVING SUM(v) > 30 ORDER BY grp", ids, 16);
  TDB_CHECK_EQ(n, 2);   /* grp 2 (70) and grp 3 (50); grp 1 (30) excluded */
  TDB_CHECK_EQ(ids[0], 2); TDB_CHECK_EQ(ids[1], 3);

  /* GROUP BY + ORDER BY on an aggregate + LIMIT */
  n = collect_ints(db,
    "SELECT grp FROM t GROUP BY grp ORDER BY SUM(v) DESC LIMIT 2", ids, 16);
  TDB_CHECK_EQ(n, 2);
  TDB_CHECK_EQ(ids[0], 2); TDB_CHECK_EQ(ids[1], 3);   /* 70, 50 */

  /* COUNT(DISTINCT ...) */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(DISTINCT grp) FROM t"), 3);

  /* aggregate over a join, grouped by an inner-table column */
  exec(db, "CREATE TABLE emp (id INTEGER PRIMARY KEY, dept INTEGER, sal INTEGER)");
  exec(db, "INSERT INTO emp VALUES (1,10,100),(2,10,200),(3,20,300)");
  exec(db, "CREATE TABLE dept (did INTEGER, dname TEXT)");
  exec(db, "INSERT INTO dept VALUES (10,'eng'),(20,'sales')");
  TDB_CHECK_EQ(scalar(db,
    "SELECT t FROM (SELECT d.dname dn, SUM(e.sal) t FROM emp e JOIN dept d ON e.dept=d.did "
    "GROUP BY d.dname) WHERE dn = 'eng'"), 300);

  /* abandon a partially consumed aggregate stream (groups buffered, freed) */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT grp, SUM(v) FROM t GROUP BY grp ORDER BY grp", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    tdb_finalize(s);
  }

  tdb_close(db);
}

static void test_stream_distinct(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, grp INTEGER, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,1,10),(2,1,10),(3,2,30),(4,2,30),(5,3,30)");
  int ids[16], n;

  /* single-column DISTINCT preserves first-seen order */
  n = collect_ints(db, "SELECT DISTINCT grp FROM t", ids, 16);
  TDB_CHECK_EQ(n, 3);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 2); TDB_CHECK_EQ(ids[2], 3);

  n = collect_ints(db, "SELECT DISTINCT v FROM t", ids, 16);
  TDB_CHECK_EQ(n, 2);   /* 10, 30 */
  TDB_CHECK_EQ(ids[0], 10); TDB_CHECK_EQ(ids[1], 30);

  /* multi-column DISTINCT: (1,10),(2,30),(3,30) = 3 distinct rows */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT DISTINCT grp, v FROM t)"), 3);

  /* LIMIT applies to the deduplicated stream */
  n = collect_ints(db, "SELECT DISTINCT grp FROM t LIMIT 2", ids, 16);
  TDB_CHECK_EQ(n, 2);
  TDB_CHECK_EQ(ids[0], 1); TDB_CHECK_EQ(ids[1], 2);

  /* DISTINCT after a WHERE filter */
  n = collect_ints(db, "SELECT DISTINCT v FROM t WHERE grp >= 2", ids, 16);
  TDB_CHECK_EQ(n, 1);
  TDB_CHECK_EQ(ids[0], 30);

  /* NULLs compare equal: {NULL,NULL,5,5} -> 2 distinct values */
  exec(db, "CREATE TABLE n (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO n VALUES (1,NULL),(2,NULL),(3,5),(4,5)");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM (SELECT DISTINCT v FROM n)"), 2);

  /* DISTINCT over a join */
  exec(db, "CREATE TABLE a (id INTEGER PRIMARY KEY, k INTEGER)");
  exec(db, "CREATE TABLE b (k INTEGER, tag INTEGER)");
  exec(db, "INSERT INTO a VALUES (1,100),(2,100),(3,200)");
  exec(db, "INSERT INTO b VALUES (100,7),(200,7)");
  /* a.k -> b.tag: rows (1,7),(2,7),(3,7); DISTINCT tag = 1 */
  TDB_CHECK_EQ(scalar(db,
    "SELECT COUNT(*) FROM (SELECT DISTINCT b.tag FROM a JOIN b ON a.k = b.k)"), 1);

  /* abandon a partially consumed distinct stream (seen-set freed) */
  {
    tdb_stmt *s = NULL;
    tdb_prepare_v2(db, "SELECT DISTINCT grp FROM t", -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    tdb_finalize(s);
  }

  tdb_close(db);
}

static void test_hash_partitioning(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);

  /* CREATE TABLE ... PARTITION BY HASH (id) PARTITIONS 4 spawns 4 child
  ** tables named "evt__p0..3". */
  TDB_CHECK_EQ(exec(db,
    "CREATE TABLE evt (id INTEGER PRIMARY KEY, k TEXT) "
    "PARTITION BY HASH (id) PARTITIONS 4"), TDB_OK);
  for (int i = 0; i < 4; i++) {
    char q[80]; snprintf(q, sizeof(q), "SELECT COUNT(*) FROM evt__p%d", i);
    TDB_CHECK_EQ(scalar(db, q), 0);
  }

  /* Insert 12 rows; HASH should distribute them across the 4 children. */
  for (int i = 1; i <= 12; i++) {
    char q[100]; snprintf(q, sizeof(q), "INSERT INTO evt VALUES (%d, 'r%d')", i, i);
    TDB_CHECK_EQ(exec(db, q), TDB_OK);
  }
  /* Parent SELECT unions all children. */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM evt"), 12);
  /* Children are non-empty and total back up to 12. */
  int64_t sum = 0;
  for (int i = 0; i < 4; i++) {
    char q[80]; snprintf(q, sizeof(q), "SELECT COUNT(*) FROM evt__p%d", i);
    int64_t n = scalar(db, q);
    TDB_CHECK(n > 0);
    sum += n;
  }
  TDB_CHECK_EQ(sum, 12);

  /* DELETE WHERE id=N — applies to the correct child, parent SELECT reflects it. */
  TDB_CHECK_EQ(exec(db, "DELETE FROM evt WHERE id = 3"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM evt"), 11);

  /* UPDATE k — value visible through parent SELECT regardless of which child holds the row */
  TDB_CHECK_EQ(exec(db, "UPDATE evt SET k = 'X' WHERE id = 5"), TDB_OK);
  tdb_stmt *s = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT k FROM evt WHERE id = 5", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_STR(tdb_column_text(s, 0), "X");
  tdb_finalize(s);

  /* TRUNCATE empties every child. */
  TDB_CHECK_EQ(exec(db, "TRUNCATE TABLE evt"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM evt"), 0);
  tdb_close(db);
}

static void test_acl_grant_revoke(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (1,10),(2,20)"), TDB_OK);

  /* open mode: no user set, all ops allowed */
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);

  /* set a user with no grants — every op is denied */
  tdb_set_user(db, "alice");
  TDB_CHECK_STR(tdb_get_user(db), "alice");
  TDB_CHECK(exec(db, "SELECT * FROM t") != TDB_OK);
  TDB_CHECK(exec(db, "INSERT INTO t VALUES (3,30)") != TDB_OK);

  /* grant SELECT only — reads ok, writes still denied */
  tdb_set_user(db, NULL);
  TDB_CHECK_EQ(exec(db, "GRANT SELECT ON TABLE t TO alice"), TDB_OK);
  tdb_set_user(db, "alice");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);
  TDB_CHECK(exec(db, "INSERT INTO t VALUES (3,30)") != TDB_OK);

  /* grant the rest (comma-list parsing) */
  tdb_set_user(db, NULL);
  TDB_CHECK_EQ(exec(db, "GRANT INSERT, UPDATE, DELETE ON TABLE t TO alice"), TDB_OK);
  tdb_set_user(db, "alice");
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (3,30)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "UPDATE t SET v = v + 1"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "DELETE FROM t WHERE id = 1"), TDB_OK);

  /* revoke INSERT and verify */
  tdb_set_user(db, NULL);
  TDB_CHECK_EQ(exec(db, "REVOKE INSERT ON TABLE t FROM alice"), TDB_OK);
  tdb_set_user(db, "alice");
  TDB_CHECK(exec(db, "INSERT INTO t VALUES (9,90)") != TDB_OK);

  /* GRANT ALL — broad wildcard */
  tdb_set_user(db, NULL);
  TDB_CHECK_EQ(exec(db, "GRANT ALL ON TABLE t TO alice"), TDB_OK);
  tdb_set_user(db, "alice");
  TDB_CHECK_EQ(exec(db, "INSERT INTO t VALUES (9,90)"), TDB_OK);

  /* PUBLIC grant is honored for any user */
  tdb_set_user(db, NULL);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE u (id INTEGER PRIMARY KEY)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "GRANT SELECT ON TABLE u TO PUBLIC"), TDB_OK);
  tdb_set_user(db, "bob");
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM u"), 0);
  tdb_close(db);

  /* persistence: a GRANT survives reopening the database file */
  const char *path = "test_acl_persist.db";
  remove(path); remove("test_acl_persist.db-wal");
  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE x (v INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO x VALUES (7)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "GRANT SELECT ON TABLE x TO carol"), TDB_OK);
  tdb_close(db);

  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  tdb_set_user(db, "carol");
  TDB_CHECK_EQ(scalar(db, "SELECT v FROM x"), 7);
  tdb_set_user(db, "dave");                 /* unrelated user — still denied */
  TDB_CHECK(exec(db, "SELECT v FROM x") != TDB_OK);
  tdb_close(db);

  /* persistence: a REVOKE also survives reopening (no zombie grants on disk) */
  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "REVOKE SELECT ON TABLE x FROM carol"), TDB_OK);
  tdb_close(db);

  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  tdb_set_user(db, "carol");
  TDB_CHECK(exec(db, "SELECT v FROM x") != TDB_OK);  /* GRANT was reclaimed */
  tdb_close(db);
  remove(path); remove("test_acl_persist.db-wal");
}

static void test_window_functions(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE s (region TEXT, q INTEGER, amt INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db,
    "INSERT INTO s VALUES ('east',1,10),('east',2,20),('east',3,30),"
    "('west',1,5),('west',2,15),('west',3,25)"), TDB_OK);

  /* ROW_NUMBER per partition, ordered by q */
  tdb_stmt *st = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "SELECT region, q, ROW_NUMBER() OVER (PARTITION BY region ORDER BY q) "
    "FROM s ORDER BY region, q", -1, &st, NULL), TDB_OK);
  int counters[2] = {0, 0};
  while (tdb_step(st) == TDB_ROW) {
    const char *region = tdb_column_text(st, 0);
    int rn = tdb_column_int(st, 2);
    TDB_CHECK(region && (region[0] == 'e' || region[0] == 'w'));
    int *idx = region[0] == 'e' ? &counters[0] : &counters[1];
    (*idx)++;
    TDB_CHECK_EQ(rn, *idx);
  }
  TDB_CHECK_EQ(counters[0], 3);
  TDB_CHECK_EQ(counters[1], 3);
  tdb_finalize(st);

  /* Running SUM per partition ordered by q: east 10,30,60; west 5,20,45 */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "SELECT region, q, SUM(amt) OVER (PARTITION BY region ORDER BY q) "
    "FROM s ORDER BY region, q", -1, &st, NULL), TDB_OK);
  int expected[6] = {10, 30, 60, 5, 20, 45};
  for (int i = 0; i < 6; i++) {
    TDB_CHECK_EQ(tdb_step(st), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_int(st, 2), expected[i]);
  }
  tdb_finalize(st);

  /* Full-partition SUM (no ORDER BY) — same total for each row in the partition */
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "SELECT region, SUM(amt) OVER (PARTITION BY region) "
    "FROM s ORDER BY region", -1, &st, NULL), TDB_OK);
  while (tdb_step(st) == TDB_ROW) {
    const char *r = tdb_column_text(st, 0);
    int total = tdb_column_int(st, 1);
    TDB_CHECK(total == (r[0] == 'e' ? 60 : 45));
  }
  tdb_finalize(st);

  /* RANK / DENSE_RANK with ties */
  TDB_CHECK_EQ(exec(db, "CREATE TABLE scores (id INTEGER, v INTEGER)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO scores VALUES (1,10),(2,20),(3,20),(4,30)"), TDB_OK);
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "SELECT id, RANK() OVER (ORDER BY v), DENSE_RANK() OVER (ORDER BY v) "
    "FROM scores ORDER BY id", -1, &st, NULL), TDB_OK);
  int rk[]    = {1, 2, 2, 4};
  int dense[] = {1, 2, 2, 3};
  for (int i = 0; i < 4; i++) {
    TDB_CHECK_EQ(tdb_step(st), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_int(st, 1), rk[i]);
    TDB_CHECK_EQ(tdb_column_int(st, 2), dense[i]);
  }
  tdb_finalize(st);
  tdb_close(db);
}

static void test_recursive_cte(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);

  /* Generate-series: 1..5. SUM = 15. */
  TDB_CHECK_EQ(scalar(db,
    "WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL "
    "  SELECT n+1 FROM t WHERE n < 5) "
    "SELECT SUM(n) FROM t"), 15);

  /* Bounded factorial — exercises UNION ALL termination and frontier semantics. */
  TDB_CHECK_EQ(scalar(db,
    "WITH RECURSIVE f(i, v) AS ( "
    "  SELECT 1, 1 UNION ALL "
    "  SELECT i+1, v*(i+1) FROM f WHERE i < 6) "
    "SELECT v FROM f WHERE i = 6"), 720);

  /* Graph reachability with UNION (DISTINCT) — must terminate even when
  ** the recursion would otherwise revisit nodes. */
  TDB_CHECK_EQ(exec(db, "CREATE TABLE edges (src TEXT, dst TEXT)"), TDB_OK);
  TDB_CHECK_EQ(exec(db,
    "INSERT INTO edges VALUES ('a','b'),('b','c'),('c','d'),('d','b')"), TDB_OK);
  TDB_CHECK_EQ(scalar(db,
    "WITH RECURSIVE reach(node) AS ( "
    "  SELECT 'a' UNION "
    "  SELECT e.dst FROM edges e JOIN reach r ON e.src = r.node) "
    "SELECT COUNT(*) FROM reach"), 4);
  tdb_close(db);
}

static void test_phase11_utility(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec(db,
    "CREATE TABLE log (id INTEGER PRIMARY KEY, msg TEXT) "
    "TABLESPACE warm "
    "WITH COMPRESSION=zstd "
    "PARTITION BY HASH (id)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "INSERT INTO log VALUES (1,'a'),(2,'b'),(3,'c')"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM log"), 3);

  /* TRUNCATE empties the table */
  TDB_CHECK_EQ(exec(db, "TRUNCATE TABLE log"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM log"), 0);

  /* ANALYZE / REINDEX are accepted; they do not error on a valid table even
  ** though their internal effects are stubbed. */
  TDB_CHECK_EQ(exec(db, "ANALYZE log"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "REINDEX TABLE log"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "LOCK TABLE log IN EXCLUSIVE MODE"), TDB_OK);

  /* COMMENT validates the target object exists and persists the body. */
  TDB_CHECK_EQ(exec(db, "COMMENT ON TABLE log IS 'truncated'"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "COMMENT ON COLUMN log.msg IS 'message text'"), TDB_OK);
  TDB_CHECK(exec(db, "COMMENT ON TABLE nope IS 'x'")   != TDB_OK);
  TDB_CHECK(exec(db, "COMMENT ON COLUMN log.nope IS 'x'") != TDB_OK);
  TDB_CHECK(exec(db, "COMMENT ON COLUMN bareword IS 'x'") != TDB_OK);
  /* Replacing an existing comment, and clearing with NULL, both succeed. */
  TDB_CHECK_EQ(exec(db, "COMMENT ON TABLE log IS 'second'"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "COMMENT ON COLUMN log.msg IS NULL"), TDB_OK);

  /* CREATE TABLESPACE / DROP TABLESPACE are parsed and accepted. */
  TDB_CHECK_EQ(exec(db, "CREATE TABLESPACE warm LOCATION '/tmp/warm'"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "DROP TABLESPACE warm"), TDB_OK);

  /* Unknown target rejected */
  TDB_CHECK(exec(db, "TRUNCATE TABLE nope") != TDB_OK);
  tdb_close(db);

  /* COMMENT persistence survives a close/reopen. */
  const char *path = "test_phase11_comment.db";
  remove(path); remove("test_phase11_comment.db-wal");
  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE TABLE notes (id INTEGER PRIMARY KEY, t TEXT)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "COMMENT ON TABLE notes IS 'persistent'"), TDB_OK);
  tdb_close(db);

  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  /* clearing the body removes the row; setting it again writes a fresh one */
  TDB_CHECK_EQ(exec(db, "COMMENT ON TABLE notes IS NULL"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "COMMENT ON TABLE notes IS 'restored'"), TDB_OK);
  tdb_close(db);
  remove(path); remove("test_phase11_comment.db-wal");
}

static tdb_test_case cases[] = {
  {"geospatial", test_geospatial},
  {"spatial_index", test_spatial_index},
  {"correlated", test_correlated},
  {"streaming", test_streaming},
  {"stream_snapshot", test_stream_snapshot},
  {"stream_sort", test_stream_sort},
  {"stream_join", test_stream_join},
  {"stream_join_index", test_stream_join_index},
  {"stream_left_join", test_stream_left_join},
  {"stream_agg", test_stream_agg},
  {"stream_distinct", test_stream_distinct},
  {"upsert", test_upsert},
  {"returning", test_returning},
  {"cte", test_cte},
  {"ddl_dml_select", test_ddl_dml_select},
  {"hash_partitioning", test_hash_partitioning},
  {"acl_grant_revoke", test_acl_grant_revoke},
  {"window_functions", test_window_functions},
  {"recursive_cte", test_recursive_cte},
  {"phase11_utility", test_phase11_utility},
  {"derived_and_view", test_derived_and_view},
  {"outer_joins", test_outer_joins},
  {"setops", test_setops},
  {"explain_vacuum", test_explain_vacuum},
  {"projection_pushdown", test_projection_pushdown},
  {"alter", test_alter},
  {"alter_persist", test_alter_persist},
  {"drop_column", test_drop_column},
  {"drop_column_persist", test_drop_column_persist},
  {"drop_index", test_drop_index},
  {"drop_view", test_drop_view},
  {"drop_table_reclaim", test_drop_table_reclaim},
  {"drop_table_persist", test_drop_table_persist},
  {"vacuum_shrinks", test_vacuum_shrinks},
  {"index_scan", test_index_scan},
  {"index_persist", test_index_persist},
  {"savepoints", test_savepoints},
  {"nested_savepoints", test_nested_savepoints},
  {"temporal", test_temporal},
  {"columnar", test_columnar},
  {"columnar_temporal_persist", test_columnar_temporal_persist},
  {"like_distinct", test_like_distinct},
  {"glob_escape", test_glob_escape},
  {"bitwise", test_bitwise},
  {"builtins", test_builtins},
  {"subqueries", test_subqueries},
  {"insert_select", test_insert_select},
  {"strict_typing", test_strict_typing},
  {"generated_column", test_generated_column},
  {"join_and_group", test_join_and_group},
  {"txn_and_persist", test_txn_and_persist},
  {"bind_params", test_bind_params},
};
TDB_MAIN(cases)
