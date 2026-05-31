/* tdb_schema.c — construction and teardown of schema objects. */
#include "tdb_schema.h"
#include "../common/tdb_mem.h"

#include <string.h>

void tdb_column_init(tdb_column *c, const char *name, tdb_typespec ts) {
  memset(c, 0, sizeof(*c));
  c->name = tdb_strdup(name);
  c->type = ts;
  c->coll = TDB_COLL_BINARY;
  c->generated = TDB_GEN_NONE;
}

static void colref_free(tdb_colref *r, int n) {
  for (int i = 0; i < n; i++) { tdb_mfree(r[i].table); tdb_mfree(r[i].column); }
  tdb_mfree(r);
}

void tdb_column_free(tdb_column *c) {
  if (!c) return;
  tdb_mfree(c->name);
  tdb_mfree(c->default_sql);
  tdb_mfree(c->check_sql);
  tdb_mfree(c->generated_sql);
  tdb_mfree(c->lua_compute);
  colref_free(c->refs, c->nref);
  memset(c, 0, sizeof(*c));
}

tdb_table *tdb_table_new(const char *name) {
  tdb_table *t = (tdb_table *)tdb_calloc(sizeof(*t));
  if (!t) return NULL;
  t->name = tdb_strdup(name);
  t->row_start_col = -1;
  t->row_end_col = -1;
  t->versioning = TDB_VERSIONING_NONE;
  return t;
}

int tdb_table_add_column(tdb_table *t, const tdb_column *col) {
  tdb_column *grown =
      (tdb_column *)tdb_realloc(t->cols, sizeof(tdb_column) * (size_t)(t->ncol + 1));
  if (!grown) return TDB_NOMEM;
  t->cols = grown;
  t->cols[t->ncol] = *col; /* take ownership of heap fields */
  t->ncol++;
  return TDB_OK;
}

int tdb_table_drop_column(tdb_table *t, int ci) {
  if (ci < 0 || ci >= t->ncol) return TDB_NOTFOUND;
  tdb_column_free(&t->cols[ci]);
  for (int i = ci; i < t->ncol - 1; i++) t->cols[i] = t->cols[i + 1];
  t->ncol--;
  /* shift hidden temporal period column indices that sat after the dropped one */
  if (t->row_start_col > ci) t->row_start_col--;
  if (t->row_end_col > ci) t->row_end_col--;
  /* shift index column references (the caller guarantees no index uses `ci`) */
  for (int i = 0; i < t->nindex; i++)
    for (int k = 0; k < t->indexes[i].ncol; k++)
      if (t->indexes[i].col_idx[k] > ci) t->indexes[i].col_idx[k]--;
  return TDB_OK;
}

int tdb_table_find_column(const tdb_table *t, const char *name) {
  for (int i = 0; i < t->ncol; i++)
    if (t->cols[i].name && strcasecmp(t->cols[i].name, name) == 0) return i;
  return -1;
}

static void index_free(tdb_index *ix) {
  tdb_mfree(ix->name);
  tdb_mfree(ix->col_idx);
  tdb_mfree(ix->desc);
}

static void fkey_free(tdb_fkey *fk) {
  tdb_mfree(fk->name);
  for (int i = 0; i < fk->ncol; i++) tdb_mfree(fk->cols[i]);
  tdb_mfree(fk->cols);
  tdb_mfree(fk->ref_table);
  if (fk->ref_cols) {
    for (int i = 0; i < fk->ncol; i++) tdb_mfree(fk->ref_cols[i]);
    tdb_mfree(fk->ref_cols);
  }
}

static void layout_free(tdb_collayout *l) {
  for (int i = 0; i < l->ncol; i++) tdb_mfree(l->names[i]);
  tdb_mfree(l->names);
  tdb_mfree(l->types);
}

void tdb_table_free(tdb_table *t) {
  if (!t) return;
  for (int i = 0; i < t->ncol; i++) tdb_column_free(&t->cols[i]);
  tdb_mfree(t->cols);
  for (int i = 0; i < t->nindex; i++) index_free(&t->indexes[i]);
  tdb_mfree(t->indexes);
  for (int i = 0; i < t->nfkey; i++) fkey_free(&t->fkeys[i]);
  tdb_mfree(t->fkeys);
  for (int i = 0; i < t->nhistory; i++) layout_free(&t->history[i]);
  tdb_mfree(t->history);
  tdb_mfree(t->col_roots);
  tdb_mfree(t->check_sql);
  tdb_mfree(t->before_insert_lua);
  tdb_mfree(t->before_update_lua);
  tdb_mfree(t->before_delete_lua);
  tdb_mfree(t->name);
  tdb_mfree(t);
}

void tdb_view_free(tdb_view *v) {
  if (!v) return;
  tdb_mfree(v->name);
  tdb_mfree(v->select_sql);
  tdb_mfree(v);
}

void tdb_routine_free(tdb_routine *r) {
  if (!r) return;
  tdb_mfree(r->name);
  tdb_mfree(r->lua_src);
  tdb_mfree(r);
}
