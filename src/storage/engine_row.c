/* engine_row.c — row-oriented storage engine implementing tdb_storage.
**
** Physical row value layout (the b-tree value for a rowid):
**   [ varint xmin ][ varint xmax ][ varint prev_vid ][ user record bytes... ]
**
**   xmin     creating transaction id
**   xmax     deleting/superseding transaction id (0 = live)
**   prev_vid key into the table's history b-tree of the prior version (0=none)
**
** UPDATE pushes the old version into the history (version store) b-tree keyed
** by a monotonic version id and links it via prev_vid, then writes the new
** version in place. DELETE sets xmax in place (a tombstone). Reads walk the
** prev_vid chain and return the first version visible to the transaction's
** snapshot, giving snapshot-isolation MVCC. The history b-tree doubles as the
** retained version store for system-versioned temporal tables.
*/
#include "tdb_storage.h"
#include "tdb_btree.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"
#include "../common/tdb_buf.h"
#include "../value/tdb_record.h"

#include <string.h>

typedef struct { tdb_pager *pager; tdb_buf scratch; } row_engine;

struct tdb_scan {
  tdb_storage *s;
  tdb_txn     *txn;
  tdb_table   *t;
  tdb_btree   *tbl;
  tdb_btree   *hist;
  tdb_cursor  *cur;        /* table cursor (full scan) or index cursor */
  tdb_buf      rec;   /* resolved record for the current row */
  tdb_buf      work;  /* working buffer for chain walking */

  /* index-driven scan state (idx != NULL) */
  tdb_index   *idx;
  tdb_btree   *ixbt;
  tdb_keyinfo  ki;
  tdb_collation coll[34];
  uint8_t      desc[34];
  tdb_keyrange range;
  int          has_range;
};

/* --------------------------- row header codec ------------------------- */

static int put_rowhdr(uint8_t *buf, tdb_txnid xmin, tdb_txnid xmax, uint64_t prev) {
  int n = 0;
  n += tdb_put_varint(buf + n, xmin);
  n += tdb_put_varint(buf + n, xmax);
  n += tdb_put_varint(buf + n, prev);
  return n;
}

static const uint8_t *get_rowhdr(const uint8_t *p, int len, tdb_txnid *xmin,
                                 tdb_txnid *xmax, uint64_t *prev, int *reclen) {
  const uint8_t *start = p;
  int a;
  a = tdb_get_varint(p, 10, xmin); p += a;
  a = tdb_get_varint(p, 10, xmax); p += a;
  a = tdb_get_varint(p, 10, prev); p += a;
  *reclen = len - (int)(p - start);
  return p;
}

/* Build a wrapped row value into `out` (reset first). */
static int wrap_row(tdb_buf *out, tdb_txnid xmin, tdb_txnid xmax, uint64_t prev,
                    const uint8_t *rec, int reclen) {
  uint8_t hdr[30];
  int hn = put_rowhdr(hdr, xmin, xmax, prev);
  tdb_buf_reset(out);
  int rc = tdb_buf_append(out, hdr, (size_t)hn);
  if (rc) return rc;
  return tdb_buf_append(out, rec, (size_t)reclen);
}

/* ------------------------------- indexes ------------------------------ */

static void build_keyinfo(const tdb_table *t, const tdb_index *ix, tdb_keyinfo *ki,
                          tdb_collation *coll, uint8_t *desc) {
  int n = ix->ncol;
  for (int i = 0; i < n; i++) {
    coll[i] = t->cols[ix->col_idx[i]].coll;
    desc[i] = ix->desc ? ix->desc[i] : 0;
  }
  coll[n] = TDB_COLL_BINARY; /* trailing rowid */
  desc[n] = 0;
  ki->ncol = n + 1;
  ki->coll = coll;
  ki->desc = desc;
}

/* Encode an index key = (indexed column values..., rowid) as a record. */
static int build_index_key(const tdb_table *t, const tdb_index *ix,
                           const uint8_t *rec, int reclen, tdb_rowid rowid,
                           tdb_buf *out) {
  tdb_value vals[32];
  int nc = 0;
  if (tdb_record_decode(rec, (size_t)reclen, vals, 32, &nc)) return TDB_CORRUPT;
  tdb_value keyvals[33];
  int kn = 0;
  for (int i = 0; i < ix->ncol; i++) {
    int ci = ix->col_idx[i];
    tdb_value_init(&keyvals[kn]);
    if (ci < nc) tdb_value_copy(&keyvals[kn], &vals[ci]);
    kn++;
  }
  tdb_value_init(&keyvals[kn]);
  tdb_value_set_int(&keyvals[kn], (int64_t)rowid);
  kn++;
  tdb_buf_reset(out);
  int rc = tdb_record_encode(keyvals, kn, out);
  for (int i = 0; i < kn; i++) tdb_value_clear(&keyvals[i]);
  for (int i = 0; i < nc; i++) tdb_value_clear(&vals[i]);
  return rc;
}

