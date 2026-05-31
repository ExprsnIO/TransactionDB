/*
** tdb_mutex.h — portable mutex / reader-writer lock abstraction.
**
** Centralizes all threading primitives so the rest of the engine stays
** portable. Backed by pthreads on POSIX and SRWLOCK/CRITICAL_SECTION on
** Windows.
*/
#ifndef TDB_MUTEX_H
#define TDB_MUTEX_H

#include "tdb_internal.h"

typedef struct tdb_mutex  tdb_mutex;
typedef struct tdb_rwlock tdb_rwlock;

tdb_mutex *tdb_mutex_new(void);
/* Like tdb_mutex_new, but the owning thread may re-lock it (used for the
** re-entrant connection-level lock that makes a tdb_db thread-safe). */
tdb_mutex *tdb_mutex_new_recursive(void);
void       tdb_mutex_free(tdb_mutex *m);
void       tdb_mutex_lock(tdb_mutex *m);
int        tdb_mutex_trylock(tdb_mutex *m); /* 1 = acquired, 0 = busy */
void       tdb_mutex_unlock(tdb_mutex *m);

tdb_rwlock *tdb_rwlock_new(void);
void        tdb_rwlock_free(tdb_rwlock *rw);
void        tdb_rwlock_rdlock(tdb_rwlock *rw);
void        tdb_rwlock_wrlock(tdb_rwlock *rw);
void        tdb_rwlock_unlock(tdb_rwlock *rw);

#endif /* TDB_MUTEX_H */
