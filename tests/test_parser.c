/* test_parser.c — recursive-descent parser AST checks. */
#include "tdb_test.h"
#include "../src/sql/tdb_parser.h"
#include "../src/common/tdb_mem.h"

#include <string.h>

static tdb_ast_stmt *parse1(tdb_arena *a, const char *sql) {
  tdb_ast_stmt *s = NULL; char *err = NULL; const char *tail = NULL;
  int rc = tdb_parse(a, sql, &s, &err, &tail);
  TDB_CHECK_EQ(rc, TDB_OK);
  return s;
}

/* render the first projection expression of "SELECT <expr>" */
static void check_expr(const char *sql, const char *expect) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s = parse1(a, sql);
  if (s && s->kind == ST_SELECT && s->u.select->cols->n > 0) {
    tdb_buf b; tdb_buf_init(&b);
    tdb_expr_debug(&b, s->u.select->cols->items[0]);
    tdb_buf_putc(&b, '\0');
    TDB_CHECK_STR((const char *)b.data, expect);
    tdb_buf_free(&b);
  } else {
    TDB_CHECK(0);
  }
  tdb_arena_free(a);
}

static void test_expr_precedence(void) {
  check_expr("SELECT 1+2*3", "(1 + (2 * 3))");
  check_expr("SELECT (1+2)*3", "((1 + 2) * 3)");
  check_expr("SELECT a OR b AND c", "(a OR (b AND c))");
  check_expr("SELECT NOT a = b", "(NOT (a = b))");
  check_expr("SELECT a || b || c", "((a || b) || c)");
  check_expr("SELECT -a + b", "((- a) + b)");
}

static void test_select(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s = parse1(a,
    "SELECT a, b AS x FROM t JOIN u ON t.id = u.id "
    "WHERE a > 1 GROUP BY a HAVING c < 2 ORDER BY a DESC LIMIT 10 OFFSET 5");
  TDB_CHECK_EQ(s->kind, ST_SELECT);
  tdb_select *q = s->u.select;
  TDB_CHECK_EQ(q->cols->n, 2);
  TDB_CHECK_STR(q->cols->aliases[1], "x");
  TDB_CHECK(q->from != NULL && q->from->next != NULL);
  TDB_CHECK_EQ(q->from->next->join, JOIN_INNER);
  TDB_CHECK(q->where != NULL && q->group != NULL && q->having != NULL);
  TDB_CHECK(q->order != NULL && q->order->desc[0] == 1);
  TDB_CHECK(q->has_limit);
  tdb_arena_free(a);
}

static void test_create_table(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s = parse1(a,
    "CREATE TABLE t ("
    " id INTEGER PRIMARY KEY,"
    " name VARCHAR(20) NOT NULL,"
    " total DECIMAL(10,2) GENERATED ALWAYS AS (qty * price) STORED"
    ") WITH SYSTEM VERSIONING");
  TDB_CHECK_EQ(s->kind, ST_CREATE_TABLE);
  tdb_create_table *ct = s->u.create_table;
  TDB_CHECK_EQ(ct->ncol, 3);
  TDB_CHECK_STR(ct->cols[0].name, "id");
  TDB_CHECK_EQ(ct->cols[0].type.id, TDB_T_INTEGER);
  TDB_CHECK_EQ(ct->cols[0].pk, 1);
  TDB_CHECK_EQ(ct->cols[1].type.id, TDB_T_VARCHAR);
  TDB_CHECK_EQ(ct->cols[1].type.length, 20);
  TDB_CHECK_EQ(ct->cols[1].notnull, 1);
  TDB_CHECK_EQ(ct->cols[2].type.id, TDB_T_DECIMAL);
  TDB_CHECK_EQ(ct->cols[2].type.precision, 10);
  TDB_CHECK_EQ(ct->cols[2].type.scale, 2);
  TDB_CHECK_EQ(ct->cols[2].generated, TDB_GEN_STORED);
  TDB_CHECK_STR(ct->cols[2].generated_sql, "qty * price");
  TDB_CHECK_EQ(ct->system_versioning, 1);
  tdb_arena_free(a);
}

