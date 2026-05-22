#include "tdb/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

tdb_status_t tdb_storage_open(tdb_storage_t *store, const char *path, size_t page_size) {
    if (!store || !path || page_size < 128) return TDB_ERR_INVALID_ARG;

    store->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (store->fd < 0) return TDB_ERR_IO;

    store->path = strdup(path);
    if (!store->path) { close(store->fd); return TDB_ERR_NOMEM; }
    store->page_size = page_size;

    struct stat st;
    if (fstat(store->fd, &st) == 0) {
        store->page_count = (uint64_t)st.st_size / page_size;
    } else {
        store->page_count = 0;
    }

    return TDB_OK;
}

tdb_status_t tdb_storage_close(tdb_storage_t *store) {
    if (!store) return TDB_ERR_INVALID_ARG;
    if (store->fd >= 0) {
        fsync(store->fd);
        close(store->fd);
        store->fd = -1;
    }
    free(store->path);
    store->path = NULL;
    return TDB_OK;
}

tdb_status_t tdb_storage_read_page(tdb_storage_t *store, tdb_page_id_t page_id, void *buf) {
    if (!store || !buf) return TDB_ERR_INVALID_ARG;

    off_t offset = (off_t)(page_id * store->page_size);
    ssize_t n = pread(store->fd, buf, store->page_size, offset);
    if (n < 0 || (size_t)n != store->page_size) return TDB_ERR_IO;

    return TDB_OK;
}

tdb_status_t tdb_storage_write_page(tdb_storage_t *store, tdb_page_id_t page_id, const void *buf) {
    if (!store || !buf) return TDB_ERR_INVALID_ARG;

    off_t offset = (off_t)(page_id * store->page_size);
    ssize_t n = pwrite(store->fd, buf, store->page_size, offset);
    if (n < 0 || (size_t)n != store->page_size) return TDB_ERR_IO;

    return TDB_OK;
}

tdb_status_t tdb_storage_extend(tdb_storage_t *store, tdb_page_id_t *new_page_id) {
    if (!store) return TDB_ERR_INVALID_ARG;

    void *blank = calloc(1, store->page_size);
    if (!blank) return TDB_ERR_NOMEM;

    tdb_page_id_t pid = store->page_count;
    off_t offset = (off_t)(pid * store->page_size);
    ssize_t n = pwrite(store->fd, blank, store->page_size, offset);
    free(blank);

    if (n < 0 || (size_t)n != store->page_size) return TDB_ERR_IO;

    store->page_count++;
    if (new_page_id) *new_page_id = pid;

    return TDB_OK;
}

tdb_status_t tdb_storage_sync(tdb_storage_t *store) {
    if (!store) return TDB_ERR_INVALID_ARG;
    if (fsync(store->fd) != 0) return TDB_ERR_IO;
    return TDB_OK;
}

/* ─── Page-level compression (P4) ────────────────────────────────────── */

#ifdef TDB_WITH_ZSTD
# include <zstd.h>
#endif

size_t tdb_compress_bound(size_t in_len) {
#ifdef TDB_WITH_ZSTD
    return 4 + ZSTD_compressBound(in_len);
#else
    /* Pass-through: 4-byte frame header + raw bytes. */
    return 4 + in_len;
#endif
}

int tdb_compression_available(void) {
#ifdef TDB_WITH_ZSTD
    return 1;
#else
    return 0;
#endif
}

tdb_status_t tdb_compress_page(const void *in, size_t in_len,
                                void *out, size_t *out_len) {
    if (!in || !out || !out_len) return TDB_ERR_INVALID_ARG;
    if (*out_len < 4) return TDB_ERR_INVALID_ARG;
    unsigned char *o = (unsigned char *)out;
    o[0] = 'T'; o[1] = 'Z'; o[2] = '1';
#ifdef TDB_WITH_ZSTD
    if (*out_len < 4 + ZSTD_compressBound(in_len)) return TDB_ERR_INVALID_ARG;
    size_t r = ZSTD_compress(o + 4, *out_len - 4, in, in_len, 3);
    if (ZSTD_isError(r)) return TDB_ERR_IO;
    o[3] = 1; /* codec: zstd */
    *out_len = 4 + r;
    return TDB_OK;
#else
    if (*out_len < 4 + in_len) return TDB_ERR_INVALID_ARG;
    o[3] = 0; /* codec: stored */
    memcpy(o + 4, in, in_len);
    *out_len = 4 + in_len;
    return TDB_OK;
#endif
}

tdb_status_t tdb_decompress_page(const void *in, size_t in_len,
                                  void *out, size_t *out_len) {
    if (!in || !out || !out_len) return TDB_ERR_INVALID_ARG;
    if (in_len < 4) return TDB_ERR_INVALID_ARG;
    const unsigned char *i = (const unsigned char *)in;
    if (i[0] != 'T' || i[1] != 'Z' || i[2] != '1') return TDB_ERR_INVALID_ARG;
    unsigned char codec = i[3];
    if (codec == 0) {
        /* Stored: raw bytes follow the header. */
        size_t payload = in_len - 4;
        if (*out_len < payload) return TDB_ERR_INVALID_ARG;
        memcpy(out, i + 4, payload);
        *out_len = payload;
        return TDB_OK;
    }
#ifdef TDB_WITH_ZSTD
    if (codec == 1) {
        size_t r = ZSTD_decompress(out, *out_len, i + 4, in_len - 4);
        if (ZSTD_isError(r)) return TDB_ERR_IO;
        *out_len = r;
        return TDB_OK;
    }
#endif
    /* Unknown / unsupported codec on this build. */
    return TDB_ERR_INVALID_ARG;
}
