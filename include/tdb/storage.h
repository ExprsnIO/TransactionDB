#ifndef TDB_STORAGE_H
#define TDB_STORAGE_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storage manager: raw file I/O for database pages */
typedef struct {
    int         fd;
    char       *path;
    size_t      page_size;
    uint64_t    page_count;
} tdb_storage_t;

tdb_status_t tdb_storage_open(tdb_storage_t *store, const char *path, size_t page_size);
tdb_status_t tdb_storage_close(tdb_storage_t *store);
tdb_status_t tdb_storage_read_page(tdb_storage_t *store, tdb_page_id_t page_id, void *buf);
tdb_status_t tdb_storage_write_page(tdb_storage_t *store, tdb_page_id_t page_id, const void *buf);
tdb_status_t tdb_storage_extend(tdb_storage_t *store, tdb_page_id_t *new_page_id);
tdb_status_t tdb_storage_sync(tdb_storage_t *store);

/* ─── Page-level compression (Batch P4) ────────────────────────────────
 *
 * Compress / decompress raw page bytes. The implementation is gated by the
 * TDB_WITH_ZSTD compile flag set from CMake's find_package(zstd). When zstd
 * is not linked, the helpers are pass-through (output == input) but return
 * success — call sites can use the same API on every build, and zstd
 * presence becomes a runtime quality-of-service property rather than an
 * ABI break.
 *
 * Output buffer must be at least tdb_compress_bound(in_len) bytes. The
 * helpers write the actual compressed length back through *out_len.
 *
 * Frame format (4-byte big-endian prefix):
 *   bytes 0..2:  magic "TZ1"
 *   byte    3:   codec id: 0 = stored (no compression), 1 = zstd
 *   bytes 4..N:  payload (compressed or raw)
 *
 * decompress_page validates the magic, picks the codec, and writes the
 * decoded bytes. A mismatched magic returns TDB_ERR_INVALID_ARG. */

size_t        tdb_compress_bound(size_t in_len);
tdb_status_t  tdb_compress_page(const void *in, size_t in_len,
                                 void *out, size_t *out_len);
tdb_status_t  tdb_decompress_page(const void *in, size_t in_len,
                                   void *out, size_t *out_len);
/* Returns 1 if the linked binary was built with TDB_WITH_ZSTD, else 0. */
int           tdb_compression_available(void);

#ifdef __cplusplus
}
#endif

#endif /* TDB_STORAGE_H */
