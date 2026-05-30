/* test_schema.c — schema object construction, calc columns, versioning flags. */
#include "tdb_test.h"
#include "../src/catalog/tdb_schema.h"
#include "../src/common/tdb_mem.h"

#include <string.h>

static void test_build_table(void) {
  tdb_table *t = tdb_table_new("orders");
  TDB_CHECK(t != NULL);

  tdb_column c;
  tdb_column_init(&c, "id", tdb_typespec_parse("INTEGER"));
  c.pk = 1; c.notnull = 1;
  tdb_table_add_column(t, &c);

  tdb_column_init(&c, "qty", tdb_typespec_parse("INTEGER"));
  tdb_table_add_column(t, &c);

  tdb_column_init(&c, "price", tdb_typespec_parse("DECIMAL(10,2)"));
  tdb_table_add_column(t, &c);

  /* a STORED calculated column referencing other columns */
  tdb_column_init(&c, "total", tdb_typespec_parse("DECIMAL(12,2)"));
  c.generated = TDB_GEN_STORED;
  c.generated_sql = tdb_strdup("qty * price");
  tdb_table_add_column(t, &c);

  TDB_CHECK_EQ(t->ncol, 4);
  TDB_CHECK_EQ(tdb_table_find_column(t, "price"), 2);
  TDB_CHECK_EQ(tdb_table_find_column(t, "TOTAL"), 3); /* case-insensitive */
  TDB_CHECK_EQ(tdb_table_find_column(t, "missing"), -1);
  TDB_CHECK_EQ(t->cols[3].generated, TDB_GEN_STORED);
  TDB_CHECK_STR(t->cols[3].generated_sql, "qty * price");
  TDB_CHECK_EQ(t->cols[0].pk, 1);

  tdb_table_free(t);
}

static void test_versioning_defaults(void) {
  tdb_table *t = tdb_table_new("audit");
  TDB_CHECK_EQ(t->versioning, TDB_VERSIONING_NONE);
  TDB_CHECK_EQ(t->row_start_col, -1);
  TDB_CHECK_EQ(t->row_end_col, -1);
  TDB_CHECK_EQ(t->schema_version, 0u);

  /* enable system versioning with hidden temporal bound columns */
  tdb_column c;
  tdb_column_init(&c, "valid_from", tdb_typespec_parse("TIMESTAMP"));
  c.hidden = 1; tdb_table_add_column(t, &c);
  tdb_column_init(&c, "valid_to", tdb_typespec_parse("TIMESTAMP"));
  c.hidden = 1; tdb_table_add_column(t, &c);
  t->versioning = TDB_VERSIONING_SYSTEM;
  t->row_start_col = 0;
  t->row_end_col = 1;

  TDB_CHECK_EQ(t->versioning, TDB_VERSIONING_SYSTEM);
  TDB_CHECK(t->cols[0].hidden && t->cols[1].hidden);
  tdb_table_free(t);
}

static tdb_test_case cases[] = {
  {"build_table", test_build_table},
  {"versioning_defaults", test_versioning_defaults},
};
TDB_MAIN(cases)
