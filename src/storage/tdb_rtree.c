/*
** tdb_rtree.c — Guttman R-tree (quadratic split) over the pager.
**
** Node page layout (big-endian, like the rest of the file format):
**   off 0  u8   page type (TDB_PAGE_RTREE_LEAF / _INTERIOR)
**   off 1  u8   reserved
**   off 2  u16  entry count
**   off 4  u32  reserved
**   off 8  entries[]  -- fixed 40-byte records:
**            minx,miny,maxx,maxy  (4 x f64, IEEE-754 bit pattern as u64)
**            payload              (u64: rowid in a leaf, child pgno interior)
**
** Inserts descend by least bbox enlargement; an overflowing node is split with
** Guttman's quadratic PickSeeds/PickNext. A non-root split returns a new
** sibling to the parent; a root split keeps the root page number stable by
** moving the old root's entries into a fresh child and rewriting the root as a
** two-entry interior node.
*/
#include "tdb_rtree.h"
#include "tdb_format.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>

#define RT_HDR    8
#define RT_ENT    40           /* 4 doubles + 1 u64 */

typedef struct { tdb_bbox bb; uint64_t pl; } rtent;

/* one split result handed back up to the parent */
typedef struct { int valid; tdb_pgno pgno; tdb_bbox bb; } rt_split;

/* --------------------------- byte helpers ----------------------------- */

static double rd_dbl(const uint8_t *p) {
  uint64_t u = tdb_get_u64(p); double d; memcpy(&d, &u, 8); return d;
}
static void wr_dbl(uint8_t *p, double d) {
  uint64_t u; memcpy(&u, &d, 8); tdb_put_u64(p, u);
}

static int    nd_type(const tdb_page *pg)      { return pg->data[0]; }
static int    nd_count(const tdb_page *pg)     { return tdb_get_u16(pg->data + 2); }
static void   nd_set_count(tdb_page *pg, int n){ tdb_put_u16(pg->data + 2, (uint16_t)n); }
static int    nd_is_leaf(const tdb_page *pg)   { return nd_type(pg) == TDB_PAGE_RTREE_LEAF; }

static void nd_init(tdb_page *pg, int type) {
  memset(pg->data, 0, RT_HDR);
  pg->data[0] = (uint8_t)type;
  nd_set_count(pg, 0);
}

static void nd_get(const tdb_page *pg, int i, rtent *e) {
  const uint8_t *p = pg->data + RT_HDR + (size_t)i * RT_ENT;
  e->bb.minx = rd_dbl(p);      e->bb.miny = rd_dbl(p + 8);
  e->bb.maxx = rd_dbl(p + 16); e->bb.maxy = rd_dbl(p + 24);
  e->pl = tdb_get_u64(p + 32);
}
static void nd_put(tdb_page *pg, int i, const rtent *e) {
  uint8_t *p = pg->data + RT_HDR + (size_t)i * RT_ENT;
  wr_dbl(p, e->bb.minx);      wr_dbl(p + 8, e->bb.miny);
  wr_dbl(p + 16, e->bb.maxx); wr_dbl(p + 24, e->bb.maxy);
  tdb_put_u64(p + 32, e->pl);
}

/* max entries per node for this page size */
static int nd_cap(tdb_pager *p) {
  return (int)((tdb_pager_page_size(p) - RT_HDR) / RT_ENT);
}

/* --------------------------- bbox geometry ---------------------------- */

static double bb_area(const tdb_bbox *b) {
  double w = b->maxx - b->minx, h = b->maxy - b->miny;
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  return w * h;
}
static void bb_union(const tdb_bbox *a, const tdb_bbox *b, tdb_bbox *o) {
  o->minx = a->minx < b->minx ? a->minx : b->minx;
  o->miny = a->miny < b->miny ? a->miny : b->miny;
  o->maxx = a->maxx > b->maxx ? a->maxx : b->maxx;
  o->maxy = a->maxy > b->maxy ? a->maxy : b->maxy;
}
/* growth in area if `a` is enlarged to also cover `b` */
static double bb_enlarge(const tdb_bbox *a, const tdb_bbox *b) {
  tdb_bbox u; bb_union(a, b, &u);
  return bb_area(&u) - bb_area(a);
}

