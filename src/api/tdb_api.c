/*
** tdb_api.c — implementation of the public C API (transactiondb.h).
**
** NOTE: This is the Phase-0 skeleton. The connection currently holds only an
** error buffer; storage, catalog, transaction and SQL execution machinery are
** wired in over subsequent phases. Functions that depend on not-yet-built
** layers return TDB_UNSUPPORTED so the surface is complete and link-clean.
*/
#include "transactiondb.h"
#include "../common/tdb_mem.h"
#include "../value/tdb_value.h"

#include <string.h>
#include <stdio.h>

struct tdb_db {
  char    errmsg[256];
  int     flags;
  char   *path;
};

static void set_err(tdb_db *db, const char *msg) {
  if (!db) return;
  snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg ? msg : "");
}

int tdb_open(const char *filename, tdb_db **ppDb) {
  return tdb_open_v2(filename, ppDb,
                     TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
}

int tdb_open_v2(const char *filename, tdb_db **ppDb, int flags) {
  if (!ppDb) return TDB_MISUSE;
  *ppDb = NULL;
  tdb_db *db = (tdb_db *)tdb_calloc(sizeof(*db));
  if (!db) return TDB_NOMEM;
  db->flags = flags;
  db->path = filename ? tdb_strdup(filename) : NULL;
  db->errmsg[0] = '\0';
  *ppDb = db;
  return TDB_OK;
}

int tdb_close(tdb_db *db) {
  if (!db) return TDB_OK;
  tdb_mfree(db->path);
  tdb_mfree(db);
  return TDB_OK;
}

const char *tdb_errmsg(tdb_db *db) {
  if (!db) return "out of memory";
  return db->errmsg[0] ? db->errmsg : "not an error";
}

void tdb_free(void *p) { tdb_mfree(p); }

/* ---- Statement / execution surface (wired up in later phases) -------- */

int tdb_exec(tdb_db *db, const char *sql, tdb_exec_cb cb, void *user,
             char **errmsg) {
  TDB_UNUSED(sql); TDB_UNUSED(cb); TDB_UNUSED(user);
  set_err(db, "SQL execution not yet implemented");
  if (errmsg) *errmsg = tdb_strdup("SQL execution not yet implemented");
  return TDB_UNSUPPORTED;
}

int tdb_prepare_v2(tdb_db *db, const char *sql, int nbyte, tdb_stmt **ppStmt,
                   const char **pzTail) {
  TDB_UNUSED(sql); TDB_UNUSED(nbyte); TDB_UNUSED(pzTail);
  if (ppStmt) *ppStmt = NULL;
  set_err(db, "prepare not yet implemented");
  return TDB_UNSUPPORTED;
}

int tdb_step(tdb_stmt *stmt) { TDB_UNUSED(stmt); return TDB_MISUSE; }
int tdb_reset(tdb_stmt *stmt) { TDB_UNUSED(stmt); return TDB_OK; }
int tdb_finalize(tdb_stmt *stmt) { TDB_UNUSED(stmt); return TDB_OK; }

int tdb_bind_int64(tdb_stmt *s, int i, int64_t v) { TDB_UNUSED(s);TDB_UNUSED(i);TDB_UNUSED(v); return TDB_MISUSE; }
int tdb_bind_int(tdb_stmt *s, int i, int v) { return tdb_bind_int64(s, i, v); }
int tdb_bind_double(tdb_stmt *s, int i, double v) { TDB_UNUSED(s);TDB_UNUSED(i);TDB_UNUSED(v); return TDB_MISUSE; }
int tdb_bind_text(tdb_stmt *s, int i, const char *v, int n) { TDB_UNUSED(s);TDB_UNUSED(i);TDB_UNUSED(v);TDB_UNUSED(n); return TDB_MISUSE; }
int tdb_bind_blob(tdb_stmt *s, int i, const void *v, int n) { TDB_UNUSED(s);TDB_UNUSED(i);TDB_UNUSED(v);TDB_UNUSED(n); return TDB_MISUSE; }
int tdb_bind_null(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return TDB_MISUSE; }
int tdb_bind_parameter_count(tdb_stmt *s) { TDB_UNUSED(s); return 0; }

int tdb_column_count(tdb_stmt *s) { TDB_UNUSED(s); return 0; }
int tdb_column_type(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return TDB_NULL; }
int64_t tdb_column_int64(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return 0; }
int tdb_column_int(tdb_stmt *s, int i) { return (int)tdb_column_int64(s, i); }
double tdb_column_double(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return 0; }
const char *tdb_column_text(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return NULL; }
const void *tdb_column_blob(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return NULL; }
int tdb_column_bytes(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return 0; }
const char *tdb_column_name(tdb_stmt *s, int i) { TDB_UNUSED(s);TDB_UNUSED(i); return NULL; }

int tdb_create_function(tdb_db *db, const char *name, int nArg, tdb_func fn, void *p) {
  TDB_UNUSED(name); TDB_UNUSED(nArg); TDB_UNUSED(fn); TDB_UNUSED(p);
  set_err(db, "user functions not yet implemented");
  return TDB_UNSUPPORTED;
}

/* ---- Value accessors used by user functions -------------------------- */
int tdb_value_type(tdb_value *v) {
  if (!v) return TDB_NULL;
  switch (v->type) {
    case TDB_VAL_INT:  return TDB_INTEGER;
    case TDB_VAL_REAL: return TDB_FLOAT;
    case TDB_VAL_TEXT: return TDB_TEXT;
    case TDB_VAL_BLOB: return TDB_BLOB;
    default:           return TDB_NULL;
  }
}
int64_t tdb_value_int64(tdb_value *v) { return v ? tdb_value_as_int(v) : 0; }
double  tdb_value_double(tdb_value *v) { return v ? tdb_value_as_real(v) : 0; }
const char *tdb_value_text(tdb_value *v) { return v ? tdb_value_as_text(v) : NULL; }
const void *tdb_value_blob(tdb_value *v) {
  return (v && (v->type == TDB_VAL_BLOB || v->type == TDB_VAL_TEXT)) ? v->u.s.p : NULL;
}
int tdb_value_bytes(tdb_value *v) {
  return (v && (v->type == TDB_VAL_BLOB || v->type == TDB_VAL_TEXT)) ? v->u.s.n : 0;
}

/* ---- Result setters: context defined in later phases; stubs for now --- */
void tdb_result_int64(tdb_context *c, int64_t v) { TDB_UNUSED(c); TDB_UNUSED(v); }
void tdb_result_double(tdb_context *c, double v) { TDB_UNUSED(c); TDB_UNUSED(v); }
void tdb_result_text(tdb_context *c, const char *v, int n) { TDB_UNUSED(c);TDB_UNUSED(v);TDB_UNUSED(n); }
void tdb_result_null(tdb_context *c) { TDB_UNUSED(c); }
void tdb_result_error(tdb_context *c, const char *m) { TDB_UNUSED(c); TDB_UNUSED(m); }
void *tdb_user_data(tdb_context *c) { TDB_UNUSED(c); return NULL; }
