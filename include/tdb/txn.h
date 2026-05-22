#ifndef TDB_TXN_H
#define TDB_TXN_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Transaction states */
typedef enum {
    TDB_TXN_ACTIVE = 0,
    TDB_TXN_COMMITTED,
    TDB_TXN_ABORTED,
} tdb_txn_state_t;

/* Isolation levels */
typedef enum {
    TDB_ISO_READ_UNCOMMITTED,
    TDB_ISO_READ_COMMITTED,
    TDB_ISO_REPEATABLE_READ,
    TDB_ISO_SERIALIZABLE,
    TDB_ISO_SNAPSHOT,
} tdb_isolation_t;

/* Lock modes — supports row-level and object-level locking */
typedef enum {
    TDB_LOCK_SHARED = 0,
    TDB_LOCK_EXCLUSIVE,
    TDB_LOCK_INTENT_SHARED,     /* intent lock for object-level */
    TDB_LOCK_INTENT_EXCLUSIVE,  /* intent lock for object-level */
    TDB_LOCK_SHARE_ROW_EXCLUSIVE,
    TDB_LOCK_ACCESS_SHARE,
    TDB_LOCK_ROW_SHARE,
    TDB_LOCK_ROW_EXCLUSIVE,
    TDB_LOCK_SHARE_UPDATE_EXCLUSIVE,
    TDB_LOCK_ACCESS_EXCLUSIVE,
    TDB_LOCK_ADVISORY_SHARED,   /* advisory locks */
    TDB_LOCK_ADVISORY_EXCLUSIVE,
} tdb_lock_mode_t;

/* Lock granularity levels */
typedef enum {
    TDB_LOCK_GRAN_ROW = 0,     /* row-level: specific page+slot */
    TDB_LOCK_GRAN_PAGE,        /* page-level: entire page */
    TDB_LOCK_GRAN_OBJECT,      /* object-level: named object (table, index, view, etc.) */
    TDB_LOCK_GRAN_TABLE,       /* table-level: shorthand for object lock on table */
    TDB_LOCK_GRAN_DATABASE,    /* database-level */
    TDB_LOCK_GRAN_ADVISORY,    /* user-defined advisory lock */
} tdb_lock_granularity_t;

/* Lock request target: supports row, object, and advisory levels */
typedef struct {
    tdb_lock_granularity_t granularity;
    tdb_page_id_t  page_id;     /* for ROW/PAGE level */
    tdb_slot_t     slot;        /* for ROW level */
    uint64_t       object_id;   /* for OBJECT/TABLE/DATABASE level (hash of name) */
    uint64_t       advisory_key; /* for ADVISORY level (user-chosen key) */
} tdb_lock_target_t;

/* A single lock held by a transaction */
typedef struct tdb_lock_entry {
    tdb_lock_target_t      target;
    tdb_lock_mode_t        mode;
    tdb_txn_id_t           owner;
    struct tdb_lock_entry *next;        /* chain in hash bucket */
    struct tdb_lock_entry *txn_next;    /* chain of locks held by this txn */
} tdb_lock_entry_t;

/* Lock wait queue entry */
typedef struct tdb_lock_waiter {
    tdb_txn_id_t            txn_id;
    tdb_lock_mode_t         mode;
    tdb_lock_target_t       target;
    bool                    granted;
    struct tdb_lock_waiter *next;
} tdb_lock_waiter_t;

/* Lock table */
#define TDB_LOCK_TABLE_SIZE 4096

typedef struct {
    tdb_lock_entry_t  *buckets[TDB_LOCK_TABLE_SIZE];
    tdb_lock_waiter_t *wait_queue;
} tdb_lock_table_t;

/* Transaction descriptor */
typedef struct {
    tdb_txn_id_t    txn_id;
    tdb_txn_state_t state;
    tdb_isolation_t isolation;
    tdb_txn_id_t    snapshot_xmin;  /* oldest active txn at snapshot time */
    tdb_txn_id_t    snapshot_xmax;  /* next txn id at snapshot time */
    bool            read_only;
    tdb_lsn_t       last_lsn;      /* last LSN written by this txn */
    tdb_lock_entry_t *locks_held;   /* linked list of locks held */
} tdb_txn_t;

/* Active transaction table entry */
typedef struct {
    tdb_txn_id_t    txn_id;
    tdb_txn_state_t state;
    tdb_lsn_t       last_lsn;
} tdb_att_entry_t;

/* Transaction manager */
#define TDB_MAX_ACTIVE_TXNS 256

typedef struct {
    tdb_txn_id_t     next_txn_id;
    tdb_att_entry_t  active[TDB_MAX_ACTIVE_TXNS];
    size_t           active_count;
    tdb_lock_table_t lock_table;
} tdb_txn_manager_t;

/* Transaction manager lifecycle */
tdb_status_t tdb_txn_manager_init(tdb_txn_manager_t *mgr);
void         tdb_txn_manager_destroy(tdb_txn_manager_t *mgr);

/* Transaction operations */
tdb_status_t tdb_txn_begin(tdb_txn_manager_t *mgr, tdb_txn_t *txn, tdb_isolation_t iso);
tdb_status_t tdb_txn_commit(tdb_txn_manager_t *mgr, tdb_txn_t *txn);
tdb_status_t tdb_txn_abort(tdb_txn_manager_t *mgr, tdb_txn_t *txn);

/* MVCC visibility check */
bool tdb_txn_is_visible(const tdb_txn_t *txn, const tdb_row_version_t *ver);

/* Lock operations */
tdb_status_t tdb_lock_acquire(tdb_txn_manager_t *mgr, tdb_txn_t *txn,
                               tdb_lock_target_t target, tdb_lock_mode_t mode);
tdb_status_t tdb_lock_release_all(tdb_txn_manager_t *mgr, tdb_txn_t *txn);

/* Deadlock detection (returns TDB_ERR_DEADLOCK if cycle found) */
tdb_status_t tdb_lock_detect_deadlock(tdb_txn_manager_t *mgr, tdb_txn_id_t txn_id);

#ifdef __cplusplus
}
#endif

#endif /* TDB_TXN_H */
