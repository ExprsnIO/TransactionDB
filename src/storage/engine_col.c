/* engine_col.c — columnar (DSM) storage engine implementing tdb_storage.
**
** A columnar table stores each column's values in its own b-tree keyed by
** rowid, plus a `meta` b-tree (t->root) keyed by rowid holding the MVCC header
** [xmin][xmax][prev_vid], and a `history` b-tree (t->history_root) of
** superseded full row versions (row-major, like the row engine). The current
** version of a row is reassembled on read from the per-column b-trees; older
** versions come from the history store. MVCC visibility is applied exactly as
** in the row engine (snapshot or AS OF), so columnar tables behave identically
** through the vtable — only the on-disk layout differs (true column-major).
**
** Projection pushdown (reading only the needed columns) and per-column
** compression are natural follow-ups; today scans reassemble the full record.
*/
#include "tdb_storage.h"
#include "tdb_btree.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"
#include "../common/tdb_buf.h"
#include "../value/tdb_record.h"
#include "../txn/tdb_txn.h"

#include <string.h>

typedef struct { tdb_pager *pager; tdb_buf scratch; } col_engine;

struct tdb_scan {
  tdb_storage *s;
  tdb_txn     *txn;
  tdb_table   *t;
  tdb_btree   *meta;
  tdb_cursor  *cur;
  tdb_buf      rec, work;
  tdb_txnid    as_of;
  const uint8_t *colmask;   /* projection pushdown: NULL = all columns */
};

/* ----------------------- header codec (row-format) -------------------- */

static int put_hdr(uint8_t *buf, tdb_txnid xmin, tdb_txnid xmax, uint64_t prev) {
  int n = 0;
  n += tdb_put_varint(buf + n, xmin);
  n += tdb_put_varint(buf + n, xmax);
  n += tdb_put_varint(buf + n, prev);
  return n;
}
static const uint8_t *get_hdr(const uint8_t *p, int len, tdb_txnid *xmin,
                              tdb_txnid *xmax, uint64_t *prev, int *reclen) {
  const uint8_t *start = p;
  p += tdb_get_varint(p, 10, xmin);
  p += tdb_get_varint(p, 10, xmax);
  p += tdb_get_varint(p, 10, prev);
  *reclen = len - (int)(p - start);
  return p;
}
static int wrap_hist(tdb_buf *out, tdb_txnid xmin, tdb_txnid xmax, uint64_t prev,
                     const uint8_t *rec, int reclen) {
  uint8_t hdr[30]; int hn = put_hdr(hdr, xmin, xmax, prev);
  tdb_buf_reset(out);
  int rc = tdb_buf_append(out, hdr, (size_t)hn);
  if (!rc) rc = tdb_buf_append(out, rec, (size_t)reclen);
  return rc;
}

/* ----------------------------- column I/O ----------------------------- */

