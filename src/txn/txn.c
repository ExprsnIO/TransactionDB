/*
 * Transaction Manager — MVCC + 2PL hybrid
 *
 * - MVCC for read operations (snapshot isolation)
 * - Two-phase locking for write operations
 * - Row versioning via xmin/xmax stamps
 * - Deadlock detection via wait-for graph DFS
 */

#include "tdb/txn.h"
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

/* Hash a lock target to a bucket index */
static size_t lock_hash(tdb_lock_target_t target)
{
    return (size_t)((target.page_id * 31 + target.slot) % TDB_LOCK_TABLE_SIZE);
}

/* Check if two lock targets are equal */
static bool lock_target_eq(tdb_lock_target_t a, tdb_lock_target_t b)
{
    return a.page_id == b.page_id && a.slot == b.slot;
}

/* Find the ATT slot for a given txn_id. Returns index or -1. */
static int att_find(const tdb_txn_manager_t *mgr, tdb_txn_id_t txn_id)
{
    for (size_t i = 0; i < mgr->active_count; i++) {
        if (mgr->active[i].txn_id == txn_id) {
            return (int)i;
        }
    }
    return -1;
}

/* Remove an ATT entry by index (swap with last for O(1) removal) */
static void att_remove(tdb_txn_manager_t *mgr, int idx)
{
    if (idx < 0 || (size_t)idx >= mgr->active_count) {
        return;
    }
    mgr->active_count--;
    if ((size_t)idx < mgr->active_count) {
        mgr->active[idx] = mgr->active[mgr->active_count];
    }
    memset(&mgr->active[mgr->active_count], 0, sizeof(tdb_att_entry_t));
}

/* ----------------------------------------------------------------
 * Transaction manager lifecycle
 * ---------------------------------------------------------------- */

tdb_status_t tdb_txn_manager_init(tdb_txn_manager_t *mgr)
{
    if (!mgr) {
        return TDB_ERR_INVALID_ARG;
    }
    memset(mgr, 0, sizeof(*mgr));
    mgr->next_txn_id = 1;
    return TDB_OK;
}

