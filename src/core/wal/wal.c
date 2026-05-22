/*
 * Write-Ahead Log (ARIES-based)
 *
 * Log record types:
 * - UPDATE:     before/after images for physiological undo/redo
 * - COMMIT:     transaction commit marker
 * - ABORT:      transaction abort marker
 * - CHECKPOINT: active transaction table + dirty page table
 * - CLR:        compensation log record for undo operations
 * - BEGIN:      transaction begin marker
 *
 * WAL file format: sequential records, each consisting of:
 *   [tdb_wal_record_t header][before_image bytes][after_image bytes]
 *
 * LSN is the byte offset within the WAL file where the record starts.
 */

#include "tdb/wal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * tdb_wal_init
 * --------------------------------------------------------------------------
 * Open or create the WAL file.  Allocate TDB_WAL_BUFFER_SIZE bytes for the
 * in-memory log buffer.  Walk the existing file to find the byte offset just
 * past the last valid record — that becomes next_lsn.
 */
tdb_status_t tdb_wal_init(tdb_wal_t *wal, const char *path)
{
    if (!wal || !path) {
        return TDB_ERR_INVALID_ARG;
    }

    memset(wal, 0, sizeof(*wal));

    wal->path = strdup(path);
    if (!wal->path) {
        return TDB_ERR_NOMEM;
    }

    wal->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (wal->fd < 0) {
        free(wal->path);
        wal->path = NULL;
        return TDB_ERR_IO;
    }

    wal->buffer = (uint8_t *)malloc(TDB_WAL_BUFFER_SIZE);
    if (!wal->buffer) {
        close(wal->fd);
        wal->fd = -1;
        free(wal->path);
        wal->path = NULL;
        return TDB_ERR_NOMEM;
    }
    wal->buf_size   = TDB_WAL_BUFFER_SIZE;
    wal->buf_offset = 0;

    /*
     * Determine next_lsn by scanning existing records in the file.
     * Each record header contains total_len which tells us the full
     * record size (header + before_image + after_image).  We walk
     * forward until we can no longer read a complete record.
     */
    struct stat st;
    if (fstat(wal->fd, &st) < 0) {
        free(wal->buffer);
        close(wal->fd);
        free(wal->path);
        memset(wal, 0, sizeof(*wal));
        return TDB_ERR_IO;
    }

    off_t file_size = st.st_size;
    tdb_lsn_t pos  = 0;

    while ((off_t)pos + (off_t)sizeof(tdb_wal_record_t) <= file_size) {
        tdb_wal_record_t hdr;
        ssize_t nr = pread(wal->fd, &hdr, sizeof(hdr), (off_t)pos);
        if (nr != (ssize_t)sizeof(hdr)) {
            break; /* partial header — stop */
        }
        if (hdr.total_len < sizeof(tdb_wal_record_t)) {
            break; /* corrupt or zero — stop */
        }
        if ((off_t)pos + (off_t)hdr.total_len > file_size) {
            break; /* incomplete record — stop */
        }
        pos += hdr.total_len;
    }

    wal->next_lsn      = pos;
    wal->flushed_lsn   = pos;
    wal->buf_start_lsn = pos;

    return TDB_OK;
}

/* --------------------------------------------------------------------------
 * tdb_wal_destroy
 * --------------------------------------------------------------------------
 * Flush any remaining buffered data, close the file descriptor, and free all
 * dynamically allocated memory.
 */
void tdb_wal_destroy(tdb_wal_t *wal)
{
    if (!wal) {
        return;
    }

    /* Best-effort flush of remaining buffer contents. */
    if (wal->fd >= 0 && wal->buf_offset > 0) {
        tdb_wal_flush(wal, wal->next_lsn);
    }

    if (wal->fd >= 0) {
        close(wal->fd);
        wal->fd = -1;
    }
    free(wal->buffer);
    wal->buffer = NULL;
    free(wal->path);
    wal->path = NULL;
}

/* --------------------------------------------------------------------------
 * internal: flush the entire buffer to disk
 * --------------------------------------------------------------------------
 * Writes all bytes in [buffer, buffer + buf_offset) to the WAL file at
 * offset buf_start_lsn, then calls fsync().
 */
