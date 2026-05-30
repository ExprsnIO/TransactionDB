/* test_cpp.cpp — exercises the header-only C++ RAII wrapper. */
#include "tdb_test.h"
#include "transactiondb.hpp"

static void test_basic(void) {
  tdb::Database db(":memory:");
  db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)");

  auto ins = db.prepare("INSERT INTO t VALUES (?, ?, ?)");
  ins.bind(1, 1).bind(2, "alice").bind(3, 9.5);
  TDB_CHECK(ins.step() == false);   /* DONE, no rows */
  ins.reset();
  ins.bind(1, 2).bind(2, std::string_view("bob")).bind(3, 7.0);
  ins.step();

  auto q = db.prepare("SELECT name, score FROM t WHERE id = ?");
  q.bind(1, 1);
  TDB_CHECK(q.step());
  std::string nm = q.getText(0);
  TDB_CHECK_STR(nm.c_str(), "alice");
  TDB_CHECK(q.getDouble(1) > 9.4 && q.getDouble(1) < 9.6);
  TDB_CHECK(q.step() == false);
  TDB_CHECK_EQ(q.columnCount(), 2);
  std::string cn = q.columnName(0);
  TDB_CHECK_STR(cn.c_str(), "name");
}

static void test_aggregate_and_move(void) {
  tdb::Database db(":memory:");
  db.exec("CREATE TABLE n (id INTEGER PRIMARY KEY, v INTEGER)");
  db.exec("INSERT INTO n VALUES (1,10),(2,20),(3,30)");

  /* move construction */
  tdb::Database db2(std::move(db));
  auto q = db2.prepare("SELECT SUM(v), COUNT(*) FROM n");
  TDB_CHECK(q.step());
  TDB_CHECK_EQ(q.getInt(0), 60);
  TDB_CHECK_EQ(q.getInt(1), 3);
}

static void test_error_throws(void) {
  tdb::Database db(":memory:");
  int threw = 0;
  try { db.exec("SELECT * FROM nope"); }
  catch (const tdb::Error &e) { threw = 1; (void)e.code; }
  TDB_CHECK(threw);
}

static tdb_test_case cases[] = {
  {"basic", test_basic},
  {"aggregate_and_move", test_aggregate_and_move},
  {"error_throws", test_error_throws},
};
TDB_MAIN(cases)
