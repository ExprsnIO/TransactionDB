/*
** tdb_mvcc.h — multi-version concurrency control: snapshots and visibility.
**
** Every stored row version carries a creator transaction id (xmin) and a
** deleter/superseder transaction id (xmax, 0 = live). A reader holds a
** snapshot and consults tdb_mvcc_visible() to decide whether a given version
** is visible to it. Commit status of an arbitrary xid is provided by a
** caller-supplied callback so this module has no dependency on the
** transaction manager (and is trivially unit-testable).
*/
#ifndef TDB_MVCC_H
#define TDB_MVCC_H

#include "../common/tdb_internal.h"

typedef enum tdb_xstate {
  TDB_XACT_ACTIVE = 0,
  TDB_XACT_COMMITTED,
  TDB_XACT_ABORTED
} tdb_xstate;

/* Returns the commit state of `xid`. */
typedef tdb_xstate (*tdb_xstate_fn)(void *ctx, tdb_txnid xid);

typedef struct tdb_snapshot {
  tdb_txnid  self;     /* the owning transaction's id (own writes visible) */
  tdb_txnid  xmax;     /* one past the highest xid assigned at snapshot time */
  tdb_txnid *active;   /* xids in-flight when the snapshot was taken */
  int        nactive;
} tdb_snapshot;

int tdb_snapshot_in_active(const tdb_snapshot *s, tdb_txnid xid);

/* True iff the version (xmin, xmax) is visible to snapshot `s`. */
int tdb_mvcc_visible(tdb_xstate_fn fn, void *ctx, const tdb_snapshot *s,
                     tdb_txnid xmin, tdb_txnid xmax);

#endif /* TDB_MVCC_H */