static int open_index(row_engine *e, tdb_table *t, tdb_index *ix, tdb_btree **out,
                      tdb_keyinfo *ki, tdb_collation *coll, uint8_t *desc) {
  build_keyinfo(t, ix, ki, coll, desc);
  return tdb_btree_open(e->pager, ix->root, TDB_BT_INDEX, ki, out);
}

static int index_apply(row_engine *e, tdb_table *t, const uint8_t *rec, int reclen,
                       tdb_rowid rowid, int insert) {
  for (int k = 0; k < t->nindex; k++) {
    tdb_index *ix = &t->indexes[k];
    tdb_keyinfo ki; tdb_collation coll[34]; uint8_t desc[34];
    tdb_btree *ib;
    int rc = open_index(e, t, ix, &ib, &ki, coll, desc);
    if (rc) return rc;
    tdb_buf key; tdb_buf_init(&key);
    rc = build_index_key(t, ix, rec, reclen, rowid, &key);
    if (!rc) {
      if (insert) rc = tdb_index_put(ib, key.data, (int)key.len);
      else { int found; rc = tdb_index_del(ib, key.data, (int)key.len, &found); }
    }
    tdb_buf_free(&key);
    tdb_btree_close(ib);
    if (rc) return rc;
  }
  return TDB_OK;
}

/* ----------------------------- vtable impl ---------------------------- */

static int eng_create_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  row_engine *e = (row_engine *)s->impl;
  TDB_UNUSED(txn);
  int rc = tdb_btree_create(e->pager, TDB_BT_TABLE, &t->root);
  if (rc) return rc;
  return tdb_btree_create(e->pager, TDB_BT_TABLE, &t->history_root);
}

static int eng_drop_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  TDB_UNUSED(s); TDB_UNUSED(txn); TDB_UNUSED(t);
  /* Page reclamation for dropped trees is left to a future vacuum. */
  return TDB_OK;
}

static int eng_create_index(tdb_storage *s, tdb_txn *txn, tdb_table *t,
                            tdb_index *ix) {
  row_engine *e = (row_engine *)s->impl;
  int rc = tdb_btree_create(e->pager, TDB_BT_INDEX, &ix->root);
  if (rc) return rc;
  /* Populate from currently-visible rows. */
  tdb_scan *sc;
  rc = s->vtab->scan_open(s, txn, t, NULL, NULL, &sc);
  if (rc) return rc;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = s->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW) {
    tdb_keyinfo ki; tdb_collation coll[34]; uint8_t desc[34];
    tdb_btree *ib;
    if (open_index(e, t, ix, &ib, &ki, coll, desc) == TDB_OK) {
      tdb_buf key; tdb_buf_init(&key);
      if (build_index_key(t, ix, rec, reclen, rid, &key) == TDB_OK)
        tdb_index_put(ib, key.data, (int)key.len);
      tdb_buf_free(&key);
      tdb_btree_close(ib);
    }
  }
  s->vtab->scan_close(sc);
  return (rc == TDB_DONE) ? TDB_OK : rc;
}

static int eng_next_rowid(tdb_storage *s, tdb_table *t, tdb_rowid *out) {
  row_engine *e = (row_engine *)s->impl;
  tdb_btree *bt;
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_rowid mx = 0;
  rc = tdb_btree_max_rowid(bt, &mx);
  tdb_btree_close(bt);
  if (rc) return rc;
  *out = mx + 1;
  return TDB_OK;
}

static int eng_insert(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                      const uint8_t *rec, int reclen) {
  row_engine *e = (row_engine *)s->impl;
  tdb_btree *bt;
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  rc = wrap_row(&e->scratch, txn->id, 0, 0, rec, reclen);
  if (!rc) rc = tdb_btree_put(bt, rowid, e->scratch.data, (int)e->scratch.len);
  tdb_btree_close(bt);
  if (!rc) rc = index_apply(e, t, rec, reclen, rowid, 1);
  return rc;
}