static tdb_status_t wal_flush_buffer(tdb_wal_t *wal)
{
    if (wal->buf_offset == 0) {
        return TDB_OK;
    }

    size_t  remaining = wal->buf_offset;
    size_t  written   = 0;
    off_t   offset    = (off_t)wal->buf_start_lsn;

    while (written < remaining) {
        ssize_t nw = pwrite(wal->fd,
                            wal->buffer + written,
                            remaining - written,
                            offset + (off_t)written);
        if (nw < 0) {
            if (errno == EINTR) {
                continue;
            }
            return TDB_ERR_IO;
        }
        written += (size_t)nw;
    }

    if (fsync(wal->fd) < 0) {
        return TDB_ERR_IO;
    }

    wal->flushed_lsn   = wal->buf_start_lsn + wal->buf_offset;
    wal->buf_start_lsn = wal->flushed_lsn;
    wal->buf_offset    = 0;

    return TDB_OK;
}

/* --------------------------------------------------------------------------
 * tdb_wal_flush
 * --------------------------------------------------------------------------
 * Flush buffer contents to disk.  If up_to_lsn falls within (or beyond) the
 * current buffer we flush everything; otherwise the request is already
 * satisfied by a previous flush and we return immediately.
 */
tdb_status_t tdb_wal_flush(tdb_wal_t *wal, tdb_lsn_t up_to_lsn)
{
    if (!wal) {
        return TDB_ERR_INVALID_ARG;
    }

    /* Already durable? */
    if (up_to_lsn <= wal->flushed_lsn) {
        return TDB_OK;
    }

    return wal_flush_buffer(wal);
}

/* --------------------------------------------------------------------------
 * tdb_wal_append
 * --------------------------------------------------------------------------
 * Serialize a WAL record header followed by the before- and after-images into
 * the in-memory buffer.  The LSN assigned to the record is the byte offset
 * in the WAL file where the record will reside once flushed.
 *
 * If the serialized record would not fit in the remaining buffer space the
 * buffer is flushed first.  Records that exceed the entire buffer size are
 * rejected.
 */
tdb_status_t tdb_wal_append(tdb_wal_t *wal, tdb_wal_record_t *record,
                             const void *before, const void *after)
{
    if (!wal || !record) {
        return TDB_ERR_INVALID_ARG;
    }

    uint16_t before_len = before ? record->before_len : 0;
    uint16_t after_len  = after  ? record->after_len  : 0;

    /* If caller passed NULL images, normalise lengths in the record. */
    record->before_len = before_len;
    record->after_len  = after_len;

    uint32_t total = (uint32_t)sizeof(tdb_wal_record_t) + before_len + after_len;
    record->total_len = total;

    if (total > wal->buf_size) {
        return TDB_ERR_FULL; /* record exceeds entire buffer */
    }

    /* Flush if the record would overflow the buffer. */
    if (wal->buf_offset + total > wal->buf_size) {
        tdb_status_t s = wal_flush_buffer(wal);
        if (s != TDB_OK) {
            return s;
        }
    }

    /* Assign a monotonically increasing LSN (byte offset in file). */
    record->lsn = wal->next_lsn;

    /* Copy the header into the buffer. */
    memcpy(wal->buffer + wal->buf_offset, record, sizeof(tdb_wal_record_t));
    wal->buf_offset += sizeof(tdb_wal_record_t);

    /* Copy before-image. */
    if (before_len > 0) {
        memcpy(wal->buffer + wal->buf_offset, before, before_len);
        wal->buf_offset += before_len;
    }

    /* Copy after-image. */
    if (after_len > 0) {
        memcpy(wal->buffer + wal->buf_offset, after, after_len);
        wal->buf_offset += after_len;
    }

    wal->next_lsn += total;

    return TDB_OK;
}

/* --------------------------------------------------------------------------
 * tdb_wal_checkpoint
 * --------------------------------------------------------------------------
 * Write a checkpoint record that embeds the Active Transaction Table (ATT)
 * and the Dirty Page Table (DPT).  The payload layout stored in the before-
 * and after-image area is:
 *
 *   [uint32_t att_count]
 *   [tdb_wal_att_entry_t * att_count]
 *   [uint32_t dpt_count]
 *   [tdb_wal_dpt_entry_t * dpt_count]
 *
 * We stuff this payload into before_image area for simplicity (after_image
 * is unused).  The record type is TDB_WAL_CHECKPOINT so the recovery code
 * knows to interpret the payload accordingly.
 */
