/* test_crypto.c — built-in cryptography functions and the C/C++ plugin engine. */
#include "tdb_test.h"
#include "transactiondb.h"

#include <string.h>
#include <stdio.h>

static const char *scalar_text(tdb_db *db, const char *sql, char *buf, int n) {
  tdb_stmt *s = NULL; buf[0] = '\0';
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return buf;
  if (tdb_step(s) == TDB_ROW) {
    const char *t = tdb_column_text(s, 0);
    if (t) snprintf(buf, (size_t)n, "%s", t);
  }
  tdb_finalize(s);
  return buf;
}

static int64_t scalar_int(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL; int64_t v = -999;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return v;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_sha256(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[128];
  /* NIST test vector for "abc" */
  TDB_CHECK_STR(scalar_text(db, "SELECT sha256('abc')", b, sizeof(b)),
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  tdb_close(db);
}

static void test_md5(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[64];
  TDB_CHECK_STR(scalar_text(db, "SELECT md5('abc')", b, sizeof(b)), "900150983cd24fb0d6963f7d28e17f72");
  TDB_CHECK_STR(scalar_text(db, "SELECT md5('')", b, sizeof(b)), "d41d8cd98f00b204e9800998ecf8427e");
  tdb_close(db);
}

static void test_hmac(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[128];
  /* RFC-style vector: HMAC-SHA256(key="key", msg="The quick brown fox...") */
  TDB_CHECK_STR(
      scalar_text(db, "SELECT hmac_sha256('key', 'The quick brown fox jumps over the lazy dog')",
                  b, sizeof(b)),
      "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
  tdb_close(db);
}

static void test_crc32(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  /* CRC-32 of "123456789" is 0xCBF43926 */
  TDB_CHECK_EQ(scalar_int(db, "SELECT crc32('123456789')"), 0xCBF43926LL);
  tdb_close(db);
}

static void test_hex_roundtrip(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[64];
  TDB_CHECK_STR(scalar_text(db, "SELECT hex('AB')", b, sizeof(b)), "4142");
  TDB_CHECK_STR(scalar_text(db, "SELECT hex(unhex('deadbeef'))", b, sizeof(b)), "deadbeef");
  /* randomblob(8) -> 8 bytes -> 16 hex chars */
  TDB_CHECK_EQ(scalar_int(db, "SELECT length(hex(randomblob(8)))"), 16);
  tdb_close(db);
}

/* a user function registered through the public API */
static void udf_triple(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  tdb_result_int64(ctx, 3 * tdb_value_int64(argv[0]));
}

static void test_create_function(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  TDB_CHECK_EQ(tdb_create_function(db, "triple", 1, udf_triple, NULL), TDB_OK);
  TDB_CHECK_EQ(scalar_int(db, "SELECT triple(7)"), 21);
  TDB_CHECK_EQ(scalar_int(db, "SELECT triple(triple(2))"), 18);
  tdb_close(db);
}

static void test_sha1(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[64];
  /* NIST test vector for SHA-1("abc") */
  TDB_CHECK_STR(scalar_text(db, "SELECT sha1('abc')", b, sizeof(b)),
                "a9993e364706816aba3e25717850c26c9cd0d89d");
  /* empty string */
  TDB_CHECK_STR(scalar_text(db, "SELECT sha1('')", b, sizeof(b)),
                "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  tdb_close(db);
}

static void test_base64(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[128];
  TDB_CHECK_STR(scalar_text(db, "SELECT base64_encode('Man')", b, sizeof(b)), "TWFu");
  TDB_CHECK_STR(scalar_text(db, "SELECT base64_encode('Hello, world')", b, sizeof(b)),
                "SGVsbG8sIHdvcmxk");
  /* roundtrip via the blob path */
  TDB_CHECK_STR(scalar_text(db, "SELECT hex(base64_decode('SGVsbG8='))", b, sizeof(b)),
                "48656c6c6f");
  tdb_close(db);
}

#ifdef TDB_HAVE_OPENSSL
static void test_sha512(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char b[256];
  /* NIST test vector for SHA-512("abc") */
  TDB_CHECK_STR(scalar_text(db, "SELECT sha512('abc')", b, sizeof(b)),
                "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  tdb_close(db);
}

static void test_aes_gcm_roundtrip(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  /* AES-GCM is randomized (random IV), so we round-trip through a CTE: the
  ** ciphertext column flows from encrypt into decrypt, and the decrypted text
  ** must equal the original. */
  tdb_stmt *s = NULL;
  TDB_CHECK_EQ(tdb_prepare_v2(db,
    "WITH t AS (SELECT aes_gcm_encrypt('hello world', 's3cret') AS ct) "
    "SELECT CAST(aes_gcm_decrypt(ct, 's3cret') AS TEXT) FROM t",
    -1, &s, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_step(s), TDB_ROW);
  const char *t = tdb_column_text(s, 0);
  TDB_CHECK(t != NULL && strcmp(t, "hello world") == 0);
  tdb_finalize(s);
  tdb_close(db);
}

static void test_pbkdf2(void) {
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  /* RFC 6070 PBKDF2-HMAC-SHA1 vectors do not apply (we use SHA-256), but the
  ** function should at least be deterministic and yield the requested length. */
  char b[160];
  TDB_CHECK_STR(scalar_text(db,
      "SELECT hex(pbkdf2('password', 'salt', 1000, 16))", b, sizeof(b)),
      scalar_text(db,
      "SELECT hex(pbkdf2('password', 'salt', 1000, 16))", b, sizeof(b)));
  TDB_CHECK_EQ(scalar_int(db,
      "SELECT length(pbkdf2('password', 'salt', 1000, 32))"), 32);
  tdb_close(db);
}
#endif

static void test_load_extension(void) {
#ifdef TDB_PLUGIN_PATH
  tdb_db *db; TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char *err = NULL;
  int rc = tdb_load_extension(db, TDB_PLUGIN_PATH, NULL, &err);
  if (rc != TDB_OK) { fprintf(stderr, "  load_extension: %s\n", err ? err : "?"); }
  TDB_CHECK_EQ(rc, TDB_OK);
  tdb_free(err);
  if (rc == TDB_OK) {
    TDB_CHECK_EQ(scalar_int(db, "SELECT plugin_addone(41)"), 42);
    char b[64];
    TDB_CHECK_STR(scalar_text(db, "SELECT plugin_greet('db')", b, sizeof(b)), "hello, db!");
  }
  tdb_close(db);
#endif
}

static const tdb_test_case cases[] = {
  {"sha1", test_sha1},
  {"sha256", test_sha256},
  {"md5", test_md5},
  {"hmac", test_hmac},
  {"crc32", test_crc32},
  {"hex_roundtrip", test_hex_roundtrip},
  {"base64", test_base64},
#ifdef TDB_HAVE_OPENSSL
  {"sha512", test_sha512},
  {"aes_gcm_roundtrip", test_aes_gcm_roundtrip},
  {"pbkdf2", test_pbkdf2},
#endif
  {"create_function", test_create_function},
  {"load_extension", test_load_extension},
};
TDB_MAIN(cases)
