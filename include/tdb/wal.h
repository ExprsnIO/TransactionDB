#ifndef TDB_WAL_H
#define TDB_WAL_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WAL record types */
typedef enum {
    TDB_WAL_UPDATE = 1,
    TDB_WAL_COMMIT,
    TDB_WAL_ABORT,
    TDB_WAL_CHECKPOINT,
    TDB_WAL_CLR,
    TDB_WAL_BEGIN,
} tdb_wal_record_type_t;

/* WAL record header — followed by before_image[before_len] + after_image[after_len] */
typedef struct {
    tdb_lsn_t              lsn;
    tdb_lsn_t              prev_lsn;      /* previous LSN for this transaction */
    tdb_txn_id_t           txn_id;
    tdb_wal_record_type_t  type;
    tdb_page_id_t          page_id;
    uint16_t               offset;        /* byte offset within page */
    uint16_t               before_len;
    uint16_t               after_len;
    uint32_t               total_len;     /* header + before + after */
} tdb_wal_record_t;

/* Checkpoint data: active transaction table entry */
typedef struct {
    tdb_txn_id_t txn_id;
    tdb_lsn_t    last_lsn;
} tdb_wal_att_entry_t;

/* Checkpoint data: dirty page table entry */
typedef struct {
    tdb_page_id_t page_id;
    tdb_lsn_t     rec_lsn;    /* first LSN that dirtied this page */
} tdb_wal_dpt_entry_t;

#define TDB_WAL_BUFFER_SIZE (1024 * 1024)  /* 1 MB log buffer */

typedef struct {
    int         fd;
    char       *path;
    tdb_lsn_t   next_lsn;
    tdb_lsn_t   flushed_lsn;
    uint8_t    *buffer;
    size_t      buf_size;
    size_t      buf_offset;     /* current write position in buffer */
    tdb_lsn_t   buf_start_lsn; /* LSN of first record in buffer */
} tdb_wal_t;

/* Initialize WAL from a file path */
tdb_status_t tdb_wal_init(tdb_wal_t *wal, const char *path);

/* Close and free WAL resources */
void tdb_wal_destroy(tdb_wal_t *wal);

/* Append a log record. Assigns LSN and returns it via record->lsn.
 * before/after may be NULL for commit/abort/begin records. */
tdb_status_t tdb_wal_append(tdb_wal_t *wal, tdb_wal_record_t *record,
                             const void *before, const void *after);

/* Flush the log buffer to disk up to the given LSN */
tdb_status_t tdb_wal_flush(tdb_wal_t *wal, tdb_lsn_t up_to_lsn);

/* Write a checkpoint record containing the active transaction table
 * and dirty page table */
tdb_status_t tdb_wal_checkpoint(tdb_wal_t *wal,
                                 const tdb_wal_att_entry_t *att, size_t att_count,
                                 const tdb_wal_dpt_entry_t *dpt, size_t dpt_count);

/* Recovery: read log records starting from given LSN.
 * Calls callback for each record. Returns TDB_OK when log is exhausted. */
typedef void (*tdb_wal_scan_cb)(const tdb_wal_record_t *record,
                                 const void *before, const void *after,
                                 void *ctx);

tdb_status_t tdb_wal_scan(tdb_wal_t *wal, tdb_lsn_t start_lsn,
                            tdb_wal_scan_cb callback, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TDB_WAL_H */
