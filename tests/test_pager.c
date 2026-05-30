/* test_pager.c — page cache, alloc/free, commit, rollback, persistence. */
#include "tdb_test.h"
#include "../src/storage/tdb_pager.h"

#include <stdio.h>
#include <string.h>

static void test_alloc_commit_persist(void) {
  const char *path = "test_pager_1.db";
  remove(path); remove("test_pager_1.db-wal");

  tdb_pgno pgs[3];
  {
    tdb_pager *p = NULL;
    TDB_CHECK_EQ(tdb_pager_open(NULL, path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE, &p), TDB_OK);
    TDB_CHECK_EQ(tdb_pager_db_size(p), 1u); /* page 1 = header */
    tdb_pager_begin(p);
    for (int i = 0; i < 3; i++) {
      tdb_page *pg = NULL;
      TDB_CHECK_EQ(tdb_pager_alloc(p, &pg), TDB_OK);
      pgs[i] = pg->pgno;
      memset(pg->data, 0x40 + i, tdb_pager_page_size(p));
      tdb_pager_write(p, pg);
      tdb_pager_unref(p, pg);
    }
    TDB_CHECK_EQ(tdb_pager_commit(p), TDB_OK);
    tdb_pager_close(p);
  }
  {
    tdb_pager *p = NULL;
    TDB_CHECK_EQ(tdb_pager_open(NULL, path, TDB_OPEN_READWRITE, &p), TDB_OK);
    for (int i = 0; i < 3; i++) {
      tdb_page *pg = NULL;
      TDB_CHECK_EQ(tdb_pager_get(p, pgs[i], &pg), TDB_OK);
      TDB_CHECK_EQ(pg->data[0], 0x40 + i);
      TDB_CHECK_EQ(pg->data[100], 0x40 + i);
      tdb_pager_unref(p, pg);
    }
    tdb_pager_close(p);
  }
  remove(path); remove("test_pager_1.db-wal");
}

static void test_rollback(void) {
  const char *path = "test_pager_2.db";
  remove(path); remove("test_pager_2.db-wal");

  tdb_pager *p = NULL;
  tdb_pager_open(NULL, path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE, &p);
  uint32_t ps = tdb_pager_page_size(p);

  tdb_pager_begin(p);
  tdb_page *pg = NULL;
  tdb_pager_alloc(p, &pg);
  tdb_pgno target = pg->pgno;
  memset(pg->data, 0xAA, ps);
  tdb_pager_write(p, pg);
  tdb_pager_unref(p, pg);
  tdb_pager_commit(p);

  /* now modify and roll back */
  tdb_pager_begin(p);
  tdb_pager_get(p, target, &pg);
  memset(pg->data, 0xBB, ps);
  tdb_pager_write(p, pg);
  tdb_pager_unref(p, pg);
  TDB_CHECK_EQ(tdb_pager_rollback(p), TDB_OK);

  tdb_pager_get(p, target, &pg);
  TDB_CHECK_EQ(pg->data[0], 0xAA); /* rolled back to committed value */
  tdb_pager_unref(p, pg);

  tdb_pager_close(p);
  remove(path); remove("test_pager_2.db-wal");
}

static void test_freelist(void) {
  const char *path = "test_pager_3.db";
  remove(path); remove("test_pager_3.db-wal");

  tdb_pager *p = NULL;
  tdb_pager_open(NULL, path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE, &p);
  tdb_pager_begin(p);

  tdb_page *a = NULL, *b = NULL;
  tdb_pager_alloc(p, &a);
  tdb_pgno pa = a->pgno;
  tdb_pager_unref(p, a);
  tdb_pager_alloc(p, &b);
  tdb_pgno pb = b->pgno;
  tdb_pager_unref(p, b);
  TDB_CHECK(pb > pa);

  /* free pb, next alloc should reuse it */
  TDB_CHECK_EQ(tdb_pager_free(p, pb), TDB_OK);
  tdb_page *c = NULL;
  tdb_pager_alloc(p, &c);
  TDB_CHECK_EQ(c->pgno, pb);
  tdb_pager_unref(p, c);

  tdb_pager_commit(p);
  tdb_pager_close(p);
  remove(path); remove("test_pager_3.db-wal");
}

static void test_memory_db(void) {
  tdb_pager *p = NULL;
  TDB_CHECK_EQ(tdb_pager_open(NULL, ":memory:", TDB_OPEN_MEMORY, &p), TDB_OK);
  tdb_pager_begin(p);
  tdb_page *pg = NULL;
  tdb_pager_alloc(p, &pg);
  tdb_pgno t = pg->pgno;
  memset(pg->data, 0x5A, tdb_pager_page_size(p));
  tdb_pager_write(p, pg);
  tdb_pager_unref(p, pg);
  tdb_pager_commit(p);
  tdb_pager_get(p, t, &pg);
  TDB_CHECK_EQ(pg->data[0], 0x5A);
  tdb_pager_unref(p, pg);
  tdb_pager_close(p);
}

static tdb_test_case cases[] = {
  {"alloc_commit_persist", test_alloc_commit_persist},
  {"rollback", test_rollback},
  {"freelist", test_freelist},
  {"memory_db", test_memory_db},
};
TDB_MAIN(cases)