static void test_dml(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s;

  s = parse1(a, "INSERT INTO t (a, b) VALUES (1, 2), (3, 4)");
  TDB_CHECK_EQ(s->kind, ST_INSERT);
  TDB_CHECK_EQ(s->u.insert.ncol, 2);
  TDB_CHECK_EQ(s->u.insert.nrows, 2);

  s = parse1(a, "INSERT INTO t SELECT * FROM u");
  TDB_CHECK_EQ(s->kind, ST_INSERT);
  TDB_CHECK(s->u.insert.select != NULL);

  s = parse1(a, "UPDATE t SET a = 1, b = 2 WHERE id = 3");
  TDB_CHECK_EQ(s->kind, ST_UPDATE);
  TDB_CHECK_EQ(s->u.update.nset, 2);
  TDB_CHECK(s->u.update.where != NULL);

  s = parse1(a, "DELETE FROM t WHERE x = 1");
  TDB_CHECK_EQ(s->kind, ST_DELETE);
  TDB_CHECK(s->u.del.where != NULL);
  tdb_arena_free(a);
}

static void test_index_view_routine(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s;

  s = parse1(a, "CREATE UNIQUE INDEX i ON t (a, b DESC)");
  TDB_CHECK_EQ(s->kind, ST_CREATE_INDEX);
  TDB_CHECK_EQ(s->u.create_index->unique, 1);
  TDB_CHECK_EQ(s->u.create_index->ncol, 2);
  TDB_CHECK_EQ(s->u.create_index->desc[1], 1);

  s = parse1(a, "CREATE VIEW v AS SELECT * FROM t WHERE x > 0");
  TDB_CHECK_EQ(s->kind, ST_CREATE_VIEW);
  TDB_CHECK(strstr(s->u.create_view.select_sql, "SELECT") != NULL);

  s = parse1(a, "CREATE FUNCTION dbl(x INT) RETURNS INT LANGUAGE LUA AS $$ return x*2 $$");
  TDB_CHECK_EQ(s->kind, ST_CREATE_ROUTINE);
  TDB_CHECK_EQ(s->u.create_routine.is_function, 1);
  TDB_CHECK_STR(s->u.create_routine.name, "dbl");
  TDB_CHECK(strstr(s->u.create_routine.lua_src, "return") != NULL);
  tdb_arena_free(a);
}

static void test_txn_drop(void) {
  tdb_arena *a = tdb_arena_new(4096);
  TDB_CHECK_EQ(parse1(a, "BEGIN")->kind, ST_BEGIN);
  TDB_CHECK_EQ(parse1(a, "COMMIT")->kind, ST_COMMIT);
  TDB_CHECK_EQ(parse1(a, "ROLLBACK")->kind, ST_ROLLBACK);
  TDB_CHECK_EQ(parse1(a, "SAVEPOINT s1")->kind, ST_SAVEPOINT);
  TDB_CHECK_EQ(parse1(a, "ROLLBACK TO s1")->kind, ST_ROLLBACK_TO);
  tdb_ast_stmt *d = parse1(a, "DROP TABLE IF EXISTS t");
  TDB_CHECK_EQ(d->kind, ST_DROP_TABLE);
  TDB_CHECK_EQ(d->u.drop.if_exists, 1);
  tdb_arena_free(a);
}

