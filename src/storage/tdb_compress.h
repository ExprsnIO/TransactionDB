/*
** tdb_compress.h — page/blob compression interface.
**
** The interface is a thin wrapper around a build-time-selected codec.
** When the library is configured with -DTDB_BUILD_ZSTD=ON the wrapper
** dispatches to Zstandard; without it the functions are an identity codec
** (used for tests and to keep the API stable across builds).
**
** The intended use is page-image compression on the durability path:
** committed page images can be compressed before they are appended to the
** WAL, and decompressed on replay. The on-disk frame still carries the raw
** page size, so a future codec change can be made without a format break.
*/
#ifndef TDB_COMPRESS_H
#define TDB_COMPRESS_H

#include "../common/tdb_internal.h"

/* Codec identifiers (stored on disk wherever a compressed payload appears). */
#define TDB_COMP_NONE 0
#define TDB_COMP_ZSTD 1

/* Returns the codec id active in this build (TDB_COMP_NONE or TDB_COMP_ZSTD). */
int tdb_compress_codec(void);

/* The codec name ("none", "zstd"). Always non-NULL. */
const char *tdb_compress_codec_name(void);

/* Worst-case output size for compressing `src_n` bytes. */
size_t tdb_compress_bound(size_t src_n);

/* Compress src[0..src_n) into dst[0..*dst_n); on entry *dst_n holds the
** capacity of dst, on exit the compressed length. Returns TDB_OK or
** TDB_NOMEM / TDB_ERROR. */
int tdb_compress(const void *src, size_t src_n, void *dst, size_t *dst_n);

/* Decompress src[0..src_n) into dst[0..*dst_n); on entry *dst_n is the
** expected decompressed size (must be exact for the zstd path). */
int tdb_decompress(const void *src, size_t src_n, void *dst, size_t *dst_n);

#endif /* TDB_COMPRESS_H */
