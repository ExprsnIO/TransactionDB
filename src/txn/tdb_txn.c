/* tdb_txn.c — transaction manager, snapshots, commit registry. */
#include "tdb_txn.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_mutex.h"

#include <string.h>

typedef struct { tdb_txnid id; tdb_xstate state; } xrec;

struct tdb_txnmgr {
  tdb_pager   *pager;
  tdb_lockmgr *locks;
  tdb_mutex   *mu;
  tdb_txnid    base;     /* xids <= base are committed (from prior sessions) */
  tdb_txnid    next;     /* next xid to assign */
  xrec        *recs;     /* this-session transactions */
  int          nrec, caprec;
  tdb_txnid   *active;   /* currently active xids */
  int          nactive, capact;
};

int tdb_txnmgr_open(tdb_pager *p, tdb_lockmgr *lm, tdb_txnmgr **out) {
  tdb_txnmgr *m = (tdb_txnmgr *)tdb_calloc(sizeof(*m));
  if (!m) return TDB_NOMEM;
  m->pager = p;
  m->locks = lm;
  m->mu = tdb_mutex_new();
  m->base = tdb_pager_max_txnid(p);
  m->next = m->base + 1;
  *out = m;
  return TDB_OK;
}

void tdb_txnmgr_close(tdb_txnmgr *m) {
  if (!m) return;
  tdb_mutex_free(m->mu);
  tdb_mfree(m->recs);
  tdb_mfree(m->active);
  tdb_mfree(m);
}

tdb_lockmgr *tdb_txnmgr_locks(tdb_txnmgr *m) { return m->locks; }

static void set_state(tdb_txnmgr *m, tdb_txnid id, tdb_xstate st) {
  for (int i = 0; i < m->nrec; i++) {
    if (m->recs[i].id == id) { m->recs[i].state = st; return; }
  }
  if (m->nrec == m->caprec) {
    int cap = m->caprec ? m->caprec * 2 : 16;
    m->recs = (xrec *)tdb_realloc(m->recs, sizeof(xrec) * (size_t)cap);
    m->caprec = cap;
  }
  m->recs[m->nrec].id = id;
  m->recs[m->nrec].state = st;
  m->nrec++;
}

/* tdb_xstate_fn-compatible: ctx is the txnmgr. */
tdb_xstate tdb_txnmgr_state(void *ctx, tdb_txnid xid) {
  tdb_txnmgr *m = (tdb_txnmgr *)ctx;
  if (xid == 0) return TDB_XACT_ABORTED;
  if (xid <= m->base) return TDB_XACT_COMMITTED;
  for (int i = 0; i < m->nrec; i++)
    if (m->recs[i].id == xid) return m->recs[i].state;
  return TDB_XACT_ABORTED; /* unknown id: treat as not-committed */
}

static void active_add(tdb_txnmgr *m, tdb_txnid id) {
  if (m->nactive == m->capact) {
    int cap = m->capact ? m->capact * 2 : 16;
    m->active = (tdb_txnid *)tdb_realloc(m->active, sizeof(tdb_txnid) * (size_t)cap);
    m->capact = cap;
  }
  m->active[m->nactive++] = id;
}

static void active_remove(tdb_txnmgr *m, tdb_txnid id) {
  for (int i = 0; i < m->nactive; i++) {
    if (m->active[i] == id) {
      m->active[i] = m->active[--m->nactive];
      return;
    }
  }
}

static void take_snapshot(tdb_txnmgr *m, tdb_txn *t) {
  tdb_mfree(t->snap.active);
  t->snap.self = t->id;
  t->snap.xmax = m->next;
  t->snap.nactive = m->nactive;
  t->snap.active = NULL;
  if (m->nactive > 0) {
    t->snap.active = (tdb_txnid *)tdb_malloc(sizeof(tdb_txnid) * (size_t)m->nactive);
    memcpy(t->snap.active, m->active, sizeof(tdb_txnid) * (size_t)m->nactive);
  }
}

int tdb_txn_begin(tdb_txnmgr *m, tdb_isolation iso, int writable, tdb_txn **out) {
  tdb_txn *t = (tdb_txn *)tdb_calloc(sizeof(*t));
  if (!t) return TDB_NOMEM;
  tdb_mutex_lock(m->mu);
  t->mgr = m;
  t->id = m->next++;
  t->iso = iso;
  t->state = TDB_XACT_ACTIVE;
  t->writable = writable;
  set_state(m, t->id, TDB_XACT_ACTIVE);
  active_add(m, t->id);
  take_snapshot(m, t);
  tdb_mutex_unlock(m->mu);

  if (writable) {
    int rc = tdb_pager_begin(m->pager);
    if (rc) { tdb_txn_rollback(t); return rc; }
  }
  *out = t;
  return TDB_OK;
}

