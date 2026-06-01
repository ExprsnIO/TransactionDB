/* test_composite.c — composite (ROW/STRUCT) value type and storage. */
#include "tdb_test.h"
#include "tdb_value.h"
#include "tdb_record.h"
#include "tdb_sqltype.h"
#include "tdb_buf.h"
#include "transactiondb.h"

#include <string.h>

static void test_pack_count_field(void) {
  tdb_value f[3];
  tdb_value_init(&f[0]); tdb_value_set_int(&f[0], 42);
  tdb_value_init(&f[1]); tdb_value_set_real(&f[1], 2.5);
  tdb_value_init(&f[2]); tdb_value_set_text(&f[2], "hi", -1, 1);

  tdb_value c; tdb_value_init(&c);
  TDB_CHECK_EQ(tdb_value_composite_pack(&c, f, 3), TDB_OK);
  TDB_CHECK_EQ(tdb_value_composite_count(&c), 3);

  tdb_value g; tdb_value_init(&g);
  TDB_CHECK_EQ(tdb_value_composite_field(&c, 0, &g), TDB_OK);
  TDB_CHECK_EQ(tdb_value_as_int(&g), 42);
  TDB_CHECK_EQ(tdb_value_composite_field(&c, 2, &g), TDB_OK);
  TDB_CHECK_STR(tdb_value_as_text(&g), "hi");
  TDB_CHECK(tdb_value_composite_field(&c, 5, &g) == TDB_RANGE);

  tdb_value_clear(&g); tdb_value_clear(&c);
  for (int i = 0; i < 3; i++) tdb_value_clear(&f[i]);
}

static void test_as_text(void) {
  tdb_value f[3];
  tdb_value_init(&f[0]); tdb_value_set_int(&f[0], 1);
  tdb_value_init(&f[1]); tdb_value_set_int(&f[1], 2);
  tdb_value_init(&f[2]); tdb_value_set_text(&f[2], "x", -1, 1);
  tdb_value c; tdb_value_init(&c);
  tdb_value_composite_pack(&c, f, 3);
  TDB_CHECK_STR(tdb_value_as_text(&c), "(1, 2, x)");
  tdb_value_clear(&c);
  for (int i = 0; i < 3; i++) tdb_value_clear(&f[i]);
}

static void test_nested(void) {
  tdb_value inner[2];
  tdb_value_init(&inner[0]); tdb_value_set_int(&inner[0], 7);
  tdb_value_init(&inner[1]); tdb_value_set_int(&inner[1], 8);
  tdb_value ci; tdb_value_init(&ci);
  tdb_value_composite_pack(&ci, inner, 2);

  tdb_value outer[2];
  tdb_value_init(&outer[0]); tdb_value_set_text(&outer[0], "k", -1, 1);
  outer[1] = ci;  /* move the composite in */
  tdb_value co; tdb_value_init(&co);
  tdb_value_composite_pack(&co, outer, 2);
  TDB_CHECK_EQ(tdb_value_composite_count(&co), 2);

  tdb_value got; tdb_value_init(&got);
  TDB_CHECK_EQ(tdb_value_composite_field(&co, 1, &got), TDB_OK);
  /* field 1 decodes as a blob; re-tag as composite and inspect */
  got.type = TDB_VAL_COMPOSITE;
  TDB_CHECK_EQ(tdb_value_composite_count(&got), 2);
  tdb_value g2; tdb_value_init(&g2);
  tdb_value_composite_field(&got, 0, &g2);
  TDB_CHECK_EQ(tdb_value_as_int(&g2), 7);

  tdb_value_clear(&g2); tdb_value_clear(&got);
  tdb_value_clear(&co);
  tdb_value_clear(&outer[0]); tdb_value_clear(&outer[1]);
  tdb_value_clear(&inner[0]); tdb_value_clear(&inner[1]);
}

