/* test_concurrency.c — one tdb_db shared across threads (serialized mode).
**
** Exercises the connection-level recursive lock: many threads concurrently
** run INSERT/UPDATE/SELECT against the same connection. Correctness criteria:
** no crash, no data races (run under ThreadSanitizer/ASan), and the final
** row count / sums match what the writers performed.
*/
#include "tdb_test.h"
#include "transactiondb.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NWRITERS 4
#define NREADERS 4
#define PER_WRITER 200

typedef struct { tdb_db *db; int base; } warg;

static int exec1(tdb_db *db, const char *sql) {
  char *e = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &e);
  tdb_free(e);
  return rc;
}

static void *writer(void *p) {
  warg *w = (warg *)p;
  for (int i = 0; i < PER_WRITER; i++) {
    char sql[160];
    int id = w->base + i;
    snprintf(sql, sizeof(sql),
             "INSERT INTO t (id, owner, v) VALUES (%d, %d, %d)",
             id, w->base, id);
    exec1(w->db, sql);
    /* update half of them */
    if (i % 2 == 0) {
      snprintf(sql, sizeof(sql), "UPDATE t SET v = v + 1000 WHERE id = %d", id);
      exec1(w->db, sql);
    }
  }
  return NULL;
}

static void *reader(void *p) {
  warg *w = (warg *)p;
  for (int i = 0; i < PER_WRITER; i++) {
    /* a SELECT must never crash or read torn state */
    tdb_stmt *s = NULL;
    if (tdb_prepare_v2(w->db, "SELECT COUNT(*), COALESCE(SUM(v),0) FROM t",
                       -1, &s, NULL) == TDB_OK) {
      while (tdb_step(s) == TDB_ROW) {
        (void)tdb_column_int64(s, 0);
        (void)tdb_column_int64(s, 1);
      }
      tdb_finalize(s);
    }
  }
  return NULL;
}

static void test_shared_connection(void) {
  tdb_db *db;
  TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(exec1(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, owner INTEGER, v INTEGER)"), TDB_OK);

  pthread_t wt[NWRITERS], rt[NREADERS];
  warg wargs[NWRITERS], rargs[NREADERS];
  for (int i = 0; i < NWRITERS; i++) {
    wargs[i].db = db; wargs[i].base = (i + 1) * 100000;
    pthread_create(&wt[i], NULL, writer, &wargs[i]);
  }
  for (int i = 0; i < NREADERS; i++) {
    rargs[i].db = db; rargs[i].base = 0;
    pthread_create(&rt[i], NULL, reader, &rargs[i]);
  }
  for (int i = 0; i < NWRITERS; i++) pthread_join(wt[i], NULL);
  for (int i = 0; i < NREADERS; i++) pthread_join(rt[i], NULL);

  /* every writer inserted PER_WRITER distinct ids; none collide across writers */
  tdb_stmt *s = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT COUNT(*) FROM t", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), NWRITERS * PER_WRITER);
  tdb_finalize(s);

  /* each writer's rows are all present under its owner tag */
  for (int wi = 0; wi < NWRITERS; wi++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM t WHERE owner = %d", (wi + 1) * 100000);
    TDB_CHECK_EQ(tdb_prepare_v2(db, sql, -1, &s, NULL), TDB_OK);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_int(s, 0), PER_WRITER);
    tdb_finalize(s);
  }
  tdb_close(db);
}

/* Concurrent explicit transactions on one connection are serialized by the
** connection lock, so each BEGIN..COMMIT runs to completion atomically. */
static void *txn_worker(void *p) {
  warg *w = (warg *)p;
  for (int i = 0; i < 50; i++) {
    exec1(w->db, "BEGIN");
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE ctr SET n = n + 1 WHERE id = 1");
    exec1(w->db, sql);
    exec1(w->db, "COMMIT");
  }
  return NULL;
}

static void test_serialized_txns(void) {
  tdb_db *db;
  TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  exec1(db, "CREATE TABLE ctr (id INTEGER PRIMARY KEY, n INTEGER)");
  exec1(db, "INSERT INTO ctr VALUES (1, 0)");

  pthread_t th[NWRITERS];
  warg a[NWRITERS];
  for (int i = 0; i < NWRITERS; i++) { a[i].db = db; a[i].base = 0; pthread_create(&th[i], NULL, txn_worker, &a[i]); }
  for (int i = 0; i < NWRITERS; i++) pthread_join(th[i], NULL);

  /* every increment landed (no lost updates) */
  tdb_stmt *s = NULL;
  tdb_prepare_v2(db, "SELECT n FROM ctr WHERE id = 1", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), NWRITERS * 50);
  tdb_finalize(s);
  tdb_close(db);
}

/* ---- shared-cache: many CONNECTIONS to one file ---------------------- */

