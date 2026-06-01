/* tdb_analyze.c — DDL AST to schema-object conversion. */
#include "tdb_analyze.h"
#include "../common/tdb_mem.h"

#include <string.h>

tdb_table *tdb_ast_to_table(const tdb_create_table *ct, char **err) {
  tdb_table *t = tdb_table_new(ct->name);
  if (!t) { if (err) *err = tdb_strdup("out of memory"); return NULL; }

  for (int i = 0; i < ct->ncol; i++) {
    const tdb_coldef *cd = &ct->cols[i];
    tdb_column c;
    tdb_column_init(&c, cd->name, cd->type);
    c.coll = cd->coll;
    c.notnull = cd->notnull;
    c.pk = cd->pk;
    c.unique = cd->unique;
    c.hidden = cd->hidden;
    c.generated = cd->generated;
    if (cd->generated_sql) c.generated_sql = tdb_strdup(cd->generated_sql);
    if (cd->default_sql) c.default_sql = tdb_strdup(cd->default_sql);
    if (cd->check_sql) c.check_sql = tdb_strdup(cd->check_sql);
    if (tdb_table_add_column(t, &c) != TDB_OK) {
      tdb_table_free(t);
      if (err) *err = tdb_strdup("out of memory");
      return NULL;
    }
  }

  /* table-level PRIMARY KEY (col, ...) */
  for (int i = 0; i < ct->npk; i++) {
    int ci = tdb_table_find_column(t, ct->pk_cols[i]);
    if (ci < 0) {
      tdb_table_free(t);
      if (err) *err = tdb_strdup("PRIMARY KEY names unknown column");
      return NULL;
    }
    t->cols[ci].pk = 1;
  }

  if (ct->check_sql) t->check_sql = tdb_strdup(ct->check_sql);
  t->columnar = ct->columnar;
  if (ct->tablespace) t->tablespace = tdb_strdup(ct->tablespace);
  if (ct->compression) t->compression = tdb_strdup(ct->compression);
  t->partition_kind = (int)ct->partition_kind;
  if (ct->npart_col > 0) {
    t->partition_cols = (char **)tdb_calloc(sizeof(char *) * (size_t)ct->npart_col);
    if (!t->partition_cols) { tdb_table_free(t); if (err) *err = tdb_strdup("out of memory"); return NULL; }
    for (int i = 0; i < ct->npart_col; i++) t->partition_cols[i] = tdb_strdup(ct->partition_cols[i]);
    t->npart_col = ct->npart_col;
  }
  /* Default partition count for HASH partitioning is 4 if unspecified. */
  if (ct->partition_kind == TDB_PART_HASH) {
    t->partition_count = ct->partition_count > 0 ? ct->partition_count : 4;
  } else if (ct->partition_kind != TDB_PART_NONE) {
    t->partition_count = ct->partition_count;
  }

  /* system-versioned temporal table: add hidden period bound columns */
  if (ct->system_versioning) {
    tdb_column s, e;
    tdb_column_init(&s, ct->period_start ? ct->period_start : "row_start",
                    tdb_typespec_parse("TIMESTAMP"));
    s.hidden = 1; s.notnull = 1;
    tdb_column_init(&e, ct->period_end ? ct->period_end : "row_end",
                    tdb_typespec_parse("TIMESTAMP"));
    e.hidden = 1; e.notnull = 1;
    t->row_start_col = t->ncol;
    tdb_table_add_column(t, &s);
    t->row_end_col = t->ncol;
    tdb_table_add_column(t, &e);
    t->versioning = TDB_VERSIONING_SYSTEM;
  }

  if (err) *err = NULL;
  return t;
}
