/* tdb_mutex.c — pthread-backed mutex / rwlock (POSIX) with a Win32 fallback. */
#include "tdb_mutex.h"
#include "tdb_mem.h"

#if defined(_WIN32)
/* ----------------------------- Windows ------------------------------- */
#include <windows.h>

struct tdb_mutex { CRITICAL_SECTION cs; };
struct tdb_rwlock { SRWLOCK lock; int writing; };

tdb_mutex *tdb_mutex_new(void) {
  tdb_mutex *m = (tdb_mutex *)tdb_malloc(sizeof(*m));
  if (m) InitializeCriticalSection(&m->cs);
  return m;
}
void tdb_mutex_free(tdb_mutex *m) {
  if (!m) return;
  DeleteCriticalSection(&m->cs);
  tdb_mfree(m);
}
void tdb_mutex_lock(tdb_mutex *m) { EnterCriticalSection(&m->cs); }
int  tdb_mutex_trylock(tdb_mutex *m) { return TryEnterCriticalSection(&m->cs) ? 1 : 0; }
void tdb_mutex_unlock(tdb_mutex *m) { LeaveCriticalSection(&m->cs); }

tdb_rwlock *tdb_rwlock_new(void) {
  tdb_rwlock *rw = (tdb_rwlock *)tdb_malloc(sizeof(*rw));
  if (rw) { InitializeSRWLock(&rw->lock); rw->writing = 0; }
  return rw;
}
void tdb_rwlock_free(tdb_rwlock *rw) { tdb_mfree(rw); }
void tdb_rwlock_rdlock(tdb_rwlock *rw) { AcquireSRWLockShared(&rw->lock); }
void tdb_rwlock_wrlock(tdb_rwlock *rw) { AcquireSRWLockExclusive(&rw->lock); rw->writing = 1; }
void tdb_rwlock_unlock(tdb_rwlock *rw) {
  if (rw->writing) { rw->writing = 0; ReleaseSRWLockExclusive(&rw->lock); }
  else ReleaseSRWLockShared(&rw->lock);
}

#else
/* ------------------------------ POSIX -------------------------------- */
#include <pthread.h>

struct tdb_mutex { pthread_mutex_t m; };
struct tdb_rwlock { pthread_rwlock_t rw; };

tdb_mutex *tdb_mutex_new(void) {
  tdb_mutex *m = (tdb_mutex *)tdb_malloc(sizeof(*m));
  if (m) pthread_mutex_init(&m->m, NULL);
  return m;
}
void tdb_mutex_free(tdb_mutex *m) {
  if (!m) return;
  pthread_mutex_destroy(&m->m);
  tdb_mfree(m);
}
void tdb_mutex_lock(tdb_mutex *m) { pthread_mutex_lock(&m->m); }
int  tdb_mutex_trylock(tdb_mutex *m) { return pthread_mutex_trylock(&m->m) == 0 ? 1 : 0; }
void tdb_mutex_unlock(tdb_mutex *m) { pthread_mutex_unlock(&m->m); }

tdb_rwlock *tdb_rwlock_new(void) {
  tdb_rwlock *rw = (tdb_rwlock *)tdb_malloc(sizeof(*rw));
  if (rw) pthread_rwlock_init(&rw->rw, NULL);
  return rw;
}
void tdb_rwlock_free(tdb_rwlock *rw) {
  if (!rw) return;
  pthread_rwlock_destroy(&rw->rw);
  tdb_mfree(rw);
}
void tdb_rwlock_rdlock(tdb_rwlock *rw) { pthread_rwlock_rdlock(&rw->rw); }
void tdb_rwlock_wrlock(tdb_rwlock *rw) { pthread_rwlock_wrlock(&rw->rw); }
void tdb_rwlock_unlock(tdb_rwlock *rw) { pthread_rwlock_unlock(&rw->rw); }

#endif
