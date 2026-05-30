/* test_catalog.c — schema serialization survives a database reopen. */
#include "tdb_test.h"
#include "../src/catalog/tdb_catalog.h"
#include "../src/storage/tdb_storage.h"
#include "../src/txn/tdb_txn.h"
#include "../src/txn/tdb_lock.h"
#include "../src/common/tdb_mem.h"

#include <stdio.h>
#include <string.h>

static void test_persist_schema(void) {
  const char *path = "test_catalog.db";
  remove(path); remove("test_catalog.db-wal");

  /* --- create a rich table and persist it --- */
  {
    tdb_pager *p; tdb_pager_open(NULL, path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE, &p);
    tdb_catalog *cat; tdb_catalog_open(p, &cat);
    tdb_lockmgr *lm = tdb_lockmgr_new();
    tdb_txnmgr *tm; tdb_txnmgr_open(p, lm, &tm);
    tdb_storage *s; tdb_engine_row_open(p, &s);

    tdb_table *t = tdb_table_new("invoices");
    tdb_column c;
    tdb_column_init(&c, "id", tdb_typespec_parse("BIGINT")); c.pk = 1; c.notnull = 1;
    tdb_table_add_column(t, &c);
    tdb_column_init(&c, "amount", tdb_typespec_parse("DECIMAL(12,2)"));
    tdb_table_add_column(t, &c);
    tdb_column_init(&c, "tax", tdb_typespec_parse("DECIMAL(12,2)"));
    c.generated = TDB_GEN_STORED;
    c.generated_sql = tdb_strdup("amount * 0.2");
    tdb_table_add_column(t, &c);
    t->versioning = TDB_VERSIONING_SYSTEM;
    t->row_start_col = 0;
    t->row_end_col = 1;

    /* add a secondary index on amount */
    tdb_txn *d; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 1, &d);
    s->vtab->create_table(s, d, t);
    /* persist the table FIRST (no indexes yet) ... */
    tdb_catalog_add_table(cat, t);
    /* ... then add an index and rewrite the catalog row via update_table */
    tdb_index ix; memset(&ix, 0, sizeof(ix));
    ix.name = tdb_strdup("idx_amount");
    ix.ncol = 1;
    ix.col_idx = (int *)tdb_malloc(sizeof(int)); ix.col_idx[0] = 1;
    s->vtab->create_index(s, d, t, &ix);
    tdb_index *grown = (tdb_index *)tdb_realloc(t->indexes, sizeof(tdb_index));
    t->indexes = grown; t->indexes[0] = ix; t->nindex = 1;
    tdb_catalog_update_table(cat, t);
    tdb_txn_commit(d);

    tdb_storage_close(s);
    tdb_txnmgr_close(tm);
    tdb_lockmgr_free(lm);
    tdb_catalog_close(cat);
    tdb_pager_close(p);
  }

  /* --- reopen and verify everything round-tripped --- */
  {
    tdb_pager *p; tdb_pager_open(NULL, path, TDB_OPEN_READWRITE, &p);
    tdb_catalog *cat; tdb_catalog_open(p, &cat);

    TDB_CHECK_EQ(tdb_catalog_table_count(cat), 1);
    tdb_table *t = tdb_catalog_find_table(cat, "invoices");
    TDB_CHECK(t != NULL);
    TDB_CHECK_EQ(t->ncol, 3);
    TDB_CHECK_STR(t->cols[0].name, "id");
    TDB_CHECK_EQ(t->cols[0].type.id, TDB_T_BIGINT);
    TDB_CHECK_EQ(t->cols[0].pk, 1);
    TDB_CHECK_EQ(t->cols[1].type.id, TDB_T_DECIMAL);
    TDB_CHECK_EQ(t->cols[1].type.precision, 12);
    TDB_CHECK_EQ(t->cols[1].type.scale, 2);
    TDB_CHECK_EQ(t->cols[2].generated, TDB_GEN_STORED);
    TDB_CHECK_STR(t->cols[2].generated_sql, "amount * 0.2");
    TDB_CHECK_EQ(t->versioning, TDB_VERSIONING_SYSTEM);
    TDB_CHECK_EQ(t->row_start_col, 0);
    TDB_CHECK_EQ(t->row_end_col, 1);
    TDB_CHECK_EQ(t->nindex, 1);
    TDB_CHECK_STR(t->indexes[0].name, "idx_amount");
    TDB_CHECK_EQ(t->indexes[0].col_idx[0], 1);

    tdb_catalog_close(cat);
    tdb_pager_close(p);
  }
  remove(path); remove("test_catalog.db-wal");
}

static tdb_test_case cases[] = {
  {"persist_schema", test_persist_schema},
};
TDB_MAIN(cases)
