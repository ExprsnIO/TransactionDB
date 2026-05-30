/*
** tdb_util.h — low-level encoding helpers shared across the engine.
**
**  - LEB128-style unsigned varints (used by records, btree cells, WAL).
**  - Big-endian fixed-width integer put/get (all on-disk integers are BE).
**  - CRC32C (Castagnoli) checksums and FNV-1a hashing.
*/
#ifndef TDB_UTIL_H
#define TDB_UTIL_H

#include "tdb_internal.h"

/* Varints: encode up to 9 bytes. Returns number of bytes written/read. */
int tdb_put_varint(uint8_t *p, uint64_t v);
int tdb_get_varint(const uint8_t *p, size_t avail, uint64_t *v);
int tdb_varint_len(uint64_t v);

/* Big-endian fixed-width helpers. */
void     tdb_put_u16(uint8_t *p, uint16_t v);
void     tdb_put_u32(uint8_t *p, uint32_t v);
void     tdb_put_u64(uint8_t *p, uint64_t v);
uint16_t tdb_get_u16(const uint8_t *p);
uint32_t tdb_get_u32(const uint8_t *p);
uint64_t tdb_get_u64(const uint8_t *p);

/* Checksums / hashing. */
uint32_t tdb_crc32c(uint32_t seed, const void *data, size_t n);
uint64_t tdb_fnv1a(const void *data, size_t n);

#endif /* TDB_UTIL_H */
