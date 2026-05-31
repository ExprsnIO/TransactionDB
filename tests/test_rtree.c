/* test_rtree.c — persistent R-tree: insert/split, window search, delete. */
#include "tdb_test.h"
#include "../src/storage/tdb_rtree.h"
#include "../src/storage/tdb_pager.h"

#include <stdlib.h>
#include <string.h>

static tdb_pager *open_mem(void) {
  tdb_pager *p = NULL;
  tdb_pager_open(NULL, ":memory:", TDB_OPEN_MEMORY, &p);
  tdb_pager_begin(p);
  return p;
}

static tdb_bbox pt(double x, double y) { tdb_bbox b = { x, y, x, y }; return b; }
static tdb_bbox box(double a, double b, double c, double d) { tdb_bbox r = { a, b, c, d }; return r; }

/* search collector */
typedef struct { tdb_rowid ids[8192]; int n; } collector;
static int collect(void *ctx, tdb_rowid id) {
  collector *c = (collector *)ctx;
  if (c->n < 8192) c->ids[c->n++] = id;
  return 0;
}
static int has_id(const collector *c, tdb_rowid id) {
  for (int i = 0; i < c->n; i++) if (c->ids[i] == id) return 1;
  return 0;
}

/* A grid of points; a window query must return exactly the points inside it. */
static void test_grid(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root;
  TDB_CHECK_EQ(tdb_rtree_create(p, &root), TDB_OK);

  /* 50x50 grid -> 2500 points, forcing many node splits */
  tdb_rowid id = 1;
  for (int x = 0; x < 50; x++)
    for (int y = 0; y < 50; y++) {
      tdb_bbox b = pt(x, y);
      TDB_CHECK_EQ(tdb_rtree_insert(p, root, &b, id++), TDB_OK);
    }

  /* window [10,10]..[19,19] -> the 10x10 = 100 points in that block */
  collector c; c.n = 0;
  tdb_bbox q = box(10, 10, 19, 19);
  TDB_CHECK_EQ(tdb_rtree_search(p, root, &q, collect, &c), 0);
  TDB_CHECK_EQ(c.n, 100);
  /* spot-check membership: (10,10), (15,15), (19,19) in; (9,9),(20,20) out */
  collector all; all.n = 0;
  tdb_bbox full = box(-1, -1, 1000, 1000);
  tdb_rtree_search(p, root, &full, collect, &all);
  TDB_CHECK_EQ(all.n, 2500);

  /* a query box matching a single point */
  collector one; one.n = 0;
  tdb_bbox q1 = box(25, 25, 25, 25);
  tdb_rtree_search(p, root, &q1, collect, &one);
  TDB_CHECK_EQ(one.n, 1);

  /* empty region */
  collector none; none.n = 0;
  tdb_bbox qe = box(100, 100, 200, 200);
  tdb_rtree_search(p, root, &qe, collect, &none);
  TDB_CHECK_EQ(none.n, 0);

  tdb_rtree_destroy(p, root);
  tdb_pager_close(p);
}

/* Overlapping rectangles; verify a query hits exactly the overlapping ones. */
static void test_rects_and_delete(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root;
  tdb_rtree_create(p, &root);

  /* rowid r covers [r, r, r+2, r+2] for r in 1..200 (overlapping run) */
  for (tdb_rowid r = 1; r <= 200; r++) {
    tdb_bbox b = box((double)r, (double)r, (double)r + 2, (double)r + 2);
    TDB_CHECK_EQ(tdb_rtree_insert(p, root, &b, r), TDB_OK);
  }

  /* point (50,50) sits inside rects whose [r,r+2] spans 50: r in 48..50 */
  collector c; c.n = 0;
  tdb_bbox q = box(50, 50, 50, 50);
  tdb_rtree_search(p, root, &q, collect, &c);
  TDB_CHECK(has_id(&c, 48) && has_id(&c, 49) && has_id(&c, 50));
  TDB_CHECK(!has_id(&c, 47) && !has_id(&c, 51));

  /* delete rowid 49, re-query: gone, neighbors remain */
  int found = 0;
  tdb_bbox b49 = box(49, 49, 51, 51);
  TDB_CHECK_EQ(tdb_rtree_delete(p, root, &b49, 49, &found), TDB_OK);
  TDB_CHECK(found);
  collector c2; c2.n = 0;
  tdb_rtree_search(p, root, &q, collect, &c2);
  TDB_CHECK(!has_id(&c2, 49));
  TDB_CHECK(has_id(&c2, 48) && has_id(&c2, 50));

  /* deleting a non-existent rowid reports not found */
  found = 1;
  tdb_bbox b999 = box(999, 999, 1000, 1000);
  tdb_rtree_delete(p, root, &b999, 999, &found);
  TDB_CHECK(!found);

  tdb_rtree_destroy(p, root);
  tdb_pager_close(p);
}

/* delete everything, ensure searches come back empty and tree stays valid */
static void test_delete_all(void) {
  tdb_pager *p = open_mem();
  tdb_pgno root;
  tdb_rtree_create(p, &root);
  const int N = 500;
  for (int i = 1; i <= N; i++) { tdb_bbox b = pt(i % 25, i / 25); tdb_rtree_insert(p, root, &b, (tdb_rowid)i); }
  for (int i = 1; i <= N; i++) {
    int found = 0; tdb_bbox b = pt(i % 25, i / 25);
    tdb_rtree_delete(p, root, &b, (tdb_rowid)i, &found);
    TDB_CHECK(found);
  }
  collector c; c.n = 0;
  tdb_bbox full = box(-1, -1, 1000, 1000);
  tdb_rtree_search(p, root, &full, collect, &c);
  TDB_CHECK_EQ(c.n, 0);
  tdb_rtree_destroy(p, root);
  tdb_pager_close(p);
}

static tdb_test_case cases[] = {
  {"grid", test_grid},
  {"rects_and_delete", test_rects_and_delete},
  {"delete_all", test_delete_all},
};
TDB_MAIN(cases)
