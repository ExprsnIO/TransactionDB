/* tdb_lock.c — in-memory lock table with wait-die conflict resolution. */
#include "tdb_lock.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_mutex.h"
#include "../common/tdb_util.h"

#include <string.h>

#define LOCK_BUCKETS 256

typedef struct holder {
  tdb_txnid     txn;
  tdb_lockmode  mode;
  struct holder *next;
} holder;

typedef struct resource {
  tdb_restype type;
  uint64_t    id;
  uint64_t    sub;
  holder     *holders;
  struct resource *hnext;
} resource;

struct tdb_lockmgr {
  tdb_mutex *mu;
  resource  *buckets[LOCK_BUCKETS];
};

tdb_lockmgr *tdb_lockmgr_new(void) {
  tdb_lockmgr *m = (tdb_lockmgr *)tdb_calloc(sizeof(*m));
  if (!m) return NULL;
  m->mu = tdb_mutex_new();
  return m;
}

static void free_resource(resource *r) {
  holder *h = r->holders;
  while (h) { holder *n = h->next; tdb_mfree(h); h = n; }
  tdb_mfree(r);
}

void tdb_lockmgr_free(tdb_lockmgr *m) {
  if (!m) return;
  for (int b = 0; b < LOCK_BUCKETS; b++) {
    resource *r = m->buckets[b];
    while (r) { resource *n = r->hnext; free_resource(r); r = n; }
  }
  tdb_mutex_free(m->mu);
  tdb_mfree(m);
}

static unsigned res_hash(tdb_restype type, uint64_t id, uint64_t sub) {
  uint64_t k[3] = { (uint64_t)type, id, sub };
  return (unsigned)(tdb_fnv1a(k, sizeof(k)) % LOCK_BUCKETS);
}

static resource *res_find(tdb_lockmgr *m, tdb_restype type, uint64_t id,
                          uint64_t sub, int create) {
  unsigned b = res_hash(type, id, sub);
  for (resource *r = m->buckets[b]; r; r = r->hnext)
    if (r->type == type && r->id == id && r->sub == sub) return r;
  if (!create) return NULL;
  resource *r = (resource *)tdb_calloc(sizeof(*r));
  if (!r) return NULL;
  r->type = type; r->id = id; r->sub = sub;
  r->hnext = m->buckets[b];
  m->buckets[b] = r;
  return r;
}

static int mode_conflict(tdb_lockmode a, tdb_lockmode b) {
  return a == TDB_LOCK_EXCL || b == TDB_LOCK_EXCL;
}

int tdb_lock_acquire(tdb_lockmgr *m, tdb_txnid txn, tdb_restype type,
                     uint64_t res_id, uint64_t sub_id, tdb_lockmode mode) {
  tdb_mutex_lock(m->mu);
  resource *r = res_find(m, type, res_id, sub_id, 1);
  if (!r) { tdb_mutex_unlock(m->mu); return TDB_NOMEM; }

  /* Already a holder? Possibly upgrade. */
  holder *self = NULL;
  int others = 0;
  for (holder *h = r->holders; h; h = h->next) {
    if (h->txn == txn) self = h;
    else others++;
  }
  if (self) {
    if (mode == TDB_LOCK_EXCL && self->mode != TDB_LOCK_EXCL) {
      if (others > 0) {
        /* upgrade conflicts with other holders: wait-die against them */
        for (holder *h = r->holders; h; h = h->next) {
          if (h->txn != txn) {
            int rc = (txn < h->txn) ? TDB_BUSY : TDB_ABORT;
            tdb_mutex_unlock(m->mu);
            return rc;
          }
        }
      }
      self->mode = TDB_LOCK_EXCL;
    }
    tdb_mutex_unlock(m->mu);
    return TDB_OK;
  }

  /* New request: check conflicts with existing holders. */
  for (holder *h = r->holders; h; h = h->next) {
    if (mode_conflict(mode, h->mode)) {
      int rc = (txn < h->txn) ? TDB_BUSY : TDB_ABORT; /* wait-die */
      tdb_mutex_unlock(m->mu);
      return rc;
    }
  }

  holder *nh = (holder *)tdb_malloc(sizeof(*nh));
  if (!nh) { tdb_mutex_unlock(m->mu); return TDB_NOMEM; }
  nh->txn = txn; nh->mode = mode; nh->next = r->holders;
  r->holders = nh;
  tdb_mutex_unlock(m->mu);
  return TDB_OK;
}

void tdb_lock_release_all(tdb_lockmgr *m, tdb_txnid txn) {
  tdb_mutex_lock(m->mu);
  for (int b = 0; b < LOCK_BUCKETS; b++) {
    for (resource *r = m->buckets[b]; r; r = r->hnext) {
      holder **pp = &r->holders;
      while (*pp) {
        if ((*pp)->txn == txn) { holder *d = *pp; *pp = d->next; tdb_mfree(d); }
        else pp = &(*pp)->next;
      }
    }
  }
  tdb_mutex_unlock(m->mu);
}

int tdb_lock_holder_count(tdb_lockmgr *m, tdb_restype type, uint64_t res_id,
                          uint64_t sub_id) {
  tdb_mutex_lock(m->mu);
  resource *r = res_find(m, type, res_id, sub_id, 0);
  int n = 0;
  if (r) for (holder *h = r->holders; h; h = h->next) n++;
  tdb_mutex_unlock(m->mu);
  return n;
}