/* covering box of all entries currently in a node */
static void nd_cover(const tdb_page *pg, tdb_bbox *out) {
  int n = nd_count(pg);
  if (n == 0) { out->minx = out->miny = 0; out->maxx = out->maxy = 0; return; }
  rtent e; nd_get(pg, 0, &e); *out = e.bb;
  for (int i = 1; i < n; i++) { nd_get(pg, i, &e); bb_union(out, &e.bb, out); }
}

/* covering box of one child page (by page number) */
static int child_cover(tdb_pager *p, tdb_pgno pgno, tdb_bbox *out) {
  tdb_page *pg; int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  nd_cover(pg, out);
  tdb_pager_unref(p, pg);
  return TDB_OK;
}

/* ------------------------------ split --------------------------------- */

/* Guttman quadratic split of `n` entries into groups A and B (index lists).
** Both groups respect a minimum fill of ~40%. */
static void quad_split(tdb_pager *p, const rtent *ents, int n,
                       int *ga, int *na, int *gb, int *nb) {
  int min_fill = (nd_cap(p) * 2) / 5;
  if (min_fill < 1) min_fill = 1;
  char assigned[1 + 65536 / RT_ENT];
  memset(assigned, 0, sizeof(assigned));

  /* PickSeeds: the pair that wastes the most area if grouped together */
  int s1 = 0, s2 = 1; double worst = -1e308;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) {
      tdb_bbox u; bb_union(&ents[i].bb, &ents[j].bb, &u);
      double d = bb_area(&u) - bb_area(&ents[i].bb) - bb_area(&ents[j].bb);
      if (d > worst) { worst = d; s1 = i; s2 = j; }
    }

  *na = *nb = 0;
  ga[(*na)++] = s1; assigned[s1] = 1;
  gb[(*nb)++] = s2; assigned[s2] = 1;
  tdb_bbox ba = ents[s1].bb, bb = ents[s2].bb;
  int remaining = n - 2;

  while (remaining > 0) {
    /* if one group must take all the rest to meet min fill, do so */
    if (*na + remaining == min_fill) {
      for (int i = 0; i < n; i++) if (!assigned[i]) { ga[(*na)++] = i; assigned[i] = 1; }
      break;
    }
    if (*nb + remaining == min_fill) {
      for (int i = 0; i < n; i++) if (!assigned[i]) { gb[(*nb)++] = i; assigned[i] = 1; }
      break;
    }
    /* PickNext: the entry with the strongest preference for one group */
    int pick = -1, to_a = 1; double best = -1e308;
    for (int i = 0; i < n; i++) {
      if (assigned[i]) continue;
      double da = bb_enlarge(&ba, &ents[i].bb);
      double db = bb_enlarge(&bb, &ents[i].bb);
      double diff = da > db ? da - db : db - da;
      if (diff > best) { best = diff; pick = i; to_a = (da < db); }
    }
    if (pick < 0) break;   /* unreachable (remaining > 0), but keeps bounds provable */
    assigned[pick] = 1; remaining--;
    if (to_a) { ga[(*na)++] = pick; bb_union(&ba, &ents[pick].bb, &ba); }
    else      { gb[(*nb)++] = pick; bb_union(&bb, &ents[pick].bb, &bb); }
  }
}

/* Add `add` to node `pg`. If it overflows, split: keep group A in `pg`, put
** group B in a freshly allocated sibling, and report it via `out`. */
static int nd_add_or_split(tdb_pager *p, tdb_page *pg, const rtent *add, rt_split *out) {
  int n = nd_count(pg);
  int cap = nd_cap(p);
  if (n < cap) {
    nd_put(pg, n, add);
    nd_set_count(pg, n + 1);
    tdb_pager_write(p, pg);
    out->valid = 0;
    return TDB_OK;
  }
  /* overflow: gather all n+1 entries and split */
  int total = n + 1;
  rtent *ents = (rtent *)tdb_malloc(sizeof(rtent) * (size_t)total);
  if (!ents) return TDB_NOMEM;
  for (int i = 0; i < n; i++) nd_get(pg, i, &ents[i]);
  ents[n] = *add;

  int *ga = (int *)tdb_malloc(sizeof(int) * (size_t)total);
  int *gb = (int *)tdb_malloc(sizeof(int) * (size_t)total);
  if (!ga || !gb) { tdb_mfree(ents); tdb_mfree(ga); tdb_mfree(gb); return TDB_NOMEM; }
  int na = 0, nb = 0;
  quad_split(p, ents, total, ga, &na, gb, &nb);

  int type = nd_type(pg);
  tdb_page *sib; int rc = tdb_pager_alloc(p, &sib);
  if (rc) { tdb_mfree(ents); tdb_mfree(ga); tdb_mfree(gb); return rc; }
  nd_init(sib, type);
  for (int i = 0; i < nb; i++) nd_put(sib, i, &ents[gb[i]]);
  nd_set_count(sib, nb);
  tdb_pager_write(p, sib);

  /* rewrite pg with group A */
  nd_init(pg, type);
  for (int i = 0; i < na; i++) nd_put(pg, i, &ents[ga[i]]);
  nd_set_count(pg, na);
  tdb_pager_write(p, pg);

  out->valid = 1;
  out->pgno = sib->pgno;
  nd_cover(sib, &out->bb);
  tdb_pager_unref(p, sib);
  tdb_mfree(ents); tdb_mfree(ga); tdb_mfree(gb);
  return TDB_OK;
}

