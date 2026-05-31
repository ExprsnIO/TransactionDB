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

static tdb_test_case cases[] = {
  {"upsert", test_upsert},
  {"returning", test_returning},
  {"cte", test_cte},
  {"ddl_dml_select", test_ddl_dml_select},
  {"derived_and_view", test_derived_and_view},
  {"outer_joins", test_outer_joins},
  {"setops", test_setops},
  {"explain_vacuum", test_explain_vacuum},
  {"projection_pushdown", test_projection_pushdown},
  {"alter", test_alter},
  {"alter_persist", test_alter_persist},
  {"drop_column", test_drop_column},
  {"drop_column_persist", test_drop_column_persist},
  {"drop_table_reclaim", test_drop_table_reclaim},
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
