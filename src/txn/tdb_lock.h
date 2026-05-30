/*
** tdb_lock.h — lock manager with table- and row-level granularity.
**
** Locks are SHARED or EXCLUSIVE over a resource identified by (type, id, sub).
** Conflicts are resolved by the deadlock-free WAIT-DIE rule based on
** transaction id (age): an OLDER requester waits (TDB_BUSY, caller may retry),
** a YOUNGER requester dies (TDB_ABORT). Many readers may hold a resource
** SHARED; a single writer holds it EXCLUSIVE.
**
** The manager is in-memory and mutex-guarded; acquisition is non-blocking and
** returns the wait-die decision so callers control retry/abort policy.
*/
#ifndef TDB_LOCK_H
#define TDB_LOCK_H

#include "../common/tdb_internal.h"

typedef enum tdb_lockmode {
  TDB_LOCK_NONE = 0,
  TDB_LOCK_SHARED,
  TDB_LOCK_EXCL
} tdb_lockmode;

typedef enum tdb_restype {
  TDB_RES_TABLE = 0,
  TDB_RES_ROW
} tdb_restype;

typedef struct tdb_lockmgr tdb_lockmgr;

tdb_lockmgr *tdb_lockmgr_new(void);
void         tdb_lockmgr_free(tdb_lockmgr *m);

/* Acquire a lock for transaction `txn`. Returns:
**   TDB_OK     granted (or already held / upgraded)
**   TDB_BUSY   conflict, requester is older — should wait and retry
**   TDB_ABORT  conflict, requester is younger — should abort (wait-die) */
int  tdb_lock_acquire(tdb_lockmgr *m, tdb_txnid txn, tdb_restype type,
                      uint64_t res_id, uint64_t sub_id, tdb_lockmode mode);

void tdb_lock_release_all(tdb_lockmgr *m, tdb_txnid txn);

/* Introspection (used by tests). */
int  tdb_lock_holder_count(tdb_lockmgr *m, tdb_restype type, uint64_t res_id,
                           uint64_t sub_id);

#endif /* TDB_LOCK_H */
