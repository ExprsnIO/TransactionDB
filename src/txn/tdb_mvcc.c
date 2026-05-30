/* tdb_mvcc.c — snapshot visibility rules. */
#include "tdb_mvcc.h"

int tdb_snapshot_in_active(const tdb_snapshot *s, tdb_txnid xid) {
  for (int i = 0; i < s->nactive; i++)
    if (s->active[i] == xid) return 1;
  return 0;
}

/* A transaction id is "visible" to the snapshot when its effects are part of
** the consistent state the snapshot observes: it committed, started before the
** snapshot, and was not itself in-flight at snapshot time. */
static int xid_visible(tdb_xstate_fn fn, void *ctx, const tdb_snapshot *s,
                       tdb_txnid xid) {
  if (xid == s->self) return 1;                 /* own changes */
  if (fn(ctx, xid) != TDB_XACT_COMMITTED) return 0;
  if (xid >= s->xmax) return 0;                 /* started after snapshot */
  if (tdb_snapshot_in_active(s, xid)) return 0; /* in-flight at snapshot */
  return 1;
}

int tdb_mvcc_visible(tdb_xstate_fn fn, void *ctx, const tdb_snapshot *s,
                     tdb_txnid xmin, tdb_txnid xmax) {
  /* The creating transaction must be visible. */
  if (!xid_visible(fn, ctx, s, xmin)) return 0;

  /* If undeleted (or the deleter is not visible to us), the row is visible. */
  if (xmax == 0) return 1;
  if (xmax == s->self) return 0;                /* deleted by ourselves */
  if (!xid_visible(fn, ctx, s, xmax)) return 1; /* deleter not yet visible */
  return 0;                                     /* deleter committed & visible */
}
