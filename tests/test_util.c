/* test_util.c — varints, big-endian integers, CRC32C. */
#include "tdb_test.h"
#include "../src/common/tdb_util.h"

static void test_varint_roundtrip(void) {
  uint64_t vals[] = {0, 1, 127, 128, 300, 16383, 16384,
                     0xFFFFFFFFull, 0x123456789ABCull, ~0ull};
  for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
    uint8_t buf[10];
    int n = tdb_put_varint(buf, vals[i]);
    TDB_CHECK(n == tdb_varint_len(vals[i]));
    uint64_t out = 0;
    int m = tdb_get_varint(buf, (size_t)n, &out);
    TDB_CHECK_EQ(m, n);
    TDB_CHECK(out == vals[i]);
  }
}

static void test_varint_truncated(void) {
  uint8_t buf[10];
  int n = tdb_put_varint(buf, 300);
  uint64_t out = 0;
  /* not enough bytes available -> failure */
  TDB_CHECK_EQ(tdb_get_varint(buf, (size_t)(n - 1), &out), 0);
}

static void test_be_integers(void) {
  uint8_t b[8];
  tdb_put_u16(b, 0xABCD);
  TDB_CHECK_EQ(tdb_get_u16(b), 0xABCD);
  tdb_put_u32(b, 0x01020304u);
  TDB_CHECK_EQ(tdb_get_u32(b), 0x01020304u);
  TDB_CHECK_EQ(b[0], 0x01); /* big-endian: MSB first */
  tdb_put_u64(b, 0x0102030405060708ull);
  TDB_CHECK(tdb_get_u64(b) == 0x0102030405060708ull);
}

static void test_crc32c(void) {
  /* CRC32C of "123456789" is the standard check value 0xE3069283. */
  uint32_t c = tdb_crc32c(0, "123456789", 9);
  TDB_CHECK_EQ(c, 0xE3069283u);
  /* empty input with seed 0 -> 0 */
  TDB_CHECK_EQ(tdb_crc32c(0, "", 0), 0u);
}

static tdb_test_case cases[] = {
  {"varint_roundtrip", test_varint_roundtrip},
  {"varint_truncated", test_varint_truncated},
  {"be_integers", test_be_integers},
  {"crc32c", test_crc32c},
};
TDB_MAIN(cases)