/* Fetch the raw stored value for a rowid into `into`. */
static int fetch_main(row_engine *e, tdb_btree *bt, tdb_rowid rowid, tdb_buf *into,
                      int *found) {
  tdb_cursor *c; int rc = tdb_cursor_open(bt, &c);
  if (rc) return rc;
  int f = 0;
  rc = tdb_cursor_seek_rowid(c, rowid, &f);
  if (!rc && f) {
    const uint8_t *v; int n;
    rc = tdb_cursor_data(c, &v, &n);
    if (!rc) { tdb_buf_reset(into); rc = tdb_buf_append(into, v, (size_t)n); }
  }
  *found = f;
  tdb_cursor_close(c);
  return rc;
}

static int eng_update(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                      const uint8_t *rec, int reclen) {
  row_engine *e = (row_engine *)s->impl;
  tdb_btree *bt, *hist;
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  rc = tdb_btree_open(e->pager, t->history_root, TDB_BT_TABLE, NULL, &hist);
  if (rc) { tdb_btree_close(bt); return rc; }

  tdb_buf old; tdb_buf_init(&old);
  int found = 0;
  rc = fetch_main(e, bt, rowid, &old, &found);
  if (rc || !found) { tdb_buf_free(&old); tdb_btree_close(bt); tdb_btree_close(hist); return rc ? rc : TDB_NOTFOUND; }

  tdb_txnid oxmin, oxmax; uint64_t oprev; int orl;
  const uint8_t *orec = get_rowhdr(old.data, (int)old.len, &oxmin, &oxmax, &oprev, &orl);

  /* push old version into the history store */
  tdb_rowid vid = 0;
  rc = tdb_btree_max_rowid(hist, &vid);
  vid += 1;
  if (!rc) {
    tdb_buf hbuf; tdb_buf_init(&hbuf);
    rc = wrap_row(&hbuf, oxmin, txn->id, oprev, orec, orl);
    if (!rc) rc = tdb_btree_put(hist, vid, hbuf.data, (int)hbuf.len);
    tdb_buf_free(&hbuf);
  }
  /* Indexes are insert-only (the old key is retained): an MVCC reader on an
  ** older snapshot may still need it. The index scan re-checks each fetched
  ** row's visible version, so stale keys are filtered. (Reclaimed by vacuum.) */

  /* write the new live version */
  if (!rc) {
    rc = wrap_row(&e->scratch, txn->id, 0, vid, rec, reclen);
    if (!rc) rc = tdb_btree_put(bt, rowid, e->scratch.data, (int)e->scratch.len);
  }
  if (!rc) rc = index_apply(e, t, rec, reclen, rowid, 1);

  tdb_buf_free(&old);
  tdb_btree_close(bt);
  tdb_btree_close(hist);
  return rc;
}

static int eng_remove(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid) {
  row_engine *e = (row_engine *)s->impl;
  tdb_btree *bt;
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_buf old; tdb_buf_init(&old);
  int found = 0;
  rc = fetch_main(e, bt, rowid, &old, &found);
  if (!rc && found) {
    tdb_txnid oxmin, oxmax; uint64_t oprev; int orl;
    const uint8_t *orec = get_rowhdr(old.data, (int)old.len, &oxmin, &oxmax, &oprev, &orl);
    /* tombstone: keep the record but set xmax to this txn */
    tdb_buf nb; tdb_buf_init(&nb);
    rc = wrap_row(&nb, oxmin, txn->id, oprev, orec, orl);
    if (!rc) rc = tdb_btree_put(bt, rowid, nb.data, (int)nb.len);
    tdb_buf_free(&nb);
  } else if (!rc) {
    rc = TDB_NOTFOUND;
  }
  tdb_buf_free(&old);
  tdb_btree_close(bt);
  return rc;
}

