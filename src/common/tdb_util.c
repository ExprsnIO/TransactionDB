/* tdb_util.c — varints, big-endian integers, CRC32C, FNV-1a. */
#include "tdb_util.h"

/* ----------------------------- varints ------------------------------- */

int tdb_varint_len(uint64_t v) {
  int n = 1;
  while (v >= 0x80) { v >>= 7; n++; }
  return n;
}

int tdb_put_varint(uint8_t *p, uint64_t v) {
  int i = 0;
  while (v >= 0x80) {
    p[i++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  p[i++] = (uint8_t)v;
  return i;
}

int tdb_get_varint(const uint8_t *p, size_t avail, uint64_t *v) {
  uint64_t result = 0;
  int shift = 0;
  size_t i = 0;
  while (i < avail && i < 10) {
    uint8_t b = p[i++];
    result |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      *v = result;
      return (int)i;
    }
    shift += 7;
  }
  return 0; /* truncated / malformed */
}

/* ------------------------ big-endian integers ------------------------ */

void tdb_put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
void tdb_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
void tdb_put_u64(uint8_t *p, uint64_t v) {
  tdb_put_u32(p, (uint32_t)(v >> 32));
  tdb_put_u32(p + 4, (uint32_t)v);
}
uint16_t tdb_get_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}
uint32_t tdb_get_u32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
uint64_t tdb_get_u64(const uint8_t *p) {
  return (uint64_t)tdb_get_u32(p) << 32 | (uint64_t)tdb_get_u32(p + 4);
}

/* ------------------------------- CRC32C ------------------------------ */
/* Software CRC32C (Castagnoli, poly 0x1EDC6F41 reflected = 0x82F63B78). */

static uint32_t tdb__crc32c_table[256];
static int tdb__crc32c_ready = 0;

static void tdb_crc32c_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int k = 0; k < 8; k++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
    tdb__crc32c_table[i] = crc;
  }
  tdb__crc32c_ready = 1;
}

uint32_t tdb_crc32c(uint32_t seed, const void *data, size_t n) {
  if (!tdb__crc32c_ready) tdb_crc32c_init();
  uint32_t crc = ~seed;
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n; i++) {
    crc = tdb__crc32c_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

/* ------------------------------- FNV-1a ------------------------------ */

uint64_t tdb_fnv1a(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}