static int col_put(col_engine *e, tdb_pgno root, tdb_rowid rowid, const tdb_value *v) {
  tdb_btree *bt; int rc = tdb_btree_open(e->pager, root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_buf cell; tdb_buf_init(&cell);
  rc = tdb_record_encode(v, 1, &cell);
  if (!rc) rc = tdb_btree_put(bt, rowid, cell.data, (int)cell.len);
  tdb_buf_free(&cell);
  tdb_btree_close(bt);
  return rc;
}
static void col_get(col_engine *e, tdb_pgno root, tdb_rowid rowid, tdb_value *out) {
  tdb_value_init(out);
  tdb_btree *bt; if (tdb_btree_open(e->pager, root, TDB_BT_TABLE, NULL, &bt)) return;
  tdb_cursor *c; if (tdb_cursor_open(bt, &c)) { tdb_btree_close(bt); return; }
  int found = 0;
  if (tdb_cursor_seek_rowid(c, rowid, &found) == TDB_OK && found) {
    const uint8_t *v; int n;
    if (tdb_cursor_data(c, &v, &n) == TDB_OK) {
      tdb_value dec[1]; int nc = 0;
      if (tdb_record_decode(v, (size_t)n, dec, 1, &nc) == TDB_OK && nc >= 1)
        tdb_value_copy(out, &dec[0]);
      for (int i = 0; i < nc; i++) tdb_value_clear(&dec[i]);
    }
  }
  tdb_cursor_close(c);
  tdb_btree_close(bt);
}

/* reassemble the current row record from the per-column b-trees; if `colmask`
** is non-NULL, only the marked columns are read (others left NULL) */
static int reassemble(col_engine *e, tdb_table *t, tdb_rowid rowid,
                      const uint8_t *colmask, tdb_buf *out) {
  int nc = t->ncol < t->ncol_roots ? t->ncol : t->ncol_roots;
  tdb_value vals[64];
  if (nc > 64) nc = 64;
  for (int i = 0; i < nc; i++) {
    if (colmask && !colmask[i]) tdb_value_init(&vals[i]);   /* pruned -> NULL */
    else col_get(e, t->col_roots[i], rowid, &vals[i]);
  }
  tdb_buf_reset(out);
  int rc = tdb_record_encode(vals, nc, out);
  for (int i = 0; i < nc; i++) tdb_value_clear(&vals[i]);
  return rc;
}

/* ------------------------------- indexes ------------------------------ */

static void build_keyinfo(const tdb_table *t, const tdb_index *ix, tdb_keyinfo *ki,
                          tdb_collation *coll, uint8_t *desc) {
  int n = ix->ncol;
  for (int i = 0; i < n; i++) { coll[i] = t->cols[ix->col_idx[i]].coll; desc[i] = ix->desc ? ix->desc[i] : 0; }
  coll[n] = TDB_COLL_BINARY; desc[n] = 0;
  ki->ncol = n + 1; ki->coll = coll; ki->desc = desc;
}
static int build_index_key(const tdb_table *t, const tdb_index *ix,
                           const uint8_t *rec, int reclen, tdb_rowid rowid, tdb_buf *out) {
  tdb_value vals[32]; int nc = 0;
  if (tdb_record_decode(rec, (size_t)reclen, vals, 32, &nc)) return TDB_CORRUPT;
  tdb_value kv[33]; int kn = 0;
  for (int i = 0; i < ix->ncol; i++) {
    int ci = ix->col_idx[i]; tdb_value_init(&kv[kn]);
    if (ci < nc) tdb_value_copy(&kv[kn], &vals[ci]);
    kn++;
  }
  tdb_value_init(&kv[kn]); tdb_value_set_int(&kv[kn], (int64_t)rowid); kn++;
  tdb_buf_reset(out);
  int rc = tdb_record_encode(kv, kn, out);
  for (int i = 0; i < kn; i++) tdb_value_clear(&kv[i]);
  for (int i = 0; i < nc; i++) tdb_value_clear(&vals[i]);
  return rc;
}
static int index_insert(col_engine *e, tdb_table *t, const uint8_t *rec, int reclen,
                        tdb_rowid rowid) {
  for (int k = 0; k < t->nindex; k++) {
    tdb_index *ix = &t->indexes[k];
    tdb_keyinfo ki; tdb_collation coll[34]; uint8_t desc[34];
    build_keyinfo(t, ix, &ki, coll, desc);
    tdb_btree *ib; if (tdb_btree_open(e->pager, ix->root, TDB_BT_INDEX, &ki, &ib)) continue;
    tdb_buf key; tdb_buf_init(&key);
    if (build_index_key(t, ix, rec, reclen, rowid, &key) == TDB_OK)
      tdb_index_put(ib, key.data, (int)key.len);
    tdb_buf_free(&key);
    tdb_btree_close(ib);
  }
  return TDB_OK;
}

/* ------------------------------ visibility ---------------------------- */

/* resolve the version of `rowid` visible to txn/as_of into `out` */
static int resolve_col(col_engine *e, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                       const uint8_t *metaval, int metalen, tdb_buf *out,
                       int *found, tdb_txnid as_of, const uint8_t *colmask) {
  *found = 0;
  tdb_txnid xmin, xmax; uint64_t prev; int rl;
  get_hdr(metaval, metalen, &xmin, &xmax, &prev, &rl);
  int vis = as_of ? tdb_txn_visible_asof(txn, xmin, xmax, as_of)
                  : tdb_txn_visible(txn, xmin, xmax);
  if (vis) { *found = 1; return reassemble(e, t, rowid, colmask, out); }
  if (prev == 0) return TDB_OK;

  /* walk the history chain (full records) */
  tdb_btree *hist; int rc = tdb_btree_open(e->pager, t->history_root, TDB_BT_TABLE, NULL, &hist);
  if (rc) return rc;
  tdb_cursor *hc; rc = tdb_cursor_open(hist, &hc);
  if (rc) { tdb_btree_close(hist); return rc; }
  uint64_t vid = prev;
  while (vid) {
    int f = 0;
    if (tdb_cursor_seek_rowid(hc, (tdb_rowid)vid, &f) || !f) break;
    const uint8_t *v; int n;
    if (tdb_cursor_data(hc, &v, &n)) break;
    tdb_txnid hx, hxm; uint64_t hp; int hrl;
    const uint8_t *hrec = get_hdr(v, n, &hx, &hxm, &hp, &hrl);
    int hv = as_of ? tdb_txn_visible_asof(txn, hx, hxm, as_of)
                   : tdb_txn_visible(txn, hx, hxm);
    if (hv) {
      tdb_buf_reset(out);
      rc = tdb_buf_append(out, hrec, (size_t)hrl);
      *found = (rc == TDB_OK);
      break;
    }
    vid = hp;
  }
  tdb_cursor_close(hc);
  tdb_btree_close(hist);
  return rc;
}

/* ----------------------------- vtable impl ---------------------------- */

static int ceng_create_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  col_engine *e = (col_engine *)s->impl; TDB_UNUSED(txn);
  int rc = tdb_btree_create(e->pager, TDB_BT_TABLE, &t->root);         /* meta */
  if (!rc) rc = tdb_btree_create(e->pager, TDB_BT_TABLE, &t->history_root);
  if (rc) return rc;
  t->col_roots = (tdb_pgno *)tdb_malloc(sizeof(tdb_pgno) * (size_t)(t->ncol ? t->ncol : 1));
  if (!t->col_roots) return TDB_NOMEM;
  t->ncol_roots = t->ncol;
  for (int i = 0; i < t->ncol; i++) {
    rc = tdb_btree_create(e->pager, TDB_BT_TABLE, &t->col_roots[i]);
    if (rc) return rc;
  }
  return TDB_OK;
}

