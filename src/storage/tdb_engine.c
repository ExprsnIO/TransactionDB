/* tdb_engine.c — storage-engine dispatcher.
**
** A database connection uses this engine, which owns both a row engine and a
** columnar engine and routes each operation to the right one based on the
** table's `columnar` flag. The executor and catalog are oblivious to which
** physical engine backs a given table.
*/
#include "tdb_storage.h"
#include "../common/tdb_mem.h"

typedef struct { tdb_storage *row; tdb_storage *col; } disp_impl;

struct tdb_scan { tdb_storage *sub; tdb_scan *subscan; };

static tdb_storage *pick(tdb_storage *s, const tdb_table *t) {
  disp_impl *d = (disp_impl *)s->impl;
  return (t && t->columnar) ? d->col : d->row;
}

static void d_close(tdb_storage *s) {
  disp_impl *d = (disp_impl *)s->impl;
  tdb_storage_close(d->row);
  tdb_storage_close(d->col);
  tdb_mfree(d);
  tdb_mfree(s);
}

static int d_create_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  tdb_storage *e = pick(s, t); return e->vtab->create_table(e, txn, t);
}
static int d_drop_table(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  tdb_storage *e = pick(s, t); return e->vtab->drop_table(e, txn, t);
}
static int d_create_index(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_index *ix) {
  tdb_storage *e = pick(s, t); return e->vtab->create_index(e, txn, t, ix);
}
static int d_insert(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                    const uint8_t *rec, int reclen) {
  tdb_storage *e = pick(s, t); return e->vtab->insert(e, txn, t, rowid, rec, reclen);
}
static int d_update(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                    const uint8_t *rec, int reclen) {
  tdb_storage *e = pick(s, t); return e->vtab->update(e, txn, t, rowid, rec, reclen);
}
static int d_remove(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid) {
  tdb_storage *e = pick(s, t); return e->vtab->remove(e, txn, t, rowid);
}
static int d_next_rowid(tdb_storage *s, tdb_table *t, tdb_rowid *out) {
  tdb_storage *e = pick(s, t); return e->vtab->next_rowid(e, t, out);
}
static int d_seek_rowid(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                        const uint8_t **rec, int *reclen, int *found) {
  tdb_storage *e = pick(s, t); return e->vtab->seek_rowid(e, txn, t, rowid, rec, reclen, found);
}
static int d_scan_open(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_index *use_idx,
                       const tdb_keyrange *range, tdb_txnid as_of,
                       const uint8_t *colmask, tdb_scan **out) {
  tdb_storage *e = pick(s, t);
  tdb_scan *sub = NULL;
  int rc = e->vtab->scan_open(e, txn, t, use_idx, range, as_of, colmask, &sub);
  if (rc) return rc;
  tdb_scan *w = (tdb_scan *)tdb_calloc(sizeof(*w));
  if (!w) { e->vtab->scan_close(sub); return TDB_NOMEM; }
  w->sub = e; w->subscan = sub;
  *out = w;
  return TDB_OK;
}
static int d_scan_next(tdb_scan *sc, tdb_rowid *rowid, const uint8_t **rec, int *reclen) {
  return sc->sub->vtab->scan_next(sc->subscan, rowid, rec, reclen);
}
static void d_scan_close(tdb_scan *sc) {
  if (!sc) return;
  sc->sub->vtab->scan_close(sc->subscan);
  tdb_mfree(sc);
}

static const tdb_storage_vtab g_disp_vtab = {
  "dispatch", d_close,
  d_create_table, d_drop_table, d_create_index,
  d_insert, d_update, d_remove, d_next_rowid,
  d_scan_open, d_scan_next, d_scan_close, d_seek_rowid,
};

int tdb_engine_open(tdb_pager *p, tdb_storage **out) {
  disp_impl *d = (disp_impl *)tdb_calloc(sizeof(*d));
  tdb_storage *s = (tdb_storage *)tdb_calloc(sizeof(*s));
  if (!d || !s) { tdb_mfree(d); tdb_mfree(s); return TDB_NOMEM; }
  int rc = tdb_engine_row_open(p, &d->row);
  if (!rc) rc = tdb_engine_columnar_open(p, &d->col);
  if (rc) {
    if (d->row) tdb_storage_close(d->row);
    if (d->col) tdb_storage_close(d->col);
    tdb_mfree(d); tdb_mfree(s);
    return rc;
  }
  s->vtab = &g_disp_vtab; s->impl = d;
  *out = s;
  return TDB_OK;
}
