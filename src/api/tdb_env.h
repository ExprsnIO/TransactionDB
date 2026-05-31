/*
** tdb_env.h — shared database environment (shared-cache).
**
** A tdb_env owns the physical resources for one database file: the pager (+
** WAL), the in-memory catalog, the lock manager, the transaction manager, and
** the storage engine. Multiple tdb_db connections opened on the same on-disk
** path share a single refcounted tdb_env, so they see one cache, one catalog,
** and one lock/transaction space — which is what makes cross-connection
** locking (wait-die) and SERIALIZABLE meaningful.
**
** File-backed databases are shared by canonical path. ":memory:" and unnamed
** temporary databases are always private (one env per connection), since their
** contents do not outlive the connection and cannot be shared by path.
**
** The engine is single-writer: at most one read-write transaction mutates the
** pager at a time, while any number of snapshot readers run concurrently.
** tdb_env serializes writers and guards the shared catalog with its own mutex.
*/
#ifndef TDB_ENV_H
#define TDB_ENV_H

#include "../storage/tdb_pager.h"
#include "../storage/tdb_storage.h"
#include "../catalog/tdb_catalog.h"
#include "../txn/tdb_txn.h"
#include "../txn/tdb_lock.h"
#include "../common/tdb_mutex.h"

typedef struct tdb_env {
  char        *key;        /* canonical path (NULL for private envs) */
  int          refs;       /* connections sharing this env */
  int          shared;     /* 1 = registered in the global table by key */

  tdb_pager   *pager;
  tdb_catalog *cat;
  tdb_lockmgr *lm;
  tdb_txnmgr  *tm;
  tdb_storage *engine;

  tdb_mutex   *mu;         /* serializes execution on this env (recursive) */
  tdb_txnid    writer;     /* txn id of the connection holding write capability
                           ** via an open explicit transaction (0 = none) */
  struct tdb_env *gnext;   /* global registry chain */
} tdb_env;

/* Enter/leave the env's execution lock. Held for the duration of one statement
** so concurrent connections on a shared env do not race the page cache or the
** in-memory catalog (the pager is single-writer; its cache is not thread-safe).
** Recursive, so the re-entrant API does not self-deadlock. */
void tdb_env_enter(tdb_env *e);
void tdb_env_leave(tdb_env *e);

/* Claim/release write capability for an explicit transaction. Cross-connection
** write-write conflicts are resolved by WAIT-DIE on transaction age:
**   TDB_OK     granted (or already held by `txn`)
**   TDB_BUSY   another (younger-or-equal-priority) writer holds it; `txn` is
**              OLDER and should wait/retry
**   TDB_ABORT  another writer holds it and `txn` is YOUNGER -> must abort
** Must be called while holding the env (tdb_env_enter). */
int  tdb_env_acquire_writer(tdb_env *e, tdb_txnid txn);
void tdb_env_release_writer(tdb_env *e, tdb_txnid txn);

/* Acquire (open or attach to) the environment for `path`/`flags`. File-backed
** paths are shared by canonical name; memory/temp databases get a private env.
** Returns a +1 reference; release with tdb_env_release. */
int  tdb_env_acquire(const char *path, int flags, tdb_env **out);

/* Drop a reference; the env (and its resources) is destroyed at zero. */
void tdb_env_release(tdb_env *e);

#endif /* TDB_ENV_H */
