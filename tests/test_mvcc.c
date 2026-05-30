/* test_mvcc.c — snapshot visibility truth table. */
#include "tdb_test.h"
#include "../src/txn/tdb_mvcc.h"

/* fake commit-status oracle: xids in `committed[]` are committed, else aborted */
static tdb_txnid g_committed[16];
static int g_ncommitted;
static tdb_xstate fake_state(void *ctx, tdb_txnid x) {
  (void)ctx;
  for (int i = 0; i < g_ncommitted; i++) if (g_committed[i] == x) return TDB_XACT_COMMITTED;
  return TDB_XACT_ABORTED;
}
static void test_basic_visibility(void) {
  g_ncommitted = 0;
  g_committed[g_ncommitted++] = 10; /* txn 10 committed */
  g_committed[g_ncommitted++] = 20;

  tdb_snapshot s;
  s.self = 50;
  s.xmax = 30;          /* snapshot taken when next id was 30 */
  s.active = NULL;
  s.nactive = 0;

  /* row created by committed 10, live -> visible */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 10, 0) == 1);
  /* created by committed 20, deleted by committed 20 within snapshot? xmax=20<30 committed -> gone */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 10, 20) == 0);
  /* created by 10, deleted by 25 which is NOT committed -> still visible */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 10, 25) == 1);
  /* created by 25 (not committed) -> not visible */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 25, 0) == 0);
}

static void test_snapshot_boundary(void) {
  g_ncommitted = 0;
  g_committed[g_ncommitted++] = 10;
  g_committed[g_ncommitted++] = 40; /* committed but AFTER snapshot horizon */

  tdb_snapshot s; s.self = 99; s.xmax = 30; s.active = NULL; s.nactive = 0;
  /* created by 40 >= xmax(30): not visible even though committed */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 40, 0) == 0);
  /* created by 10, deleted by 40 (>= xmax): deleter not visible -> row visible */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 10, 40) == 1);
}

static void test_active_set(void) {
  g_ncommitted = 0;
  g_committed[g_ncommitted++] = 15;   /* committed */
  tdb_txnid active[] = {15};
  tdb_snapshot s; s.self = 99; s.xmax = 30; s.active = active; s.nactive = 1;
  /* 15 committed but was in-flight at snapshot time -> not visible */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 15, 0) == 0);
}

static void test_own_writes(void) {
  g_ncommitted = 0;
  tdb_snapshot s; s.self = 7; s.xmax = 5; s.active = NULL; s.nactive = 0;
  /* own (uncommitted) insert visible to self */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 7, 0) == 1);
  /* own delete -> not visible to self */
  TDB_CHECK(tdb_mvcc_visible(fake_state, NULL, &s, 7, 7) == 0);
}

static tdb_test_case cases[] = {
  {"basic_visibility", test_basic_visibility},
  {"snapshot_boundary", test_snapshot_boundary},
  {"active_set", test_active_set},
  {"own_writes", test_own_writes},
};
TDB_MAIN(cases)