static void test_record_roundtrip(void) {
  /* a row {int, composite, text}: the composite survives record encode/decode
  ** (as a blob) and is re-tagged via a COMPOSITE typespec coercion */
  tdb_value fields[2];
  tdb_value_init(&fields[0]); tdb_value_set_int(&fields[0], 100);
  tdb_value_init(&fields[1]); tdb_value_set_text(&fields[1], "abc", -1, 1);
  tdb_value comp; tdb_value_init(&comp);
  tdb_value_composite_pack(&comp, fields, 2);

  tdb_value row[3];
  tdb_value_init(&row[0]); tdb_value_set_int(&row[0], 1);
  row[1] = comp;
  tdb_value_init(&row[2]); tdb_value_set_text(&row[2], "end", -1, 1);

  tdb_buf b; tdb_buf_init(&b);
  TDB_CHECK_EQ(tdb_record_encode(row, 3, &b), TDB_OK);

  tdb_value out[3]; int nc = 0;
  TDB_CHECK_EQ(tdb_record_decode(b.data, b.len, out, 3, &nc), TDB_OK);
  TDB_CHECK_EQ(nc, 3);
  /* out[1] came back as a blob; coerce to composite via the type system */
  tdb_typespec ts; ts.id = TDB_T_COMPOSITE; ts.length = ts.precision = ts.scale = 0;
  const char *why = NULL;
  TDB_CHECK_EQ(tdb_typespec_coerce(&out[1], &ts, &why), TDB_OK);
  TDB_CHECK_EQ(tdb_value_composite_count(&out[1]), 2);
  tdb_value g; tdb_value_init(&g);
  tdb_value_composite_field(&out[1], 1, &g);
  TDB_CHECK_STR(tdb_value_as_text(&g), "abc");

  tdb_value_clear(&g);
  for (int i = 0; i < 3; i++) tdb_value_clear(&out[i]);
  tdb_buf_free(&b);
  tdb_value_clear(&row[0]); tdb_value_clear(&row[1]); tdb_value_clear(&row[2]);
  tdb_value_clear(&fields[0]); tdb_value_clear(&fields[1]);
}

static const char *scalar_text(tdb_db *db, const char *sql, char *buf, int n) {
  tdb_stmt *s = NULL;
  buf[0] = '\0';
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return buf;
  if (tdb_step(s) == TDB_ROW) {
    const char *t = tdb_column_text(s, 0);
    if (t) snprintf(buf, (size_t)n, "%s", t);
  }
  tdb_finalize(s);
  return buf;
}

static int64_t scalar_int(tdb_db *db, const char *sql) {
  tdb_stmt *s = NULL;
  int64_t v = -999;
  if (tdb_prepare_v2(db, sql, -1, &s, NULL) != TDB_OK) return v;
  if (tdb_step(s) == TDB_ROW) v = tdb_column_int64(s, 0);
  tdb_finalize(s);
  return v;
}

static void test_sql_functions(void) {
  tdb_db *db = NULL;
  TDB_CHECK_EQ(tdb_open(":memory:", &db), TDB_OK);
  char buf[64];
  TDB_CHECK_STR(scalar_text(db, "SELECT row(1, 2, 3)", buf, sizeof(buf)), "(1, 2, 3)");
  TDB_CHECK_EQ(scalar_int(db, "SELECT composite_extract(row(10, 20, 30), 2)"), 20);
  TDB_CHECK_EQ(scalar_int(db, "SELECT composite_count(row('a','b','c','d'))"), 4);
  TDB_CHECK_STR(scalar_text(db, "SELECT typeof(row(1))", buf, sizeof(buf)), "composite");
  tdb_close(db);
}

static const tdb_test_case cases[] = {
  {"pack_count_field", test_pack_count_field},
  {"as_text", test_as_text},
  {"nested", test_nested},
  {"record_roundtrip", test_record_roundtrip},
  {"sql_functions", test_sql_functions},
};
TDB_MAIN(cases)