/* ------------------------------ insert -------------------------------- */

static int node_insert(tdb_pager *p, tdb_pgno pgno, const tdb_bbox *bb,
                       uint64_t payload, rt_split *split) {
  tdb_page *pg; int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  split->valid = 0;

  if (nd_is_leaf(pg)) {
    rtent e; e.bb = *bb; e.pl = payload;
    rc = nd_add_or_split(p, pg, &e, split);
    tdb_pager_unref(p, pg);
    return rc;
  }

  /* interior: choose the child needing the least enlargement */
  int n = nd_count(pg);
  int best = -1; double best_enl = 1e308, best_area = 1e308;
  rtent e;
  for (int i = 0; i < n; i++) {
    nd_get(pg, i, &e);
    double enl = bb_enlarge(&e.bb, bb), ar = bb_area(&e.bb);
    if (enl < best_enl || (enl == best_enl && ar < best_area)) {
      best_enl = enl; best_area = ar; best = i;
    }
  }
  if (best < 0) { tdb_pager_unref(p, pg); return TDB_CORRUPT; }

  rtent chosen; nd_get(pg, best, &chosen);
  tdb_pgno child = (tdb_pgno)chosen.pl;
  rt_split cs; cs.valid = 0;
  rc = node_insert(p, child, bb, payload, &cs);
  if (rc) { tdb_pager_unref(p, pg); return rc; }

  /* refresh the chosen entry's cover (child grew) */
  tdb_bbox cover;
  rc = child_cover(p, child, &cover);
  if (rc) { tdb_pager_unref(p, pg); return rc; }
  chosen.bb = cover;
  nd_put(pg, best, &chosen);

  if (cs.valid) {
    rtent ne; ne.bb = cs.bb; ne.pl = (uint64_t)cs.pgno;
    rc = nd_add_or_split(p, pg, &ne, split);   /* writes pg */
  } else {
    tdb_pager_write(p, pg);
  }
  tdb_pager_unref(p, pg);
  return rc;
}

int tdb_rtree_insert(tdb_pager *p, tdb_pgno root, const tdb_bbox *bb, tdb_rowid rowid) {
  rt_split split; split.valid = 0;
  int rc = node_insert(p, root, bb, (uint64_t)rowid, &split);
  if (rc || !split.valid) return rc;

  /* root split: keep `root` stable by moving its contents into a new child
  ** and turning the root into a two-entry interior node. */
  tdb_page *rp; rc = tdb_pager_get(p, root, &rp);
  if (rc) return rc;
  int old_type = nd_type(rp);

  tdb_page *childA; rc = tdb_pager_alloc(p, &childA);
  if (rc) { tdb_pager_unref(p, rp); return rc; }
  memcpy(childA->data, rp->data, tdb_pager_page_size(p));
  childA->data[0] = (uint8_t)old_type;
  tdb_pager_write(p, childA);
  tdb_bbox coverA; nd_cover(childA, &coverA);
  tdb_pgno childA_no = childA->pgno;
  tdb_pager_unref(p, childA);

  nd_init(rp, TDB_PAGE_RTREE_INTERIOR);
  rtent ea; ea.bb = coverA; ea.pl = (uint64_t)childA_no;
  rtent eb; eb.bb = split.bb; eb.pl = (uint64_t)split.pgno;
  nd_put(rp, 0, &ea);
  nd_put(rp, 1, &eb);
  nd_set_count(rp, 2);
  tdb_pager_write(p, rp);
  tdb_pager_unref(p, rp);
  return TDB_OK;
}

