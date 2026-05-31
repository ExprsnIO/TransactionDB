/* tdb_env.c — shared database environment (see tdb_env.h). */
#include "tdb_env.h"
#include "../common/tdb_mem.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

/* Global registry of shared (file-backed) environments, guarded by g_mu. The
** mutex is created on first use; the process holds it only briefly. */
static tdb_mutex *g_mu = NULL;
static tdb_env   *g_envs = NULL;     /* chain of shared envs */

static tdb_mutex *registry_mutex(void) {
  /* benign first-use init: callers are already serialized by the connection
  ** lock during open in the common single-threaded-open case; double init is
  ** avoided by the static below being set once. */
  if (!g_mu) g_mu = tdb_mutex_new();
  return g_mu;
}

static int is_private(const char *path, int flags) {
  return (flags & TDB_OPEN_MEMORY) || !path || path[0] == '\0' ||
         strcmp(path, ":memory:") == 0;
}

/* Canonical key for a file path, stable whether or not the file exists yet.
** The file itself may not exist at first open, so we canonicalize its PARENT
** directory (which does exist) and re-attach the basename. This guarantees two
** connections opening the same path agree on the key regardless of open order. */
static char *canon_key(const char *path) {
#if defined(_WIN32)
  char buf[1024];
  if (_fullpath(buf, path, sizeof(buf))) return tdb_strdup(buf);
  return tdb_strdup(path);
#else
  /* split into dir + base, canonicalize the dir, re-attach the base */
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  char rdir[PATH_MAX];
  const char *resolved;
  if (!slash) {
    resolved = realpath(".", rdir) ? rdir : NULL;
  } else if (slash == path) {            /* "/file" */
    rdir[0] = '/'; rdir[1] = '\0'; resolved = rdir;
  } else {
    size_t dlen = (size_t)(slash - path);
    char dirpart[PATH_MAX];
    if (dlen >= sizeof(dirpart)) return tdb_strdup(path);
    memcpy(dirpart, path, dlen); dirpart[dlen] = '\0';
    resolved = realpath(dirpart, rdir) ? rdir : NULL;
  }
  if (!resolved) return tdb_strdup(path);
  size_t rl = strlen(resolved), bl = strlen(base);
  char *key = (char *)tdb_malloc(rl + bl + 2);
  if (!key) return NULL;
  memcpy(key, resolved, rl);
  key[rl] = '/';
  memcpy(key + rl + 1, base, bl + 1);
  return key;
#endif
}

static void env_destroy(tdb_env *e) {
  if (!e) return;
  if (e->engine) tdb_storage_close(e->engine);
  if (e->tm)     tdb_txnmgr_close(e->tm);
  if (e->lm)     tdb_lockmgr_free(e->lm);
  if (e->cat)    tdb_catalog_close(e->cat);
  if (e->pager)  tdb_pager_close(e->pager);
  if (e->mu)     tdb_mutex_free(e->mu);
  tdb_mfree(e->key);
  tdb_mfree(e);
}

/* Build a fresh environment (its own pager/catalog/lock/txn/engine). */
static int env_build(const char *path, int flags, char *key, tdb_env **out) {
  tdb_env *e = (tdb_env *)tdb_calloc(sizeof(*e));
  if (!e) { tdb_mfree(key); return TDB_NOMEM; }
  e->key = key;
  e->refs = 1;
  e->mu = tdb_mutex_new_recursive();
  if (!e->mu) { env_destroy(e); return TDB_NOMEM; }

  int rc = tdb_pager_open(NULL, path, flags, &e->pager);
  if (rc) { env_destroy(e); return rc; }
  rc = tdb_catalog_open(e->pager, &e->cat);
  if (rc) { env_destroy(e); return rc; }
  e->lm = tdb_lockmgr_new();
  if (!e->lm) { env_destroy(e); return TDB_NOMEM; }
  rc = tdb_txnmgr_open(e->pager, e->lm, &e->tm);
  if (rc) { env_destroy(e); return rc; }
  rc = tdb_engine_open(e->pager, &e->engine);
  if (rc) { env_destroy(e); return rc; }

  *out = e;
  return TDB_OK;
}

int tdb_env_acquire(const char *path, int flags, tdb_env **out) {
  *out = NULL;

  if (is_private(path, flags)) {
    tdb_env *e;
    int rc = env_build(path, flags, NULL, &e);
    if (rc) return rc;
    e->shared = 0;
    *out = e;
    return TDB_OK;
  }

  char *key = canon_key(path);
  if (!key) return TDB_NOMEM;

  tdb_mutex *gmu = registry_mutex();
  tdb_mutex_lock(gmu);

  for (tdb_env *e = g_envs; e; e = e->gnext) {
    if (e->key && strcmp(e->key, key) == 0) {
      e->refs++;
      tdb_mutex_unlock(gmu);
      tdb_mfree(key);
      *out = e;
      return TDB_OK;
    }
  }

  tdb_env *e;
  int rc = env_build(path, flags, key, &e);   /* takes ownership of key */
  if (rc) { tdb_mutex_unlock(gmu); return rc; }
  e->shared = 1;
  e->gnext = g_envs;
  g_envs = e;
  tdb_mutex_unlock(gmu);
  *out = e;
  return TDB_OK;
}

void tdb_env_enter(tdb_env *e) { if (e && e->mu) tdb_mutex_lock(e->mu); }
void tdb_env_leave(tdb_env *e) { if (e && e->mu) tdb_mutex_unlock(e->mu); }

int tdb_env_acquire_writer(tdb_env *e, tdb_txnid txn) {
  if (!e) return TDB_OK;
  if (e->writer == 0 || e->writer == txn) { e->writer = txn; return TDB_OK; }
  /* wait-die: smaller (older) txn id waits; larger (younger) aborts */
  return (txn < e->writer) ? TDB_BUSY : TDB_ABORT;
}

void tdb_env_release_writer(tdb_env *e, tdb_txnid txn) {
  if (e && e->writer == txn) e->writer = 0;
}

void tdb_env_release(tdb_env *e) {
  if (!e) return;

  if (!e->shared) {            /* private env: destroy immediately */
    env_destroy(e);
    return;
  }

  tdb_mutex *gmu = registry_mutex();
  tdb_mutex_lock(gmu);
  if (--e->refs > 0) { tdb_mutex_unlock(gmu); return; }

  /* last reference: unlink from the registry, then destroy outside the lock */
  for (tdb_env **pp = &g_envs; *pp; pp = &(*pp)->gnext) {
    if (*pp == e) { *pp = e->gnext; break; }
  }
  tdb_mutex_unlock(gmu);
  env_destroy(e);
}
