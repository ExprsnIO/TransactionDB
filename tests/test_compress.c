/* test_compress.c — page/blob compression codec.
**
** The wrapper compiles in either passthrough (TDB_COMP_NONE) or zstd
** (TDB_COMP_ZSTD) mode. We can only assert codec-agnostic invariants:
**   - the codec id matches the build, and
**   - round-tripping data through compress/decompress recovers the input.
*/
#include "tdb_test.h"
#include "../src/storage/tdb_compress.h"

#include <string.h>

static void test_roundtrip(void) {
  const char *msg = "The quick brown fox jumps over the lazy dog. "
                    "The quick brown fox jumps over the lazy dog. "
                    "The quick brown fox jumps over the lazy dog.";
  size_t n = strlen(msg);

  size_t cap = tdb_compress_bound(n);
  TDB_CHECK(cap >= n);

  char *enc = (char *)malloc(cap);
  TDB_CHECK(enc != NULL);
  size_t enc_n = cap;
  TDB_CHECK_EQ(tdb_compress(msg, n, enc, &enc_n), TDB_OK);
  TDB_CHECK(enc_n > 0);

  char *dec = (char *)malloc(n + 16);
  TDB_CHECK(dec != NULL);
  size_t dec_n = n + 16;
  TDB_CHECK_EQ(tdb_decompress(enc, enc_n, dec, &dec_n), TDB_OK);
  TDB_CHECK_EQ((int)dec_n, (int)n);
  TDB_CHECK(memcmp(msg, dec, n) == 0);

  free(enc); free(dec);
}

static void test_codec_id(void) {
  int id = tdb_compress_codec();
  TDB_CHECK(id == TDB_COMP_NONE || id == TDB_COMP_ZSTD);
  const char *name = tdb_compress_codec_name();
  TDB_CHECK(name != NULL);
  TDB_CHECK(strcmp(name, "none") == 0 || strcmp(name, "zstd") == 0);
}

static tdb_test_case cases[] = {
  {"roundtrip", test_roundtrip},
  {"codec_id", test_codec_id},
};
TDB_MAIN(cases)
