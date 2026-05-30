/*
** tdb_storage.h — pluggable storage-engine interface.
**
** The executor manipulates table data only through this vtable, never the
** b-tree or pager directly. The row-oriented engine (engine_row.c) implements
** it today; a future columnar engine can implement the same contract.
**
** Records passed to insert/update and returned by scans/seek are the *user*
** row records (the application's column values). The engine privately wraps
** each stored version with MVCC bookkeeping (xmin/xmax/version-chain) and
** applies snapshot visibility at scan/seek time using the supplied
** transaction — so visibility is engine-agnostic.
*/
#ifndef TDB_STORAGE_H
#define TDB_STORAGE_H

#include "../common/tdb_internal.h"
#include "../catalog/tdb_schema.h"
#include "../txn/tdb_txn.h"

typedef struct tdb_storage tdb_storage;
typedef struct tdb_scan    tdb_scan;

/* Bounds on an index's leading column for an index-driven scan. */
typedef struct tdb_keyrange {
  int       has_lo, has_hi;   /* whether a lower / upper bound is present */
  int       lo_incl, hi_incl; /* bound inclusivity */
  tdb_value lo, hi;           /* bound values (compared to the leading column) */
} tdb_keyrange;

typedef struct tdb_storage_vtab {
  const char *name;

  void (*close)(tdb_storage *s);

  /* DDL */
  int (*create_table)(tdb_storage *s, tdb_txn *txn, tdb_table *t);
  int (*drop_table)(tdb_storage *s, tdb_txn *txn, tdb_table *t);
  int (*create_index)(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_index *ix);

  /* DML (rowid based) */
  int (*insert)(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                const uint8_t *rec, int reclen);
  int (*update)(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                const uint8_t *rec, int reclen);
  int (*remove)(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid);

  /* Allocate the next rowid for a table. */
  int (*next_rowid)(tdb_storage *s, tdb_table *t, tdb_rowid *out);

  /* Scans — a volcano-style source. `use_idx` may be NULL (full table scan);
  ** when non-NULL, `range` (may be NULL) bounds the index's leading column.
  ** scan_next yields MVCC-visible rows only. */
  /* `colmask` (may be NULL = all) is a per-column needed-flag for projection
  ** pushdown; the columnar engine reads only the marked columns. */
  int  (*scan_open)(tdb_storage *s, tdb_txn *txn, tdb_table *t,
                    tdb_index *use_idx, const tdb_keyrange *range,
                    tdb_txnid as_of, const uint8_t *colmask, tdb_scan **out);
  int  (*scan_next)(tdb_scan *sc, tdb_rowid *rowid, const uint8_t **rec, int *reclen);
  void (*scan_close)(tdb_scan *sc);

  /* Point lookup honoring MVCC. *found = 0 if no visible version exists. */
  int (*seek_rowid)(tdb_storage *s, tdb_txn *txn, tdb_table *t, tdb_rowid rowid,
                    const uint8_t **rec, int *reclen, int *found);
} tdb_storage_vtab;

struct tdb_storage {
  const tdb_storage_vtab *vtab;
  void                   *impl;
};

/* Row-oriented engine over the b-tree. */
int  tdb_engine_row_open(tdb_pager *p, tdb_storage **out);
/* Columnar (DSM) engine: meta b-tree + one value b-tree per column. */
int  tdb_engine_columnar_open(tdb_pager *p, tdb_storage **out);
/* Dispatcher: routes each table to the row or columnar engine by t->columnar.
** This is what a database connection uses. */
int  tdb_engine_open(tdb_pager *p, tdb_storage **out);

/* Thin dispatch helpers. */
static inline void tdb_storage_close(tdb_storage *s) { if (s) s->vtab->close(s); }

#endif /* TDB_STORAGE_H */