static void test_phase11_syntax(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s;

  /* TRUNCATE / ANALYZE / REINDEX / COMMENT / LOCK */
  s = parse1(a, "TRUNCATE TABLE customers RESTART IDENTITY CASCADE");
  TDB_CHECK_EQ(s->kind, ST_TRUNCATE);
  TDB_CHECK_STR(s->u.truncate.name, "customers");
  TDB_CHECK_EQ(s->u.truncate.restart, 1);
  TDB_CHECK_EQ(s->u.truncate.cascade, 1);

  s = parse1(a, "ANALYZE orders");
  TDB_CHECK_EQ(s->kind, ST_ANALYZE);
  TDB_CHECK_STR(s->u.analyze.name, "orders");

  s = parse1(a, "REINDEX INDEX i_users_email");
  TDB_CHECK_EQ(s->kind, ST_REINDEX);
  TDB_CHECK_EQ(s->u.reindex.is_index, 1);

  s = parse1(a, "COMMENT ON TABLE accounts IS 'audited daily'");
  TDB_CHECK_EQ(s->kind, ST_COMMENT);
  TDB_CHECK_EQ(s->u.comment_on.on_kind, 1);
  TDB_CHECK_STR(s->u.comment_on.body, "audited daily");

  s = parse1(a, "LOCK TABLE inventory IN EXCLUSIVE MODE");
  TDB_CHECK_EQ(s->kind, ST_LOCK_TABLE);
  TDB_CHECK_EQ(s->u.lock_tbl.exclusive, 1);

  /* TABLESPACE + PARTITION BY + COMPRESSION on CREATE TABLE */
  s = parse1(a,
    "CREATE TABLE m (id INTEGER PRIMARY KEY, region TEXT, ts TIMESTAMP) "
    "TABLESPACE ts_warm "
    "WITH COMPRESSION=zstd "
    "PARTITION BY RANGE (ts)");
  TDB_CHECK_EQ(s->kind, ST_CREATE_TABLE);
  tdb_create_table *ct = s->u.create_table;
  TDB_CHECK_STR(ct->tablespace, "ts_warm");
  TDB_CHECK_STR(ct->compression, "zstd");
  TDB_CHECK_EQ(ct->partition_kind, TDB_PART_RANGE);
  TDB_CHECK_EQ(ct->npart_col, 1);
  TDB_CHECK_STR(ct->partition_cols[0], "ts");

  s = parse1(a, "CREATE TABLESPACE warm LOCATION '/srv/db/warm'");
  TDB_CHECK_EQ(s->kind, ST_CREATE_TABLESPACE);
  TDB_CHECK_STR(s->u.create_tablespace.name, "warm");
  TDB_CHECK_STR(s->u.create_tablespace.location, "/srv/db/warm");

  s = parse1(a, "DROP TABLESPACE IF EXISTS warm");
  TDB_CHECK_EQ(s->kind, ST_DROP_TABLESPACE);
  TDB_CHECK_EQ(s->u.drop_tablespace.if_exists, 1);

  /* GRANT / REVOKE (parsed; semantics are stubs) */
  s = parse1(a, "GRANT SELECT, INSERT ON TABLE t TO alice");
  TDB_CHECK_EQ(s->kind, ST_GRANT);
  TDB_CHECK_STR(s->u.grant.grantee, "alice");

  s = parse1(a, "REVOKE INSERT ON TABLE t FROM bob");
  TDB_CHECK_EQ(s->kind, ST_REVOKE);
  TDB_CHECK_STR(s->u.grant.grantee, "bob");

  /* New types parsed via CREATE TABLE */
  s = parse1(a,
    "CREATE TABLE x ("
    " doc XML,"
    " range INTERVAL,"
    " flags BIT(8),"
    " mask  BIT VARYING(64)"
    ")");
  TDB_CHECK_EQ(s->kind, ST_CREATE_TABLE);
  ct = s->u.create_table;
  TDB_CHECK_EQ(ct->cols[0].type.id, TDB_T_XML);
  TDB_CHECK_EQ(ct->cols[1].type.id, TDB_T_INTERVAL);
  TDB_CHECK_EQ(ct->cols[2].type.id, TDB_T_BIT);
  TDB_CHECK_EQ(ct->cols[3].type.id, TDB_T_VARBIT);
  tdb_arena_free(a);
}

static void test_errors_and_tail(void) {
  tdb_arena *a = tdb_arena_new(4096);
  tdb_ast_stmt *s = NULL; char *err = NULL; const char *tail = NULL;

  TDB_CHECK_EQ(tdb_parse(a, "SELECT FROM", &s, &err, &tail), TDB_ERROR);
  TDB_CHECK(err != NULL);

  /* multi-statement: follow the tail */
  const char *script = "SELECT 1; SELECT 2;";
  int rc = tdb_parse(a, script, &s, &err, &tail);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK_EQ(s->kind, ST_SELECT);
  rc = tdb_parse(a, tail, &s, &err, &tail);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK_EQ(s->kind, ST_SELECT);
  tdb_arena_free(a);
}

static tdb_test_case cases[] = {
  {"expr_precedence", test_expr_precedence},
  {"select", test_select},
  {"create_table", test_create_table},
  {"dml", test_dml},
  {"index_view_routine", test_index_view_routine},
  {"txn_drop", test_txn_drop},
  {"phase11_syntax", test_phase11_syntax},
  {"errors_and_tail", test_errors_and_tail},
};
TDB_MAIN(cases)
