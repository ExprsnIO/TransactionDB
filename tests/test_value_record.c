/* test_value_record.c — value semantics and record codec round-trips. */
#include "tdb_test.h"
#include "../src/value/tdb_record.h"
#include "../src/value/tdb_type.h"

#include <string.h>

static void test_value_basics(void) {
  tdb_value v;
  tdb_value_init(&v);
  TDB_CHECK_EQ(v.type, TDB_VAL_NULL);
  tdb_value_set_int(&v, 42);
  TDB_CHECK_EQ(tdb_value_as_int(&v), 42);
  TDB_CHECK(tdb_value_as_real(&v) == 42.0);
  tdb_value_set_text(&v, "hello", -1, 1);
  TDB_CHECK_STR(tdb_value_as_text(&v), "hello");
  TDB_CHECK_EQ(v.u.s.n, 5);
  tdb_value_clear(&v);
}

static void test_value_compare(void) {
  tdb_value a, b;
  tdb_value_init(&a); tdb_value_init(&b);
  tdb_value_set_int(&a, 1); tdb_value_set_int(&b, 2);
  TDB_CHECK(tdb_value_compare(&a, &b, TDB_COLL_BINARY) < 0);
  tdb_value_set_text(&a, "ABC", -1, 1); tdb_value_set_text(&b, "abc", -1, 1);
  TDB_CHECK(tdb_value_compare(&a, &b, TDB_COLL_BINARY) < 0);
  TDB_CHECK(tdb_value_compare(&a, &b, TDB_COLL_NOCASE) == 0);
  /* NULL sorts first */
  tdb_value_set_null(&a); tdb_value_set_int(&b, -5);
  TDB_CHECK(tdb_value_compare(&a, &b, TDB_COLL_BINARY) < 0);
  tdb_value_clear(&a); tdb_value_clear(&b);
}

static void test_affinity(void) {
  TDB_CHECK_EQ(tdb_affinity_from_decltype("INTEGER"), TDB_AFF_INTEGER);
  TDB_CHECK_EQ(tdb_affinity_from_decltype("VARCHAR(20)"), TDB_AFF_TEXT);
  TDB_CHECK_EQ(tdb_affinity_from_decltype("DOUBLE"), TDB_AFF_REAL);
  TDB_CHECK_EQ(tdb_affinity_from_decltype("BLOB"), TDB_AFF_BLOB);
  TDB_CHECK_EQ(tdb_affinity_from_decltype("DECIMAL"), TDB_AFF_NUMERIC);

  tdb_value v; tdb_value_init(&v);
  tdb_value_set_text(&v, "123", -1, 1);
  tdb_apply_affinity(&v, TDB_AFF_INTEGER);
  TDB_CHECK_EQ(v.type, TDB_VAL_INT);
  TDB_CHECK_EQ(tdb_value_as_int(&v), 123);
  tdb_value_clear(&v);
}

static void test_record_roundtrip(void) {
  tdb_value in[5];
  for (int i = 0; i < 5; i++) tdb_value_init(&in[i]);
  tdb_value_set_int(&in[0], -1000000);
  tdb_value_set_real(&in[1], 3.14159);
  tdb_value_set_text(&in[2], "transaction", -1, 1);
  tdb_value_set_null(&in[3]);
  tdb_value_set_int(&in[4], 1);

  tdb_buf buf; tdb_buf_init(&buf);
  TDB_CHECK_EQ(tdb_record_encode(in, 5, &buf), TDB_OK);

  tdb_value out[5];
  int ncols = 0;
  TDB_CHECK_EQ(tdb_record_decode(buf.data, buf.len, out, 5, &ncols), TDB_OK);
  TDB_CHECK_EQ(ncols, 5);
  TDB_CHECK_EQ(tdb_value_as_int(&out[0]), -1000000);
  TDB_CHECK(tdb_value_as_real(&out[1]) > 3.14 && tdb_value_as_real(&out[1]) < 3.15);
  TDB_CHECK_STR(tdb_value_as_text(&out[2]), "transaction");
  TDB_CHECK_EQ(out[3].type, TDB_VAL_NULL);
  TDB_CHECK_EQ(tdb_value_as_int(&out[4]), 1);

  for (int i = 0; i < 5; i++) { tdb_value_clear(&in[i]); tdb_value_clear(&out[i]); }
  tdb_buf_free(&buf);
}

static void test_record_compare(void) {
  tdb_value a[2], b[2];
  for (int i = 0; i < 2; i++) { tdb_value_init(&a[i]); tdb_value_init(&b[i]); }
  tdb_value_set_int(&a[0], 1); tdb_value_set_text(&a[1], "x", -1, 1);
  tdb_value_set_int(&b[0], 1); tdb_value_set_text(&b[1], "y", -1, 1);
  tdb_buf ba, bb; tdb_buf_init(&ba); tdb_buf_init(&bb);
  tdb_record_encode(a, 2, &ba);
  tdb_record_encode(b, 2, &bb);
  TDB_CHECK(tdb_record_compare(ba.data, ba.len, bb.data, bb.len, NULL) < 0);
  TDB_CHECK(tdb_record_compare(ba.data, ba.len, ba.data, ba.len, NULL) == 0);
  for (int i = 0; i < 2; i++) { tdb_value_clear(&a[i]); tdb_value_clear(&b[i]); }
  tdb_buf_free(&ba); tdb_buf_free(&bb);
}

static tdb_test_case cases[] = {
  {"value_basics", test_value_basics},
  {"value_compare", test_value_compare},
  {"affinity", test_affinity},
  {"record_roundtrip", test_record_roundtrip},
  {"record_compare", test_record_compare},
};
TDB_MAIN(cases)