/* Resolve the version of `rowid` visible to txn into `out`. */
static int resolve_visible(row_engine *e, tdb_txn *txn, tdb_btree *bt,
                           tdb_btree *hist, const uint8_t *mainval, int mainlen,
                           tdb_buf *work, tdb_buf *out, int *found) {
  TDB_UNUSED(bt);
  *found = 0;
  tdb_buf_reset(work);
  int rc = tdb_buf_append(work, mainval, (size_t)mainlen);
  if (rc) return rc;
  for (;;) {
    tdb_txnid xmin, xmax; uint64_t prev; int rl;
    const uint8_t *rec = get_rowhdr(work->data, (int)work->len, &xmin, &xmax, &prev, &rl);
    if (tdb_txn_visible(txn, xmin, xmax)) {
      tdb_buf_reset(out);
      rc = tdb_buf_append(out, rec, (size_t)rl);
      if (rc) return rc;
      *found = 1;
      return TDB_OK;
    }
    if (prev == 0) return TDB_OK; /* no visible version */
    /* load the previous version from the history store into `work` */
    tdb_cursor *hc; rc = tdb_cursor_open(hist, &hc);
    if (rc) return rc;
    int f = 0;
    rc = tdb_cursor_seek_rowid(hc, (tdb_rowid)prev, &f);
    if (!rc && f) {
      const uint8_t *v; int n;
      rc = tdb_cursor_data(hc, &v, &n);
      if (!rc) { tdb_buf_reset(work); rc = tdb_buf_append(work, v, (size_t)n); }
    } else {
      tdb_cursor_close(hc);
      return TDB_OK; /* broken chain: treat as not visible */
    }
    tdb_cursor_close(hc);
    if (rc) return rc;
  }
}

static int eng_seek_rowid(tdb_storage *s, tdb_txn *txn, tdb_table *t,
                          tdb_rowid rowid, const uint8_t **rec, int *reclen,
                          int *found) {
  row_engine *e = (row_engine *)s->impl;
  *found = 0; *rec = NULL; *reclen = 0;
  tdb_btree *bt, *hist;
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  rc = tdb_btree_open(e->pager, t->history_root, TDB_BT_TABLE, NULL, &hist);
  if (rc) { tdb_btree_close(bt); return rc; }

  tdb_buf mainv; tdb_buf_init(&mainv);
  int f = 0;
  rc = fetch_main(e, bt, rowid, &mainv, &f);
  if (!rc && f) {
    tdb_buf work; tdb_buf_init(&work);
    rc = resolve_visible(e, txn, bt, hist, mainv.data, (int)mainv.len, &work,
                         &e->scratch, found);
    tdb_buf_free(&work);
    if (!rc && *found) { *rec = e->scratch.data; *reclen = (int)e->scratch.len; }
  }
  tdb_buf_free(&mainv);
  tdb_btree_close(bt);
  tdb_btree_close(hist);
  return rc;
}

static int eng_scan_open(tdb_storage *s, tdb_txn *txn, tdb_table *t,
                         tdb_index *use_idx, const tdb_keyrange *range,
                         tdb_scan **out) {
  row_engine *e = (row_engine *)s->impl;
  tdb_scan *sc = (tdb_scan *)tdb_calloc(sizeof(*sc));
  if (!sc) return TDB_NOMEM;
  sc->s = s; sc->txn = txn; sc->t = t;
  tdb_buf_init(&sc->rec); tdb_buf_init(&sc->work);
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &sc->tbl);
  if (!rc) rc = tdb_btree_open(e->pager, t->history_root, TDB_BT_TABLE, NULL, &sc->hist);
  if (rc) { s->vtab->scan_close(sc); return rc; }

  if (use_idx) {
    sc->idx = use_idx;
    build_keyinfo(t, use_idx, &sc->ki, sc->coll, sc->desc);
    rc = tdb_btree_open(e->pager, use_idx->root, TDB_BT_INDEX, &sc->ki, &sc->ixbt);
    if (!rc) rc = tdb_cursor_open(sc->ixbt, &sc->cur);
    if (rc) { s->vtab->scan_close(sc); return rc; }
    if (range) { sc->range = *range; sc->has_range = 1; }
    if (sc->has_range && sc->range.has_lo) {
      tdb_buf kb; tdb_buf_init(&kb);
      tdb_record_encode(&sc->range.lo, 1, &kb);
      int cmp;
      tdb_cursor_seek(sc->cur, kb.data, (int)kb.len, &cmp);
      tdb_buf_free(&kb);
    } else {
      tdb_cursor_first(sc->cur);
    }
  } else {
    rc = tdb_cursor_open(sc->tbl, &sc->cur);
    if (!rc) rc = tdb_cursor_first(sc->cur);
    if (rc) { s->vtab->scan_close(sc); return rc; }
  }
  *out = sc;
  return TDB_OK;
}

