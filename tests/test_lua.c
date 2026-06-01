/* test_lua.c — embedded Lua scalar functions, procedures, and persistence. */
#include "tdb_test.h"
#include "transactiondb.h"

#include <stdio.h>

static int exec(tdb_db *db, const char *sql) {
  char *err = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &err);
  if (rc != TDB_OK) fprintf(stderr, "  exec failed: %s -> %s\n", sql, err ? err : tdb_errmsg(db));
  tdb_free(err);
  return rc;
}
static int64_t scalar(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return -999;
  int64_t v = -1;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_scalar_function(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  TDB_CHECK_EQ(exec(db, "CREATE FUNCTION dbl(x) LANGUAGE LUA AS $$ return x*2 $$"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CREATE FUNCTION addup(a, b) LANGUAGE LUA AS $$ return a + b $$"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT dbl(21)"), 42);
  TDB_CHECK_EQ(scalar(db, "SELECT addup(2, 3)"), 5);
  /* used in a WHERE over a table */
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec(db, "INSERT INTO t VALUES (1,10),(2,20),(3,30)");
  TDB_CHECK_EQ(scalar(db, "SELECT SUM(v) FROM t WHERE dbl(v) > 30"), 50); /* v=20,30 */
  tdb_close(db);
}

static void test_nested_query(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, n TEXT)");
  exec(db, "INSERT INTO t VALUES (1,'a'),(2,'b')");
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION rowcount() LANGUAGE LUA AS $$ "
    "local r = tdb.query('SELECT COUNT(*) AS c FROM t'); return r[1].c $$"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT rowcount()"), 2);
  tdb_close(db);
}

static void test_procedure(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, n TEXT)");
  TDB_CHECK_EQ(exec(db,
    "CREATE PROCEDURE addrow(i) LANGUAGE LUA AS $$ "
    "tdb.exec('INSERT INTO t VALUES (' .. i .. \", 'x')\") $$"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CALL addrow(7)"), TDB_OK);
  TDB_CHECK_EQ(exec(db, "CALL addrow(8)"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t"), 2);
  TDB_CHECK_EQ(scalar(db, "SELECT id FROM t WHERE id = 8"), 8);
  tdb_close(db);
}

/* Lua's tdb.prepare/bind/step/finalize: a stored procedure that drives a
** parameterized prepared statement from inside a Lua routine. */
static void test_prepared_in_lua(void) {
  tdb_db *db; tdb_open(":memory:", &db);
  exec(db, "CREATE TABLE accts (id INTEGER PRIMARY KEY, owner TEXT, balance INTEGER)");
  exec(db, "INSERT INTO accts VALUES (1,'alice',100),(2,'bob',50),(3,'carol',75)");

  /* sum_for(owner_name) — Lua-side prepared statement with a bind */
  TDB_CHECK_EQ(exec(db,
    "CREATE FUNCTION sum_for(name) LANGUAGE LUA AS $$ "
    "  local s = tdb.prepare('SELECT balance FROM accts WHERE owner = ?') "
    "  s:bind(1, name) "
    "  local total = 0 "
    "  while true do local r = s:step(); if r == nil then break end; total = total + r.balance end "
    "  s:finalize() "
    "  return total $$"), TDB_OK);
  TDB_CHECK_EQ(scalar(db, "SELECT sum_for('alice')"), 100);
  TDB_CHECK_EQ(scalar(db, "SELECT sum_for('bob')"), 50);
  tdb_close(db);
}

static void test_persist_function(void) {
  const char *path = "test_lua.db";
  remove(path); remove("test_lua.db-wal");
  tdb_db *db; tdb_open(path, &db);
  exec(db, "CREATE FUNCTION triple(x) LANGUAGE LUA AS $$ return x*3 $$");
  tdb_close(db);

  tdb_open(path, &db);
  TDB_CHECK_EQ(scalar(db, "SELECT triple(5)"), 15); /* routine reloaded on open */
  tdb_close(db);
  remove(path); remove("test_lua.db-wal");
}

static tdb_test_case cases[] = {
  {"scalar_function", test_scalar_function},
  {"nested_query", test_nested_query},
  {"procedure", test_procedure},
  {"prepared_in_lua", test_prepared_in_lua},
  {"persist_function", test_persist_function},
};
TDB_MAIN(cases)