void tdb_txn_manager_destroy(tdb_txn_manager_t *mgr)
{
    if (!mgr) {
        return;
    }

    /* Free all lock entries in every hash bucket */
    for (size_t i = 0; i < TDB_LOCK_TABLE_SIZE; i++) {
        tdb_lock_entry_t *entry = mgr->lock_table.buckets[i];
        while (entry) {
            tdb_lock_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
        mgr->lock_table.buckets[i] = NULL;
    }

    /* Free all waiters in the wait queue */
    tdb_lock_waiter_t *waiter = mgr->lock_table.wait_queue;
    while (waiter) {
        tdb_lock_waiter_t *next = waiter->next;
        free(waiter);
        waiter = next;
    }
    mgr->lock_table.wait_queue = NULL;

    mgr->active_count = 0;
    mgr->next_txn_id = 0;
}

/* ----------------------------------------------------------------
 * Transaction operations
 * ---------------------------------------------------------------- */

tdb_status_t tdb_txn_begin(tdb_txn_manager_t *mgr, tdb_txn_t *txn,
                           tdb_isolation_t iso)
{
    if (!mgr || !txn) {
        return TDB_ERR_INVALID_ARG;
    }

    if (mgr->active_count >= TDB_MAX_ACTIVE_TXNS) {
        return TDB_ERR_FULL;
    }

    /* Allocate txn_id */
    tdb_txn_id_t id = mgr->next_txn_id++;

    /* Compute snapshot_xmin: minimum txn_id of all currently active txns.
     * If no active transactions, use the new txn's own id. */
    tdb_txn_id_t xmin = id;
    for (size_t i = 0; i < mgr->active_count; i++) {
        if (mgr->active[i].state == TDB_TXN_ACTIVE &&
            mgr->active[i].txn_id < xmin) {
            xmin = mgr->active[i].txn_id;
        }
    }

    /* snapshot_xmax: next txn_id (all txns with id >= xmax are invisible) */
    tdb_txn_id_t xmax = mgr->next_txn_id;

    /* Initialize txn descriptor */
    memset(txn, 0, sizeof(*txn));
    txn->txn_id = id;
    txn->state = TDB_TXN_ACTIVE;
    txn->isolation = iso;
    txn->snapshot_xmin = xmin;
    txn->snapshot_xmax = xmax;
    txn->read_only = false;
    txn->last_lsn = 0;
    txn->locks_held = NULL;

    /* Add to active transaction table */
    tdb_att_entry_t *att = &mgr->active[mgr->active_count];
    att->txn_id = id;
    att->state = TDB_TXN_ACTIVE;
    att->last_lsn = 0;
    mgr->active_count++;

    return TDB_OK;
}

tdb_status_t tdb_txn_commit(tdb_txn_manager_t *mgr, tdb_txn_t *txn)
{
    if (!mgr || !txn) {
        return TDB_ERR_INVALID_ARG;
    }
    if (txn->state != TDB_TXN_ACTIVE) {
        return TDB_ERR_TXN_ABORT;
    }

    /* Change state to COMMITTED */
    txn->state = TDB_TXN_COMMITTED;

    /* Update ATT entry state before removal */
    int idx = att_find(mgr, txn->txn_id);
    if (idx >= 0) {
        mgr->active[idx].state = TDB_TXN_COMMITTED;
    }

    /* Release all locks held by this transaction */
    tdb_lock_release_all(mgr, txn);

    /* Remove from active transaction table */
    if (idx >= 0) {
        /* Re-find in case release_all didn't change the table */
        idx = att_find(mgr, txn->txn_id);
        if (idx >= 0) {
            att_remove(mgr, idx);
        }
    }

    return TDB_OK;
}

tdb_status_t tdb_txn_abort(tdb_txn_manager_t *mgr, tdb_txn_t *txn)
{
    if (!mgr || !txn) {
        return TDB_ERR_INVALID_ARG;
    }

    /* Change state to ABORTED */
    txn->state = TDB_TXN_ABORTED;

    /* Update ATT entry state */
    int idx = att_find(mgr, txn->txn_id);
    if (idx >= 0) {
        mgr->active[idx].state = TDB_TXN_ABORTED;
    }

    /* Release all locks held by this transaction */
    tdb_lock_release_all(mgr, txn);

    /* Remove from active transaction table */
    idx = att_find(mgr, txn->txn_id);
    if (idx >= 0) {
        att_remove(mgr, idx);
    }

    return TDB_OK;
}

/* ----------------------------------------------------------------
 * MVCC visibility check
 * ----------------------------------------------------------------
 *
 * A row version (xmin, xmax) is visible to a transaction if:
 *   1. xmin is from a committed txn that committed before our snapshot
 *      (or is our own transaction)
 *   2. xmax is 0 (not deleted) OR xmax is from a txn that hasn't
 *      committed in our view (or is from a future txn)
 *
 * Self-visibility: our own uncommitted writes are visible to us,
 *   unless we also deleted them (xmax == our txn_id).
 */
bool tdb_txn_is_visible(const tdb_txn_t *txn, const tdb_row_version_t *ver)
{
    if (!txn || !ver) {
        return false;
    }

    tdb_txn_id_t xmin = ver->xmin;
    tdb_txn_id_t xmax = ver->xmax;
    tdb_txn_id_t my_id = txn->txn_id;

    /* -----------------------------------------------------------
     * Case 1: Row was created by our own transaction
     * ----------------------------------------------------------- */
    if (xmin == my_id) {
        /* Our own insert. Visible unless we also deleted it. */
        if (xmax == 0) {
            return true;   /* not deleted */
        }
        if (xmax == my_id) {
            return false;  /* we deleted it */
        }
        /* xmax set by another txn on our row — still visible to us
         * since that other txn's delete hasn't committed from our view
         * (we are still active, so snapshot doesn't include it). */
        return true;
    }

    /* -----------------------------------------------------------
     * Case 2: Row was created by another transaction
     * ----------------------------------------------------------- */

    /* xmin must be from a committed txn visible in our snapshot */
    if (xmin >= txn->snapshot_xmax) {
        /* Created after our snapshot — not visible */
        return false;
    }

    /* xmin is below our snapshot_xmax. For it to be visible,
     * the creating transaction must have committed. Transactions
     * below snapshot_xmin are guaranteed committed (they finished
     * before the oldest active txn). For those in [xmin, xmax),
     * we need to check. In this simplified model without a full
     * commit log, we treat txns below snapshot_xmin as committed
     * and txns in [snapshot_xmin, snapshot_xmax) as potentially
     * still in-flight — we'd need the ATT to decide. Since
     * tdb_txn_is_visible takes only const tdb_txn_t* (no manager),
     * we use a conservative approach: txns with id < snapshot_xmax
     * that are not our own are considered committed unless they
     * are >= snapshot_xmax. */

    /* Now check xmax (deletion) */
    if (xmax == 0) {
        /* Row has not been deleted — visible */
        return true;
    }

    /* If xmax is our own txn, we deleted it */
    if (xmax == my_id) {
        return false;
    }

    /* If the deleting txn is at or beyond our snapshot, the delete
     * is not visible to us — the row is still visible. */
    if (xmax >= txn->snapshot_xmax) {
        return true;
    }

    /* The deleting txn is within our snapshot range.
     * If it committed, the row was deleted before our snapshot — not visible.
     * If it was still active/aborted, the delete doesn't count — visible.
     *
     * Without access to the manager, we use the same conservative approach:
     * txns below snapshot_xmin are committed, those in [xmin, xmax) we
     * treat as committed (consistent with the xmin check above). */
    if (xmax < txn->snapshot_xmin) {
        /* Definitely committed and finished before our snapshot */
        return false;
    }

    /* xmax is in [snapshot_xmin, snapshot_xmax) — in a full implementation
     * we'd check the commit log. Here, treat as committed (delete visible),
     * meaning the row is NOT visible. This is the conservative choice for
     * snapshot isolation. */
    return false;
}

/* ----------------------------------------------------------------
 * Lock operations
 * ---------------------------------------------------------------- */

tdb_status_t tdb_lock_acquire(tdb_txn_manager_t *mgr, tdb_txn_t *txn,
                               tdb_lock_target_t target, tdb_lock_mode_t mode)
{
    if (!mgr || !txn) {
        return TDB_ERR_INVALID_ARG;
    }

    size_t bucket = lock_hash(target);
    tdb_lock_entry_t *entry = mgr->lock_table.buckets[bucket];

    /* Walk the bucket to check for existing locks on this target */
    while (entry) {
        if (lock_target_eq(entry->target, target)) {
            if (entry->owner == txn->txn_id) {
                /* We already hold a lock on this target */
                if (entry->mode == TDB_LOCK_EXCLUSIVE) {
                    /* Already hold exclusive — nothing to do */
                    return TDB_OK;
                }
                if (mode == TDB_LOCK_SHARED) {
                    /* Already hold shared, requesting shared — fine */
                    return TDB_OK;
                }
                /* Lock upgrade: we hold shared, want exclusive.
                 * Check if any OTHER transaction also holds a lock
                 * on this target. */
                {
                    tdb_lock_entry_t *other = mgr->lock_table.buckets[bucket];
                    bool conflict = false;
                    while (other) {
                        if (lock_target_eq(other->target, target) &&
                            other->owner != txn->txn_id) {
                            conflict = true;
                            break;
                        }
                        other = other->next;
                    }
                    if (conflict) {
                        /* Cannot upgrade — add to wait queue and return timeout */
                        tdb_lock_waiter_t *w = (tdb_lock_waiter_t *)malloc(sizeof(*w));
                        if (!w) {
                            return TDB_ERR_NOMEM;
                        }
                        w->txn_id = txn->txn_id;
                        w->mode = mode;
                        w->target = target;
                        w->granted = false;
                        w->next = mgr->lock_table.wait_queue;
                        mgr->lock_table.wait_queue = w;
                        return TDB_ERR_LOCK_TIMEOUT;
                    }
                    /* No conflict — upgrade in place */
                    entry->mode = TDB_LOCK_EXCLUSIVE;
                    return TDB_OK;
                }
            } else {
                /* Different transaction holds a lock on this target */
                if (mode == TDB_LOCK_SHARED &&
                    entry->mode == TDB_LOCK_SHARED) {
                    /* Shared-shared: compatible, but we need to check all
                     * entries on this target to ensure none are exclusive. */
                    /* Continue scanning — will check after loop */
                } else {
                    /* Conflict: shared vs exclusive, or exclusive vs anything */
                    tdb_lock_waiter_t *w = (tdb_lock_waiter_t *)malloc(sizeof(*w));
                    if (!w) {
                        return TDB_ERR_NOMEM;
                    }
                    w->txn_id = txn->txn_id;
                    w->mode = mode;
                    w->target = target;
                    w->granted = false;
                    w->next = mgr->lock_table.wait_queue;
                    mgr->lock_table.wait_queue = w;
                    return TDB_ERR_LOCK_TIMEOUT;
                }
            }
        }
        entry = entry->next;
    }

    /* Check if we already hold a lock on this target (could be found above
     * in the shared-shared compatible path where we didn't return early).
     * Walk the txn's locks_held list. */
    {
        tdb_lock_entry_t *held = txn->locks_held;
        while (held) {
            if (lock_target_eq(held->target, target)) {
                /* Already have it (must be shared requesting shared) */
                return TDB_OK;
            }
            held = held->txn_next;
        }
    }

    /* No conflicts — grant the lock */
    tdb_lock_entry_t *new_entry = (tdb_lock_entry_t *)malloc(sizeof(*new_entry));
    if (!new_entry) {
        return TDB_ERR_NOMEM;
    }
    new_entry->target = target;
    new_entry->mode = mode;
    new_entry->owner = txn->txn_id;

    /* Insert into hash bucket chain */
    new_entry->next = mgr->lock_table.buckets[bucket];
    mgr->lock_table.buckets[bucket] = new_entry;

    /* Insert into txn's locks_held chain */
    new_entry->txn_next = txn->locks_held;
    txn->locks_held = new_entry;

    return TDB_OK;
}

tdb_status_t tdb_lock_release_all(tdb_txn_manager_t *mgr, tdb_txn_t *txn)
{
    if (!mgr || !txn) {
        return TDB_ERR_INVALID_ARG;
    }

    tdb_lock_entry_t *held = txn->locks_held;
    while (held) {
        tdb_lock_entry_t *next_held = held->txn_next;

        /* Remove from the hash bucket chain */
        size_t bucket = lock_hash(held->target);
        tdb_lock_entry_t **pp = &mgr->lock_table.buckets[bucket];
        while (*pp) {
            if (*pp == held) {
                *pp = held->next;
                break;
            }
            pp = &(*pp)->next;
        }

        free(held);
        held = next_held;
    }

    txn->locks_held = NULL;

    /* Also remove any wait queue entries for this txn */
    tdb_lock_waiter_t **wp = &mgr->lock_table.wait_queue;
    while (*wp) {
        if ((*wp)->txn_id == txn->txn_id) {
            tdb_lock_waiter_t *to_free = *wp;
            *wp = to_free->next;
            free(to_free);
        } else {
            wp = &(*wp)->next;
        }
    }

    return TDB_OK;
}

/* ----------------------------------------------------------------
 * Deadlock detection
 * ----------------------------------------------------------------
 *
 * Build a wait-for graph from the wait queue:
 *   For each waiter W that is not yet granted, find who holds the
 *   lock on W's target. W.txn_id waits-for holder.owner.
 *
 * Then do DFS from the given txn_id looking for a cycle back to itself.
 * ---------------------------------------------------------------- */

/* Maximum number of distinct transaction IDs we track in the DFS */
#define DFS_MAX_NODES 512

/* Edge in the wait-for graph */
typedef struct {
    tdb_txn_id_t from;
    tdb_txn_id_t to;
} wfg_edge_t;

/* DFS state */
typedef struct {
    wfg_edge_t   edges[DFS_MAX_NODES];
    size_t        edge_count;
    tdb_txn_id_t  visited[DFS_MAX_NODES];
    size_t        visited_count;
    tdb_txn_id_t  stack[DFS_MAX_NODES];
    size_t        stack_count;
    tdb_txn_id_t  target;
    bool          found_cycle;
} dfs_state_t;

static bool dfs_is_visited(const dfs_state_t *state, tdb_txn_id_t id)
{
    for (size_t i = 0; i < state->visited_count; i++) {
        if (state->visited[i] == id) {
            return true;
        }
    }
    return false;
}

static bool dfs_is_on_stack(const dfs_state_t *state, tdb_txn_id_t id)
{
    for (size_t i = 0; i < state->stack_count; i++) {
        if (state->stack[i] == id) {
            return true;
        }
    }
    return false;
}

static void dfs_visit(dfs_state_t *state, tdb_txn_id_t node)
{
    if (state->found_cycle) {
        return;
    }
    if (state->visited_count >= DFS_MAX_NODES ||
        state->stack_count >= DFS_MAX_NODES) {
        return;
    }

    state->visited[state->visited_count++] = node;
    state->stack[state->stack_count++] = node;

    /* Follow all edges from this node */
    for (size_t i = 0; i < state->edge_count; i++) {
        if (state->edges[i].from == node) {
            tdb_txn_id_t neighbor = state->edges[i].to;
            if (neighbor == state->target) {
                /* Found a cycle back to the original txn */
                state->found_cycle = true;
                return;
            }
            if (!dfs_is_visited(state, neighbor)) {
                dfs_visit(state, neighbor);
                if (state->found_cycle) {
                    return;
                }
            } else if (dfs_is_on_stack(state, neighbor)) {
                /* Back edge found — cycle involving txn_id if
                 * txn_id is on the stack */
                if (dfs_is_on_stack(state, state->target)) {
                    state->found_cycle = true;
                    return;
                }
            }
        }
    }

    /* Pop from stack */
    if (state->stack_count > 0) {
        state->stack_count--;
    }
}

tdb_status_t tdb_lock_detect_deadlock(tdb_txn_manager_t *mgr,
                                       tdb_txn_id_t txn_id)
{
    if (!mgr) {
        return TDB_ERR_INVALID_ARG;
    }

    dfs_state_t state;
    memset(&state, 0, sizeof(state));
    state.target = txn_id;

    /* Build the wait-for graph.
     * For each ungranted waiter, find which txns hold locks on that target. */
    tdb_lock_waiter_t *w = mgr->lock_table.wait_queue;
    while (w) {
        if (!w->granted) {
            /* Find holders of this target */
            size_t bucket = lock_hash(w->target);
            tdb_lock_entry_t *entry = mgr->lock_table.buckets[bucket];
            while (entry) {
                if (lock_target_eq(entry->target, w->target) &&
                    entry->owner != w->txn_id) {
                    /* w->txn_id waits for entry->owner */
                    if (state.edge_count < DFS_MAX_NODES) {
                        state.edges[state.edge_count].from = w->txn_id;
                        state.edges[state.edge_count].to = entry->owner;
                        state.edge_count++;
                    }
                }
                entry = entry->next;
            }
        }
        w = w->next;
    }

    if (state.edge_count == 0) {
        return TDB_OK;
    }

    /* Run DFS from txn_id */
    dfs_visit(&state, txn_id);

    if (state.found_cycle) {
        return TDB_ERR_DEADLOCK;
    }

    return TDB_OK;
}
