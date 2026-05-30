/* test_btree.c — table & index b-trees: splits, ordering, overflow, deletes. */
#include "tdb_test.h"
#include "../src/storage/tdb_btree.h"
#include "../src/storage/tdb_pager.h"
#include "../src/value/tdb_record.h"

#include <stdlib.h>
#include <string.h>

static tdb_pager *open_mem(void) {
  tdb_pager *p = NULL;
  tdb_pager_open(NULL, ":memory:", TDB_OPEN_MEMORY, &p);
  tdb_pager_begin(p);
  return p;
}

/* Insert enough rows to force multiple splits, then verify ordered scan. */
static void test_table_many(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root;
  TDB_CHECK_EQ(tdb_btree_create(p, TDB_BT_TABLE, &root), TDB_OK);
  tdb_btree *bt;
  tdb_btree_open(p, root, TDB_BT_TABLE, NULL, &bt);

  const int N = 2000;
  for (int i = 0; i < N; i++) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "row-%d", i);
    TDB_CHECK_EQ(tdb_btree_put(bt, (tdb_rowid)(i + 1), buf, n), TDB_OK);
  }

  /* point lookups */
  tdb_cursor *cur; tdb_cursor_open(bt, &cur);
  for (int i = 0; i < N; i += 137) {
    const uint8_t *v; int n;
    TDB_CHECK_EQ(tdb_btree_get(bt, (tdb_rowid)(i + 1), &v, &n, cur), TDB_OK);
    char expect[32]; int en = snprintf(expect, sizeof(expect), "row-%d", i);
    TDB_CHECK_EQ(n, en);
    TDB_CHECK(memcmp(v, expect, (size_t)n) == 0);
  }
  tdb_cursor_close(cur);

  /* ordered full scan */
  tdb_cursor_open(bt, &cur);
  int count = 0; tdb_rowid prev = 0;
  for (tdb_cursor_first(cur); !tdb_cursor_eof(cur); tdb_cursor_next(cur)) {
    tdb_rowid rid; tdb_cursor_rowid(cur, &rid);
    TDB_CHECK(rid > prev);
    prev = rid;
    count++;
  }
  TDB_CHECK_EQ(count, N);
  tdb_cursor_close(cur);

  tdb_rowid mx; tdb_btree_max_rowid(bt, &mx);
  TDB_CHECK_EQ((int)mx, N);

  tdb_btree_close(bt);
  tdb_pager_close(p);
}

/* Randomized insert/delete vs a reference model. */
static void test_table_fuzz(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root; tdb_btree_create(p, TDB_BT_TABLE, &root);
  tdb_btree *bt; tdb_btree_open(p, root, TDB_BT_TABLE, NULL, &bt);

  enum { M = 500 };
  int present[M + 1];
  memset(present, 0, sizeof(present));
  srand(12345);

  for (int iter = 0; iter < 4000; iter++) {
    int k = 1 + rand() % M;
    if (rand() & 1) {
      char b[16]; int n = snprintf(b, sizeof(b), "v%d", k);
      tdb_btree_put(bt, (tdb_rowid)k, b, n);
      present[k] = 1;
    } else {
      int found = 0;
      tdb_btree_del(bt, (tdb_rowid)k, &found);
      TDB_CHECK_EQ(found, present[k]);
      present[k] = 0;
    }
  }
  /* verify final state matches the model */
  tdb_cursor *cur; tdb_cursor_open(bt, &cur);
  for (int k = 1; k <= M; k++) {
    const uint8_t *v; int n;
    int rc = tdb_btree_get(bt, (tdb_rowid)k, &v, &n, cur);
    if (present[k]) TDB_CHECK_EQ(rc, TDB_OK);
    else TDB_CHECK_EQ(rc, TDB_NOTFOUND);
  }
  tdb_cursor_close(cur);
  tdb_btree_close(bt);
  tdb_pager_close(p);
}

