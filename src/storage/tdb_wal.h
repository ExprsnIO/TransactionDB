/*
** tdb_wal.h — write-ahead log for crash-safe, atomic commits.
**
** Modified page images are appended to a separate WAL file as checksummed
** frames; the final frame of a commit batch carries the post-commit database
** size, which marks it as a commit point. Readers consult the WAL (most recent
** frame for a page wins) before the main database file. A checkpoint copies
** committed frames back into the database file and resets the log.
**
** Recovery on open replays frames, validating the rolling checksum and salts,
** and accepts only frames up to the last valid commit point — so a torn or
** partially-written final batch is discarded, giving atomic durability.
*/
#ifndef TDB_WAL_H
#define TDB_WAL_H

#include "tdb_file.h"

typedef struct tdb_wal tdb_wal;

/* Open (and recover) a WAL over an already-opened WAL file. */
int  tdb_wal_open(tdb_file *wal_file, uint32_t page_size, tdb_wal **out);
void tdb_wal_close(tdb_wal *wal);

/* Read the most recent committed image of `pgno` into `out` (page_size bytes).
** *found is set to 1 if the WAL contained the page, else 0. */
int  tdb_wal_read_page(tdb_wal *wal, tdb_pgno pgno, uint8_t *out, int *found);

/* Append one frame. The last frame of a commit batch passes is_commit=1 and
** the post-commit db size in pages. */
int  tdb_wal_append(tdb_wal *wal, tdb_pgno pgno, const uint8_t *page,
                    int is_commit, uint32_t db_size);

/* Flush appended frames durably to disk. */
int  tdb_wal_sync(tdb_wal *wal);

/* Largest db size recorded by the last commit frame (0 if none). */
uint32_t tdb_wal_db_size(tdb_wal *wal);

/* Number of committed frames currently in the log. */
uint64_t tdb_wal_frame_count(tdb_wal *wal);

/* Copy all committed page images into `db`, fsync it, and reset the log.
** On success *new_db_size (if non-NULL) receives the checkpointed db size. */
int  tdb_wal_checkpoint(tdb_wal *wal, tdb_file *db, tdb_pgno *new_db_size);

#endif /* TDB_WAL_H */
