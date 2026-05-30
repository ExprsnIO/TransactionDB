/* test_sqltype.c — strict SQL type parsing and coercion. */
#include "tdb_test.h"
#include "../src/value/tdb_sqltype.h"

#include <string.h>

static void test_parse(void) {
  tdb_typespec a = tdb_typespec_parse("BIGINT");
  TDB_CHECK_EQ(a.id, TDB_T_BIGINT);

  tdb_typespec b = tdb_typespec_parse("VARCHAR(80)");
  TDB_CHECK_EQ(b.id, TDB_T_VARCHAR);
  TDB_CHECK_EQ(b.length, 80);

  tdb_typespec c = tdb_typespec_parse("DECIMAL(10,2)");
  TDB_CHECK_EQ(c.id, TDB_T_DECIMAL);
  TDB_CHECK_EQ(c.precision, 10);
  TDB_CHECK_EQ(c.scale, 2);

  tdb_typespec d = tdb_typespec_parse("timestamp");
  TDB_CHECK_EQ(d.id, TDB_T_TIMESTAMP);

  /* cross-dialect names */
  TDB_CHECK_EQ(tdb_typespec_parse("BYTEA").id, TDB_T_VARBINARY);
  TDB_CHECK_EQ(tdb_typespec_parse("NUMBER(5)").id, TDB_T_DECIMAL);
  TDB_CHECK_EQ(tdb_typespec_parse("BOOL").id, TDB_T_BOOLEAN);
}

static void test_int_strict(void) {
  tdb_typespec ti = tdb_typespec_parse("SMALLINT");
  const char *why = NULL;
  tdb_value v; tdb_value_init(&v);

  tdb_value_set_int(&v, 1000);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_OK);

  tdb_value_set_int(&v, 100000);  /* out of SMALLINT range */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_CONSTRAINT);

  tdb_value_set_text(&v, "42", -1, 1);  /* lossless text -> int */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_OK);
  TDB_CHECK_EQ(v.type, TDB_VAL_INT);
  TDB_CHECK_EQ(tdb_value_as_int(&v), 42);

  tdb_value_set_text(&v, "hello", -1, 1);  /* not an int */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_MISMATCH);

  tdb_value_set_real(&v, 3.5);  /* fractional real -> int rejected */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_MISMATCH);

  tdb_value_clear(&v);
}

static void test_decimal_strict(void) {
  tdb_typespec td = tdb_typespec_parse("DECIMAL(5,2)");
  const char *why = NULL;
  tdb_value v; tdb_value_init(&v);

  tdb_value_set_text(&v, "123.45", -1, 1);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &td, &why), TDB_OK);

  tdb_value_set_text(&v, "123.456", -1, 1);  /* too many fractional digits */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &td, &why), TDB_CONSTRAINT);

  tdb_value_set_text(&v, "1234567", -1, 1);  /* exceeds precision */
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &td, &why), TDB_CONSTRAINT);

  tdb_value_clear(&v);
}

static void test_varchar_and_bool(void) {
  const char *why = NULL;
  tdb_value v; tdb_value_init(&v);

  tdb_typespec tv = tdb_typespec_parse("VARCHAR(3)");
  tdb_value_set_text(&v, "abc", -1, 1);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &tv, &why), TDB_OK);
  tdb_value_set_text(&v, "abcd", -1, 1);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &tv, &why), TDB_CONSTRAINT);

  tdb_typespec tb = tdb_typespec_parse("BOOLEAN");
  tdb_value_set_text(&v, "true", -1, 1);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &tb, &why), TDB_OK);
  TDB_CHECK_EQ(tdb_value_as_int(&v), 1);
  tdb_value_set_int(&v, 0);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &tb, &why), TDB_OK);

  tdb_value_clear(&v);
}

static void test_null_passes(void) {
  tdb_typespec ti = tdb_typespec_parse("INTEGER");
  const char *why = NULL;
  tdb_value v; tdb_value_init(&v);
  tdb_value_set_null(&v);
  TDB_CHECK_EQ(tdb_typespec_coerce(&v, &ti, &why), TDB_OK);
  tdb_value_clear(&v);
}

static tdb_test_case cases[] = {
  {"parse", test_parse},
  {"int_strict", test_int_strict},
  {"decimal_strict", test_decimal_strict},
  {"varchar_and_bool", test_varchar_and_bool},
  {"null_passes", test_null_passes},
};
TDB_MAIN(cases)