/* Values larger than a page must round-trip through overflow chains. */
static void test_overflow(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root; tdb_btree_create(p, TDB_BT_TABLE, &root);
  tdb_btree *bt; tdb_btree_open(p, root, TDB_BT_TABLE, NULL, &bt);

  int big = 20000;
  uint8_t *blob = (uint8_t *)malloc((size_t)big);
  for (int i = 0; i < big; i++) blob[i] = (uint8_t)(i * 31 + 7);
  TDB_CHECK_EQ(tdb_btree_put(bt, 42, blob, big), TDB_OK);

  tdb_cursor *cur; tdb_cursor_open(bt, &cur);
  const uint8_t *v; int n;
  TDB_CHECK_EQ(tdb_btree_get(bt, 42, &v, &n, cur), TDB_OK);
  TDB_CHECK_EQ(n, big);
  TDB_CHECK(memcmp(v, blob, (size_t)big) == 0);
  tdb_cursor_close(cur);

  /* replacing frees the old overflow chain */
  TDB_CHECK_EQ(tdb_btree_put(bt, 42, "small", 5), TDB_OK);
  tdb_cursor_open(bt, &cur);
  TDB_CHECK_EQ(tdb_btree_get(bt, 42, &v, &n, cur), TDB_OK);
  TDB_CHECK_EQ(n, 5);
  tdb_cursor_close(cur);

  free(blob);
  tdb_btree_close(bt);
  tdb_pager_close(p);
}

/* Index b-tree keyed by encoded records, ordered by record comparison. */
static void test_index(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root; tdb_btree_create(p, TDB_BT_INDEX, &root);
  /* compare keys as 2-column records (text, int) by logical value */
  tdb_keyinfo ki; memset(&ki, 0, sizeof(ki));
  ki.ncol = 2;
  tdb_btree *bt; tdb_btree_open(p, root, TDB_BT_INDEX, &ki, &bt);

  /* keys = (name, rowid) records, inserted out of order */
  const char *names[] = {"charlie", "alpha", "bravo", "delta", "alpha"};
  int rids[] = {3, 1, 2, 4, 5};
  for (int i = 0; i < 5; i++) {
    tdb_value vv[2];
    tdb_value_init(&vv[0]); tdb_value_init(&vv[1]);
    tdb_value_set_text(&vv[0], names[i], -1, 1);
    tdb_value_set_int(&vv[1], rids[i]);
    tdb_buf b; tdb_buf_init(&b);
    tdb_record_encode(vv, 2, &b);
    TDB_CHECK_EQ(tdb_index_put(bt, b.data, (int)b.len), TDB_OK);
    tdb_buf_free(&b);
    tdb_value_clear(&vv[0]); tdb_value_clear(&vv[1]);
  }

  /* scan should be sorted: alpha/1, alpha/5, bravo/2, charlie/3, delta/4 */
  const char *order_names[] = {"alpha", "alpha", "bravo", "charlie", "delta"};
  int order_rids[] = {1, 5, 2, 3, 4};
  tdb_cursor *cur; tdb_cursor_open(bt, &cur);
  int i = 0;
  for (tdb_cursor_first(cur); !tdb_cursor_eof(cur); tdb_cursor_next(cur)) {
    const uint8_t *k; int kl;
    tdb_cursor_key(cur, &k, &kl);
    tdb_value dec[2]; int nc;
    tdb_record_decode(k, (size_t)kl, dec, 2, &nc);
    TDB_CHECK_EQ(nc, 2);
    /* decoded text is borrowed (not NUL-terminated): compare by length */
    int en = (int)strlen(order_names[i]);
    TDB_CHECK_EQ(dec[0].u.s.n, en);
    TDB_CHECK(memcmp(dec[0].u.s.p, order_names[i], (size_t)en) == 0);
    TDB_CHECK_EQ(tdb_value_as_int(&dec[1]), order_rids[i]);
    tdb_value_clear(&dec[0]); tdb_value_clear(&dec[1]);
    i++;
  }
  TDB_CHECK_EQ(i, 5);
  tdb_cursor_close(cur);

  tdb_btree_close(bt);
  tdb_pager_close(p);
}

static tdb_test_case cases[] = {
  {"table_many", test_table_many},
  {"table_fuzz", test_table_fuzz},
  {"overflow", test_overflow},
  {"index", test_index},
};
TDB_MAIN(cases)
