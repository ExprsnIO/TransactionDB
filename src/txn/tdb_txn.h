/*
** tdb_txn.h — transaction manager and transaction objects.
**
** The manager hands out monotonically increasing transaction ids, tracks the
** commit status of in-session transactions, and constructs snapshots. The
** largest committed id is persisted in the database header so ids never repeat
** across sessions (and any xid from a prior session is treated as committed,
** since uncommitted work was discarded during WAL recovery).
**
** The engine uses a single-writer model: at most one read-write transaction
** mutates the pager at a time, while any number of snapshots read concurrently
** via MVCC.
*/
#ifndef TDB_TXN_H
#define TDB_TXN_H

#include "tdb_mvcc.h"
#include "../storage/tdb_pager.h"
#include "tdb_lock.h"

typedef enum tdb_isolation {
  TDB_ISO_READ_COMMITTED = 0,
  TDB_ISO_SNAPSHOT,          /* repeatable read / snapshot (default) */
  TDB_ISO_SERIALIZABLE
} tdb_isolation;

typedef struct tdb_txnmgr tdb_txnmgr;

typedef struct tdb_txn {
  tdb_txnmgr   *mgr;
  tdb_txnid     id;
  tdb_isolation iso;
  tdb_snapshot  snap;
  int           state;     /* tdb_xstate */
  int           writable;
  /* named savepoint stack (maps name -> pager savepoint level) */
  char        **sp_names;
  int          *sp_levels;
  int           nsp, capsp;
} tdb_txn;

int  tdb_txnmgr_open(tdb_pager *p, tdb_lockmgr *lm, tdb_txnmgr **out);
void tdb_txnmgr_close(tdb_txnmgr *m);
tdb_xstate tdb_txnmgr_state(void *ctx, tdb_txnid xid); /* tdb_xstate_fn-compatible */
tdb_lockmgr *tdb_txnmgr_locks(tdb_txnmgr *m);

int  tdb_txn_begin(tdb_txnmgr *m, tdb_isolation iso, int writable, tdb_txn **out);
int  tdb_txn_commit(tdb_txn *t);
int  tdb_txn_rollback(tdb_txn *t);

/* Re-take the statement snapshot (for READ COMMITTED, called per statement). */
void tdb_txn_refresh_snapshot(tdb_txn *t);

/* Test a row version's visibility under this transaction's snapshot. */
int  tdb_txn_visible(tdb_txn *t, tdb_txnid xmin, tdb_txnid xmax);

/* Named savepoints (write transactions). RELEASE discards a savepoint and any
** nested inside it; ROLLBACK TO reverts to it (and keeps it). */
int  tdb_txn_savepoint(tdb_txn *t, const char *name);
int  tdb_txn_release(tdb_txn *t, const char *name);
int  tdb_txn_rollback_to(tdb_txn *t, const char *name);

#endif /* TDB_TXN_H */