/* index-driven scan: walk index entries in key order, fetch + MVCC-verify */
static int scan_next_index(tdb_scan *sc, tdb_rowid *rowid, const uint8_t **rec,
                           int *reclen) {
  row_engine *e = (row_engine *)sc->s->impl;
  while (!tdb_cursor_eof(sc->cur)) {
    const uint8_t *k; int kl;
    if (tdb_cursor_key(sc->cur, &k, &kl)) break;
    tdb_value cols[34]; int nc = 0;
    if (tdb_record_decode(k, (size_t)kl, cols, 34, &nc) || nc < 1) { tdb_cursor_next(sc->cur); continue; }

    int stop = 0, skip = 0;
    if (sc->has_range && sc->range.has_hi) {
      int c = tdb_value_compare(&cols[0], &sc->range.hi, TDB_COLL_BINARY);
      if (c > 0 || (c == 0 && !sc->range.hi_incl)) stop = 1;
    }
    if (!stop && sc->has_range && sc->range.has_lo) {
      int c = tdb_value_compare(&cols[0], &sc->range.lo, TDB_COLL_BINARY);
      if (c < 0 || (c == 0 && !sc->range.lo_incl)) skip = 1;
    }
    tdb_rowid rid = (nc >= 1) ? (tdb_rowid)tdb_value_as_int(&cols[nc - 1]) : 0;
    for (int i = 0; i < nc; i++) tdb_value_clear(&cols[i]);
    if (stop) return TDB_DONE;
    tdb_cursor_next(sc->cur);
    if (skip) continue;

    tdb_buf mainv; tdb_buf_init(&mainv);
    int f = 0;
    int rc = fetch_main(e, sc->tbl, rid, &mainv, &f);
    if (!rc && f) {
      int found = 0;
      rc = resolve_visible(e, sc->txn, sc->tbl, sc->hist, mainv.data, (int)mainv.len,
                           &sc->work, &sc->rec, &found);
      if (!rc && found) {
        tdb_buf_free(&mainv);
        *rowid = rid; *rec = sc->rec.data; *reclen = (int)sc->rec.len;
        return TDB_ROW;
      }
    }
    tdb_buf_free(&mainv);
    if (rc) return rc;
  }
  return TDB_DONE;
}

static int eng_scan_next(tdb_scan *sc, tdb_rowid *rowid, const uint8_t **rec,
                         int *reclen) {
  if (sc->idx) return scan_next_index(sc, rowid, rec, reclen);
  row_engine *e = (row_engine *)sc->s->impl;
  while (!tdb_cursor_eof(sc->cur)) {
    const uint8_t *v; int n;
    int rc = tdb_cursor_data(sc->cur, &v, &n);
    if (rc) return rc;
    tdb_rowid rid; tdb_cursor_rowid(sc->cur, &rid);
    int found = 0;
    rc = resolve_visible(e, sc->txn, sc->tbl, sc->hist, v, n, &sc->work, &sc->rec, &found);
    if (rc) return rc;
    tdb_cursor_next(sc->cur);
    if (found) {
      *rowid = rid;
      *rec = sc->rec.data;
      *reclen = (int)sc->rec.len;
      return TDB_ROW;
    }
  }
  return TDB_DONE;
}

static void eng_scan_close(tdb_scan *sc) {
  if (!sc) return;
  if (sc->cur) tdb_cursor_close(sc->cur);
  if (sc->ixbt) tdb_btree_close(sc->ixbt);
  if (sc->tbl) tdb_btree_close(sc->tbl);
  if (sc->hist) tdb_btree_close(sc->hist);
  tdb_buf_free(&sc->rec);
  tdb_buf_free(&sc->work);
  tdb_mfree(sc);
}

static void eng_close(tdb_storage *s) {
  if (!s) return;
  row_engine *e = (row_engine *)s->impl;
  tdb_buf_free(&e->scratch);
  tdb_mfree(e);
  tdb_mfree(s);
}

static const tdb_storage_vtab g_row_vtab = {
  "row-btree",
  eng_close,
  eng_create_table, eng_drop_table, eng_create_index,
  eng_insert, eng_update, eng_remove, eng_next_rowid,
  eng_scan_open, eng_scan_next, eng_scan_close,
  eng_seek_rowid,
};

int tdb_engine_row_open(tdb_pager *p, tdb_storage **out) {
  row_engine *e = (row_engine *)tdb_calloc(sizeof(*e));
  tdb_storage *s = (tdb_storage *)tdb_calloc(sizeof(*s));
  if (!e || !s) { tdb_mfree(e); tdb_mfree(s); return TDB_NOMEM; }
  e->pager = p;
  tdb_buf_init(&e->scratch);
  s->vtab = &g_row_vtab;
  s->impl = e;
  *out = s;
  return TDB_OK;
}
