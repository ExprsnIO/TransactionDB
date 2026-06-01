/*
** tdb_compress.c — selectable compression codec.
**
** When TDB_HAVE_ZSTD is set at build time, the library is linked against
** Zstandard and the compress/decompress routines dispatch through it.
** Otherwise the codec is an identity passthrough — useful for builds
** without the zstd dependency and as the default for in-memory databases.
*/
#include "tdb_compress.h"
#include "transactiondb.h"

#include <string.h>

#ifdef TDB_HAVE_ZSTD
#include <zstd.h>
#endif

int tdb_compress_codec(void) {
#ifdef TDB_HAVE_ZSTD
  return TDB_COMP_ZSTD;
#else
  return TDB_COMP_NONE;
#endif
}

const char *tdb_compress_codec_name(void) {
#ifdef TDB_HAVE_ZSTD
  return "zstd";
#else
  return "none";
#endif
}

size_t tdb_compress_bound(size_t src_n) {
#ifdef TDB_HAVE_ZSTD
  return ZSTD_compressBound(src_n);
#else
  return src_n;
#endif
}

int tdb_compress(const void *src, size_t src_n, void *dst, size_t *dst_n) {
  if (!src || !dst || !dst_n) return TDB_MISUSE;
#ifdef TDB_HAVE_ZSTD
  size_t r = ZSTD_compress(dst, *dst_n, src, src_n, 3);   /* level 3 = a balanced default */
  if (ZSTD_isError(r)) return TDB_ERROR;
  *dst_n = r;
  return TDB_OK;
#else
  if (*dst_n < src_n) return TDB_NOMEM;
  memcpy(dst, src, src_n);
  *dst_n = src_n;
  return TDB_OK;
#endif
}

int tdb_decompress(const void *src, size_t src_n, void *dst, size_t *dst_n) {
  if (!src || !dst || !dst_n) return TDB_MISUSE;
#ifdef TDB_HAVE_ZSTD
  size_t r = ZSTD_decompress(dst, *dst_n, src, src_n);
  if (ZSTD_isError(r)) return TDB_ERROR;
  *dst_n = r;
  return TDB_OK;
#else
  if (*dst_n < src_n) return TDB_NOMEM;
  memcpy(dst, src, src_n);
  *dst_n = src_n;
  return TDB_OK;
#endif
}