/* ------------------------------ create -------------------------------- */

int tdb_rtree_create(tdb_pager *p, tdb_pgno *root_out) {
  tdb_page *pg; int rc = tdb_pager_alloc(p, &pg);
  if (rc) return rc;
  nd_init(pg, TDB_PAGE_RTREE_LEAF);
  tdb_pager_write(p, pg);
  *root_out = pg->pgno;
  tdb_pager_unref(p, pg);
  return TDB_OK;
}

/* ------------------------------ search -------------------------------- */

static int node_search(tdb_pager *p, tdb_pgno pgno, const tdb_bbox *q,
                       tdb_rtree_visit cb, void *ctx) {
  tdb_page *pg; int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  int n = nd_count(pg), leaf = nd_is_leaf(pg);
  /* snapshot child descents so we can unref before recursing (bounded fan-out) */
  for (int i = 0; i < n; i++) {
    rtent e; nd_get(pg, i, &e);
    if (!tdb_bbox_intersects(&e.bb, q)) continue;
    if (leaf) {
      int stop = cb(ctx, (tdb_rowid)e.pl);
      if (stop) { tdb_pager_unref(p, pg); return stop; }
    } else {
      tdb_pgno child = (tdb_pgno)e.pl;
      rc = node_search(p, child, q, cb, ctx);
      if (rc) { tdb_pager_unref(p, pg); return rc; }
    }
  }
  tdb_pager_unref(p, pg);
  return TDB_OK;
}

int tdb_rtree_search(tdb_pager *p, tdb_pgno root, const tdb_bbox *q,
                     tdb_rtree_visit cb, void *ctx) {
  return node_search(p, root, q, cb, ctx);
}

/* ------------------------------ delete -------------------------------- */

static int node_delete(tdb_pager *p, tdb_pgno pgno, const tdb_bbox *bb,
                       tdb_rowid rowid, int *found) {
  tdb_page *pg; int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  int n = nd_count(pg);

  if (nd_is_leaf(pg)) {
    for (int i = 0; i < n; i++) {
      rtent e; nd_get(pg, i, &e);
      if (e.pl == (uint64_t)rowid) {
        for (int j = i + 1; j < n; j++) { rtent m; nd_get(pg, j, &m); nd_put(pg, j - 1, &m); }
        nd_set_count(pg, n - 1);
        tdb_pager_write(p, pg);
        *found = 1;
        break;
      }
    }
    tdb_pager_unref(p, pg);
    return TDB_OK;
  }

  for (int i = 0; i < n && !*found; i++) {
    rtent e; nd_get(pg, i, &e);
    if (!tdb_bbox_intersects(&e.bb, bb)) continue;
    tdb_pgno child = (tdb_pgno)e.pl;
    rc = node_delete(p, child, bb, rowid, found);
    if (rc) { tdb_pager_unref(p, pg); return rc; }
    if (*found) {
      tdb_bbox cover;
      rc = child_cover(p, child, &cover);
      if (rc) { tdb_pager_unref(p, pg); return rc; }
      e.bb = cover; nd_put(pg, i, &e);
      tdb_pager_write(p, pg);
    }
  }
  tdb_pager_unref(p, pg);
  return TDB_OK;
}

int tdb_rtree_delete(tdb_pager *p, tdb_pgno root, const tdb_bbox *bb,
                     tdb_rowid rowid, int *found) {
  *found = 0;
  return node_delete(p, root, bb, rowid, found);
}

/* ------------------------------ destroy ------------------------------- */

static int node_destroy(tdb_pager *p, tdb_pgno pgno) {
  tdb_page *pg; int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  int n = nd_count(pg), leaf = nd_is_leaf(pg);
  tdb_pgno *kids = NULL;
  if (!leaf && n > 0) {
    kids = (tdb_pgno *)tdb_malloc(sizeof(tdb_pgno) * (size_t)n);
    for (int i = 0; i < n; i++) { rtent e; nd_get(pg, i, &e); kids[i] = (tdb_pgno)e.pl; }
  }
  tdb_pager_unref(p, pg);
  if (!leaf) for (int i = 0; i < n && !rc; i++) rc = node_destroy(p, kids[i]);
  tdb_mfree(kids);
  if (!rc) rc = tdb_pager_free(p, pgno);
  return rc;
}

int tdb_rtree_destroy(tdb_pager *p, tdb_pgno root) {
  return node_destroy(p, root);
}