static int ceng_drop_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  TDB_UNUSED(txn);
  col_engine *e = (col_engine *)s->impl;
  int rc = tdb_btree_destroy(e->pager, t->root, TDB_BT_TABLE);   /* per-rowid meta */
  for (int i = 0; i < t->ncol_roots && !rc; i++)
    rc = tdb_btree_destroy(e->pager, t->col_roots[i], TDB_BT_TABLE);
  for (int i = 0; i < t->nindex && !rc; i++)
    rc = tdb_btree_destroy(e->pager, t->indexes[i].root, TDB_BT_INDEX);
  return rc;
}

static int ceng_next_rowid(tdb_storage *s, tdb_table *t, tdb_rowid *out) {
  col_engine *e = (col_engine *)s->impl;
  tdb_btree *bt; int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_rowid mx = 0; rc = tdb_btree_max_rowid(bt, &mx);
  tdb_btree_close(bt);
  *out = mx + 1;
  return rc;
}

static int meta_get(col_engine *e, tdb_table *t, tdb_rowid rowid, tdb_buf *out, int *found) {
  *found = 0;
  tdb_btree *bt; int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_cursor *c; rc = tdb_cursor_open(bt, &c);
  if (!rc) {
    int f = 0;
    rc = tdb_cursor_seek_rowid(c, rowid, &f);
    if (!rc && f) {
      const uint8_t *v; int n;
      rc = tdb_cursor_data(c, &v, &n);
      if (!rc) { tdb_buf_reset(out); rc = tdb_buf_append(out, v, (size_t)n); *found = 1; }
    }
    tdb_cursor_close(c);
  }
  tdb_btree_close(bt);
  return rc;
}
static int meta_put(col_engine *e, tdb_table *t, tdb_rowid rowid,
                    tdb_txnid xmin, tdb_txnid xmax, uint64_t prev) {
  tdb_btree *bt; int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  uint8_t hdr[30]; int hn = put_hdr(hdr, xmin, xmax, prev);
  rc = tdb_btree_put(bt, rowid, hdr, hn);
  tdb_btree_close(bt);
  return rc;
}

static int ceng_insert(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                       const uint8_t *rec, int reclen) {
  col_engine *e = (col_engine *)s->impl;
  int rc = meta_put(e, t, rowid, txn->id, 0, 0);
  if (rc) return rc;
  tdb_value vals[64]; int nc = 0;
  if (tdb_record_decode(rec, (size_t)reclen, vals, 64, &nc)) return TDB_CORRUPT;
  for (int i = 0; i < t->ncol && i < t->ncol_roots; i++) {
    tdb_value nullv; tdb_value_init(&nullv);
    rc = col_put(e, t->col_roots[i], rowid, i < nc ? &vals[i] : &nullv);
    tdb_value_clear(&nullv);
    if (rc) break;
  }
  for (int i = 0; i < nc; i++) tdb_value_clear(&vals[i]);
  if (!rc) rc = index_insert(e, t, rec, reclen, rowid);
  return rc;
}