void tdb_txn_refresh_snapshot(tdb_txn *t) {
  tdb_mutex_lock(t->mgr->mu);
  take_snapshot(t->mgr, t);
  tdb_mutex_unlock(t->mgr->mu);
}

static void sp_stack_free(tdb_txn *t) {
  for (int i = 0; i < t->nsp; i++) tdb_mfree(t->sp_names[i]);
  tdb_mfree(t->sp_names); tdb_mfree(t->sp_levels);
  t->sp_names = NULL; t->sp_levels = NULL; t->nsp = t->capsp = 0;
}

static int find_sp(tdb_txn *t, const char *name) {
  for (int i = t->nsp - 1; i >= 0; i--)
    if (strcasecmp(t->sp_names[i], name) == 0) return i;
  return -1;
}

int tdb_txn_savepoint(tdb_txn *t, const char *name) {
  if (!t->writable) return TDB_READONLY;
  int level = tdb_pager_savepoint(t->mgr->pager);
  if (level < 0) return TDB_ERROR;
  if (t->nsp == t->capsp) {
    int cap = t->capsp ? t->capsp * 2 : 4;
    t->sp_names = (char **)tdb_realloc(t->sp_names, sizeof(char *) * (size_t)cap);
    t->sp_levels = (int *)tdb_realloc(t->sp_levels, sizeof(int) * (size_t)cap);
    t->capsp = cap;
  }
  t->sp_names[t->nsp] = tdb_strdup(name);
  t->sp_levels[t->nsp] = level;
  t->nsp++;
  return TDB_OK;
}

int tdb_txn_release(tdb_txn *t, const char *name) {
  int i = find_sp(t, name);
  if (i < 0) return TDB_NOTFOUND;
  int rc = tdb_pager_savepoint_release(t->mgr->pager, t->sp_levels[i]);
  for (int j = i; j < t->nsp; j++) tdb_mfree(t->sp_names[j]);
  t->nsp = i;
  return rc;
}

int tdb_txn_rollback_to(tdb_txn *t, const char *name) {
  int i = find_sp(t, name);
  if (i < 0) return TDB_NOTFOUND;
  int rc = tdb_pager_savepoint_rollback(t->mgr->pager, t->sp_levels[i]);
  for (int j = i + 1; j < t->nsp; j++) tdb_mfree(t->sp_names[j]);
  t->nsp = i + 1;
  return rc;
}

int tdb_txn_commit(tdb_txn *t) {
  tdb_txnmgr *m = t->mgr;
  int rc = TDB_OK;
  if (t->writable) {
    tdb_pager_set_max_txnid(m->pager, t->id);
    rc = tdb_pager_commit(m->pager);
  }
  tdb_mutex_lock(m->mu);
  set_state(m, t->id, rc ? TDB_XACT_ABORTED : TDB_XACT_COMMITTED);
  if (!rc && t->id > m->base) {
    /* nothing: base advances lazily; state map handles it */
  }
  active_remove(m, t->id);
  tdb_mutex_unlock(m->mu);
  if (m->locks) tdb_lock_release_all(m->locks, t->id);

  t->state = rc ? TDB_XACT_ABORTED : TDB_XACT_COMMITTED;
  sp_stack_free(t);
  tdb_mfree(t->snap.active);
  tdb_mfree(t);
  return rc;
}

int tdb_txn_rollback(tdb_txn *t) {
  tdb_txnmgr *m = t->mgr;
  int rc = TDB_OK;
  if (t->writable) rc = tdb_pager_rollback(m->pager);
  tdb_mutex_lock(m->mu);
  set_state(m, t->id, TDB_XACT_ABORTED);
  active_remove(m, t->id);
  tdb_mutex_unlock(m->mu);
  if (m->locks) tdb_lock_release_all(m->locks, t->id);

  t->state = TDB_XACT_ABORTED;
  sp_stack_free(t);
  tdb_mfree(t->snap.active);
  tdb_mfree(t);
  return rc;
}

int tdb_txn_visible(tdb_txn *t, tdb_txnid xmin, tdb_txnid xmax) {
  return tdb_mvcc_visible(tdb_txnmgr_state, t->mgr, &t->snap, xmin, xmax);
}