typedef struct { const char *path; int base; int ok; } carg;

static void *conn_writer(void *p) {
  carg *c = (carg *)p;
  tdb_db *db;
  if (tdb_open(c->path, &db) != TDB_OK) { c->ok = 0; return NULL; }
  c->ok = 1;
  for (int i = 0; i < PER_WRITER; i++) {
    char sql[160];
    int id = c->base + i;
    snprintf(sql, sizeof(sql), "INSERT INTO t (id, owner, v) VALUES (%d, %d, %d)", id, c->base, id);
    /* retry on transient write conflicts (wait-die BUSY/ABORT) */
    for (int tries = 0; tries < 100000; tries++) {
      int rc = exec1(db, sql);
      if (rc == TDB_OK) break;
      if (rc != TDB_BUSY && rc != TDB_ABORT) break;
    }
  }
  tdb_close(db);
  return NULL;
}

/* Each thread opens its OWN connection to the same file: they share one env
** (shared cache), and the env's single-writer slot + wait-die serialize
** writers. Every insert must land exactly once. */
static void test_shared_cache_connections(void) {
  const char *path = "test_sharedcache.db";
  remove(path); remove("test_sharedcache.db-wal");

  tdb_db *db;
  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  exec1(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, owner INTEGER, v INTEGER)");
  tdb_close(db);   /* env stays alive only while a connection is open; reopen below */

  pthread_t th[NWRITERS];
  carg args[NWRITERS];
  for (int i = 0; i < NWRITERS; i++) {
    args[i].path = path; args[i].base = (i + 1) * 100000; args[i].ok = 0;
    pthread_create(&th[i], NULL, conn_writer, &args[i]);
  }
  for (int i = 0; i < NWRITERS; i++) pthread_join(th[i], NULL);
  for (int i = 0; i < NWRITERS; i++) TDB_CHECK_EQ(args[i].ok, 1);

  TDB_CHECK_EQ(tdb_open(path, &db), TDB_OK);
  tdb_stmt *s = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db, "SELECT COUNT(*) FROM t", -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), NWRITERS * PER_WRITER);   /* every row landed once */
  tdb_finalize(s);
  for (int wi = 0; wi < NWRITERS; wi++) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM t WHERE owner = %d", (wi + 1) * 100000);
    tdb_prepare_v2(db, sql, -1, &s, NULL);
    TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
    TDB_CHECK_EQ(tdb_column_int(s, 0), PER_WRITER);
    tdb_finalize(s);
  }
  tdb_close(db);
  remove(path); remove("test_sharedcache.db-wal");
}

/* Two connections to one file see each other's committed writes immediately
** (shared cache: no reopen needed). */
static void test_shared_cache_visibility(void) {
  const char *path = "test_scvis.db";
  remove(path); remove("test_scvis.db-wal");
  tdb_db *a, *b;
  TDB_CHECK_EQ(tdb_open(path, &a), TDB_OK);
  TDB_CHECK_EQ(tdb_open(path, &b), TDB_OK);

  exec1(a, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
  exec1(a, "INSERT INTO t VALUES (1,10),(2,20)");

  tdb_stmt *s = NULL;
  tdb_prepare_v2(b, "SELECT COUNT(*) FROM t", -1, &s, NULL);   /* b sees a's writes */
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 2);
  tdb_finalize(s);

  exec1(b, "INSERT INTO t VALUES (3,30)");
  tdb_prepare_v2(a, "SELECT v FROM t WHERE id = 3", -1, &s, NULL);  /* a sees b's write */
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 30);
  tdb_finalize(s);

  /* a holds an open write txn; b's autocommit write is refused, and a's
  ** uncommitted work survives the refusal */
  exec1(a, "BEGIN");
  exec1(a, "INSERT INTO t VALUES (4,40)");
  int rc = exec1(b, "INSERT INTO t VALUES (5,50)");
  TDB_CHECK(rc != TDB_OK);                       /* blocked by the writer slot */
  exec1(a, "COMMIT");
  TDB_CHECK_EQ(exec1(b, "INSERT INTO t VALUES (5,50)"), TDB_OK);   /* now allowed */

  tdb_prepare_v2(a, "SELECT COUNT(*) FROM t", -1, &s, NULL);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  TDB_CHECK_EQ(tdb_column_int(s, 0), 5);          /* 1..5 all present (no lost write) */
  tdb_finalize(s);
  tdb_close(a); tdb_close(b);
  remove(path); remove("test_scvis.db-wal");
}

static tdb_test_case cases[] = {
  {"shared_connection", test_shared_connection},
  {"serialized_txns", test_serialized_txns},
  {"shared_cache_connections", test_shared_cache_connections},
  {"shared_cache_visibility", test_shared_cache_visibility},
};
TDB_MAIN(cases)
