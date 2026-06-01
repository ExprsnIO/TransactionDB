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
  {"sha256", test_sha256},
  {"md5", test_md5},
  {"hmac", test_hmac},
  {"crc32", test_crc32},
  {"hex_roundtrip", test_hex_roundtrip},
  {"create_function", test_create_function},
  {"load_extension", test_load_extension},
};
TDB_MAIN(cases)