static int ceng_update(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                       const uint8_t *rec, int reclen) {
  col_engine *e = (col_engine *)s->impl;
  tdb_buf meta; tdb_buf_init(&meta); int found = 0;
  int rc = meta_get(e, t, rowid, &meta, &found);
  if (rc || !found) { tdb_buf_free(&meta); return rc ? rc : TDB_NOTFOUND; }
  tdb_txnid oxmin, oxmax; uint64_t oprev; int orl;
  get_hdr(meta.data, (int)meta.len, &oxmin, &oxmax, &oprev, &orl);
  tdb_buf_free(&meta);

  /* push the old full version to history (all columns) */
  tdb_buf old; tdb_buf_init(&old);
  rc = reassemble(e, t, rowid, NULL, &old);
  tdb_btree *hist; if (!rc) rc = tdb_btree_open(e->pager, t->history_root, TDB_BT_TABLE, NULL, &hist);
  if (rc) { tdb_buf_free(&old); return rc; }
  tdb_rowid vid = 0; tdb_btree_max_rowid(hist, &vid); vid += 1;
  tdb_buf hbuf; tdb_buf_init(&hbuf);
  rc = wrap_hist(&hbuf, oxmin, txn->id, oprev, old.data, (int)old.len);
  if (!rc) rc = tdb_btree_put(hist, vid, hbuf.data, (int)hbuf.len);
  tdb_buf_free(&hbuf); tdb_buf_free(&old); tdb_btree_close(hist);
  if (rc) return rc;

  /* write the new version: meta + each column */
  rc = meta_put(e, t, rowid, txn->id, 0, vid);
  if (!rc) {
    tdb_value vals[64]; int nc = 0;
    if (tdb_record_decode(rec, (size_t)reclen, vals, 64, &nc)) return TDB_CORRUPT;
    for (int i = 0; i < t->ncol && i < t->ncol_roots && !rc; i++) {
      tdb_value nullv; tdb_value_init(&nullv);
      rc = col_put(e, t->col_roots[i], rowid, i < nc ? &vals[i] : &nullv);
      tdb_value_clear(&nullv);
    }
    for (int i = 0; i < nc; i++) tdb_value_clear(&vals[i]);
  }
  if (!rc) rc = index_insert(e, t, rec, reclen, rowid);
  return rc;
}

static int ceng_remove(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid) {
  col_engine *e = (col_engine *)s->impl;
  tdb_buf meta; tdb_buf_init(&meta); int found = 0;
  int rc = meta_get(e, t, rowid, &meta, &found);
  if (!rc && found) {
    tdb_txnid oxmin, oxmax; uint64_t oprev; int orl;
    get_hdr(meta.data, (int)meta.len, &oxmin, &oxmax, &oprev, &orl);
    rc = meta_put(e, t, rowid, oxmin, txn->id, oprev); /* tombstone */
  } else if (!rc) rc = TDB_NOTFOUND;
  tdb_buf_free(&meta);
  return rc;
}

static int ceng_seek_rowid(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                           const uint8_t **rec, int *reclen, int *found) {
  col_engine *e = (col_engine *)s->impl;
  *found = 0; *rec = NULL; *reclen = 0;
  tdb_buf meta; tdb_buf_init(&meta); int mf = 0;
  int rc = meta_get(e, t, rowid, &meta, &mf);
  if (!rc && mf) {
    rc = resolve_col(e, txn, t, rowid, meta.data, (int)meta.len, &e->scratch, found, 0, NULL);
    if (!rc && *found) { *rec = e->scratch.data; *reclen = (int)e->scratch.len; }
  }
  tdb_buf_free(&meta);
  return rc;
}

static int ceng_scan_open(tdb_storage *s, tdb_txn *txn, tdb_table *t,
                          tdb_index *use_idx, const tdb_keyrange *range,
                          tdb_txnid as_of, const uint8_t *colmask, tdb_scan **out) {
  TDB_UNUSED(use_idx); TDB_UNUSED(range); /* columnar index scans: future */
  col_engine *e = (col_engine *)s->impl;
  tdb_scan *sc = (tdb_scan *)tdb_calloc(sizeof(*sc));
  if (!sc) return TDB_NOMEM;
  sc->s = s; sc->txn = txn; sc->t = t; sc->as_of = as_of; sc->colmask = colmask;
  tdb_buf_init(&sc->rec); tdb_buf_init(&sc->work);
  int rc = tdb_btree_open(e->pager, t->root, TDB_BT_TABLE, NULL, &sc->meta);
  if (!rc) rc = tdb_cursor_open(sc->meta, &sc->cur);
  if (!rc) rc = tdb_cursor_first(sc->cur);
  if (rc) { s->vtab->scan_close(sc); return rc; }
  *out = sc;
  return TDB_OK;
}

