/* test_lock.c — lock compatibility and wait-die conflict resolution. */
#include "tdb_test.h"
#include "../src/txn/tdb_lock.h"

static void test_shared_compatible(void) {
  tdb_lockmgr *m = tdb_lockmgr_new();
  TDB_CHECK_EQ(tdb_lock_acquire(m, 5, TDB_RES_ROW, 100, 1, TDB_LOCK_SHARED), TDB_OK);
  TDB_CHECK_EQ(tdb_lock_acquire(m, 7, TDB_RES_ROW, 100, 1, TDB_LOCK_SHARED), TDB_OK);
  TDB_CHECK_EQ(tdb_lock_holder_count(m, TDB_RES_ROW, 100, 1), 2);
  tdb_lockmgr_free(m);
}

static void test_wait_die(void) {
  tdb_lockmgr *m = tdb_lockmgr_new();
  /* holders 5 and 7 hold shared */
  tdb_lock_acquire(m, 5, TDB_RES_ROW, 100, 1, TDB_LOCK_SHARED);
  tdb_lock_acquire(m, 7, TDB_RES_ROW, 100, 1, TDB_LOCK_SHARED);

  /* younger requester (9 > holders) dies */
  TDB_CHECK_EQ(tdb_lock_acquire(m, 9, TDB_RES_ROW, 100, 1, TDB_LOCK_EXCL), TDB_ABORT);
  /* older requester (3 < holders) waits */
  TDB_CHECK_EQ(tdb_lock_acquire(m, 3, TDB_RES_ROW, 100, 1, TDB_LOCK_EXCL), TDB_BUSY);

  /* release holders; now exclusive succeeds */
  tdb_lock_release_all(m, 5);
  tdb_lock_release_all(m, 7);
  TDB_CHECK_EQ(tdb_lock_acquire(m, 9, TDB_RES_ROW, 100, 1, TDB_LOCK_EXCL), TDB_OK);
  TDB_CHECK_EQ(tdb_lock_holder_count(m, TDB_RES_ROW, 100, 1), 1);
  tdb_lockmgr_free(m);
}

static void test_upgrade(void) {
  tdb_lockmgr *m = tdb_lockmgr_new();
  TDB_CHECK_EQ(tdb_lock_acquire(m, 5, TDB_RES_TABLE, 1, 0, TDB_LOCK_SHARED), TDB_OK);
  /* sole holder upgrades to exclusive */
  TDB_CHECK_EQ(tdb_lock_acquire(m, 5, TDB_RES_TABLE, 1, 0, TDB_LOCK_EXCL), TDB_OK);
  /* a different txn now conflicts */
  TDB_CHECK_EQ(tdb_lock_acquire(m, 9, TDB_RES_TABLE, 1, 0, TDB_LOCK_SHARED), TDB_ABORT);
  tdb_lockmgr_free(m);
}

static void test_distinct_resources(void) {
  tdb_lockmgr *m = tdb_lockmgr_new();
  TDB_CHECK_EQ(tdb_lock_acquire(m, 5, TDB_RES_ROW, 100, 1, TDB_LOCK_EXCL), TDB_OK);
  /* different row id -> no conflict */
  TDB_CHECK_EQ(tdb_lock_acquire(m, 9, TDB_RES_ROW, 100, 2, TDB_LOCK_EXCL), TDB_OK);
  tdb_lockmgr_free(m);
}

static tdb_test_case cases[] = {
  {"shared_compatible", test_shared_compatible},
  {"wait_die", test_wait_die},
  {"upgrade", test_upgrade},
  {"distinct_resources", test_distinct_resources},
};
TDB_MAIN(cases)