tdb_status_t tdb_wal_checkpoint(tdb_wal_t *wal,
                                 const tdb_wal_att_entry_t *att, size_t att_count,
                                 const tdb_wal_dpt_entry_t *dpt, size_t dpt_count)
{
    if (!wal) {
        return TDB_ERR_INVALID_ARG;
    }

    /* Build the checkpoint payload. */
    size_t payload_size = sizeof(uint32_t)
                        + att_count * sizeof(tdb_wal_att_entry_t)
                        + sizeof(uint32_t)
                        + dpt_count * sizeof(tdb_wal_dpt_entry_t);

    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) {
        return TDB_ERR_NOMEM;
    }

    size_t off = 0;
    uint32_t att32 = (uint32_t)att_count;
    memcpy(payload + off, &att32, sizeof(att32));
    off += sizeof(att32);

    if (att_count > 0 && att) {
        memcpy(payload + off, att, att_count * sizeof(tdb_wal_att_entry_t));
        off += att_count * sizeof(tdb_wal_att_entry_t);
    }

    uint32_t dpt32 = (uint32_t)dpt_count;
    memcpy(payload + off, &dpt32, sizeof(dpt32));
    off += sizeof(dpt32);

    if (dpt_count > 0 && dpt) {
        memcpy(payload + off, dpt, dpt_count * sizeof(tdb_wal_dpt_entry_t));
    }

    /* Build the WAL record.  The checkpoint payload goes into before_image. */
    tdb_wal_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.type       = TDB_WAL_CHECKPOINT;
    rec.txn_id     = 0;
    rec.prev_lsn   = 0;
    rec.page_id    = 0;
    rec.offset     = 0;
    rec.before_len = (uint16_t)payload_size;
    rec.after_len  = 0;

    tdb_status_t status = tdb_wal_append(wal, &rec, payload, NULL);
    free(payload);

    if (status != TDB_OK) {
        return status;
    }

    /* Force checkpoint record to disk immediately. */
    return tdb_wal_flush(wal, wal->next_lsn);
}

/* --------------------------------------------------------------------------
 * tdb_wal_scan
 * --------------------------------------------------------------------------
 * Read WAL records from the file starting at start_lsn.  For each valid
 * record, deserialize the header and locate the before/after images, then
 * invoke the user-supplied callback.
 *
 * Reading is done via pread() directly from the file.  This means only
 * flushed records are visible; any records still in the in-memory buffer
 * are not scanned.  The caller should flush before scanning if they need
 * to see the latest records.
 *
 * Scanning stops gracefully when we encounter a partial header read, a
 * zero total_len, or a partial record body — these indicate EOF or a torn
 * write.
 */
tdb_status_t tdb_wal_scan(tdb_wal_t *wal, tdb_lsn_t start_lsn,
                            tdb_wal_scan_cb callback, void *ctx)
{
    if (!wal || !callback) {
        return TDB_ERR_INVALID_ARG;
    }

    /* Determine the end of flushed data on disk. */
    struct stat st;
    if (fstat(wal->fd, &st) < 0) {
        return TDB_ERR_IO;
    }
    off_t file_size = st.st_size;

    tdb_lsn_t pos = start_lsn;
    uint8_t  *rec_buf = NULL;
    size_t    rec_buf_cap = 0;

    while ((off_t)pos + (off_t)sizeof(tdb_wal_record_t) <= file_size) {
        /* Read the header. */
        tdb_wal_record_t hdr;
        ssize_t nr = pread(wal->fd, &hdr, sizeof(hdr), (off_t)pos);
        if (nr != (ssize_t)sizeof(hdr)) {
            break; /* partial header read — EOF */
        }

        /* Validate total_len. */
        if (hdr.total_len < sizeof(tdb_wal_record_t)) {
            break; /* corrupt or zero marker — stop */
        }
        if ((off_t)pos + (off_t)hdr.total_len > file_size) {
            break; /* incomplete record — torn write at EOF */
        }

        /* Read before_image + after_image. */
        size_t img_len = (size_t)hdr.before_len + (size_t)hdr.after_len;
        const void *before_ptr = NULL;
        const void *after_ptr  = NULL;

        if (img_len > 0) {
            /* Ensure our read buffer is large enough. */
            if (img_len > rec_buf_cap) {
                uint8_t *tmp = (uint8_t *)realloc(rec_buf, img_len);
                if (!tmp) {
                    free(rec_buf);
                    return TDB_ERR_NOMEM;
                }
                rec_buf     = tmp;
                rec_buf_cap = img_len;
            }

            ssize_t img_nr = pread(wal->fd, rec_buf, img_len,
                                   (off_t)pos + (off_t)sizeof(tdb_wal_record_t));
            if (img_nr != (ssize_t)img_len) {
                break; /* partial image data — stop */
            }

            if (hdr.before_len > 0) {
                before_ptr = rec_buf;
            }
            if (hdr.after_len > 0) {
                after_ptr = rec_buf + hdr.before_len;
            }
        }

        /* Invoke the callback. */
        callback(&hdr, before_ptr, after_ptr, ctx);

        pos += hdr.total_len;
    }

    free(rec_buf);
    return TDB_OK;
}