static int ceng_scan_next(tdb_scan *sc, tdb_rowid *rowid, const uint8_t **rec, int *reclen) {
  col_engine *e = (col_engine *)sc->s->impl;
  while (!tdb_cursor_eof(sc->cur)) {
    const uint8_t *m; int mn;
    if (tdb_cursor_data(sc->cur, &m, &mn)) return TDB_ERROR;
    tdb_rowid rid; tdb_cursor_rowid(sc->cur, &rid);
    /* copy meta before advancing (cursor buffer is transient) */
    tdb_buf_reset(&sc->work);
    tdb_buf_append(&sc->work, m, (size_t)mn);
    tdb_cursor_next(sc->cur);
    int found = 0;
    int rc = resolve_col(e, sc->txn, sc->t, rid, sc->work.data, (int)sc->work.len,
                         &sc->rec, &found, sc->as_of, sc->colmask);
    if (rc) return rc;
    if (found) { *rowid = rid; *rec = sc->rec.data; *reclen = (int)sc->rec.len; return TDB_ROW; }
  }
  return TDB_DONE;
}

static void ceng_scan_close(tdb_scan *sc) {
  if (!sc) return;
  if (sc->cur) tdb_cursor_close(sc->cur);
  if (sc->meta) tdb_btree_close(sc->meta);
  tdb_buf_free(&sc->rec); tdb_buf_free(&sc->work);
  tdb_mfree(sc);
}

static int ceng_create_index(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_index *ix) {
  col_engine *e = (col_engine *)s->impl;
  int rc = tdb_btree_create(e->pager, TDB_BT_INDEX, &ix->root);
  if (rc) return rc;
  tdb_scan *sc; rc = s->vtab->scan_open(s, txn, t, NULL, NULL, 0, NULL, &sc);
  if (rc) return rc;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = s->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW)
    index_insert(e, t, rec, reclen, rid);
  s->vtab->scan_close(sc);
  return (rc == TDB_DONE) ? TDB_OK : rc;
}

static void ceng_close(tdb_storage *s) {
  if (!s) return;
  col_engine *e = (col_engine *)s->impl;
  tdb_buf_free(&e->scratch);
  tdb_mfree(e);
  tdb_mfree(s);
}

static int ceng_add_column(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  TDB_UNUSED(txn);
  col_engine *e = (col_engine *)s->impl;
  tdb_pgno *grown = (tdb_pgno *)tdb_realloc(t->col_roots, sizeof(tdb_pgno) * (size_t)t->ncol);
  if (!grown) return TDB_NOMEM;
  t->col_roots = grown;
  int rc = tdb_btree_create(e->pager, TDB_BT_TABLE, &t->col_roots[t->ncol - 1]);
  if (rc) return rc;
  t->ncol_roots = t->ncol;
  return TDB_OK;
}

/* Drop column `ci`'s value b-tree by removing its root from the array and
** shifting the rest down. The b-tree's pages are orphaned until VACUUM. */
static int ceng_drop_column(tdb_storage *s, tdb_txn *txn, tdb_table *t, int ci) {
  TDB_UNUSED(txn);
  col_engine *e = (col_engine *)s->impl;
  if (ci < 0 || ci >= t->ncol_roots) {
    if (t->ncol_roots > 0) t->ncol_roots--;   /* no separate store; just shrink */
    return TDB_OK;
  }
  int rc = tdb_btree_destroy(e->pager, t->col_roots[ci], TDB_BT_TABLE);  /* reclaim pages */
  for (int i = ci; i < t->ncol_roots - 1; i++) t->col_roots[i] = t->col_roots[i + 1];
  t->ncol_roots--;
  return rc;
}

static const tdb_storage_vtab g_col_vtab = {
  "columnar", ceng_close,
  ceng_create_table, ceng_drop_table, ceng_create_index,
  ceng_insert, ceng_update, ceng_remove, ceng_next_rowid,
  ceng_scan_open, ceng_scan_next, ceng_scan_close, ceng_seek_rowid,
  ceng_add_column, ceng_drop_column,
};

int tdb_engine_columnar_open(tdb_pager *p, tdb_storage **out) {
  col_engine *e = (col_engine *)tdb_calloc(sizeof(*e));
  tdb_storage *s = (tdb_storage *)tdb_calloc(sizeof(*s));
  if (!e || !s) { tdb_mfree(e); tdb_mfree(s); return TDB_NOMEM; }
  e->pager = p; tdb_buf_init(&e->scratch);
  s->vtab = &g_col_vtab; s->impl = e;
  *out = s;
  return TDB_OK;
}
