/*
** tdb_api.c — implementation of the public C API (transactiondb.h).
**
** Backed by the real engine: pager + WAL, catalog, transaction manager, lock
** manager and the row storage engine, with SQL run through the parser and the
** executor (which currently materializes result sets).
*/
#include "tdb_db.h"
#include "../sql/tdb_parser.h"
#include "../common/tdb_mem.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void tdb_db_seterr(tdb_db *db, const char *fmt, ...) {
  if (!db) return;
  va_list ap; va_start(ap, fmt);
  vsnprintf(db->errmsg, sizeof(db->errmsg), fmt, ap);
  va_end(ap);
}

/* ----------------------------- connection ----------------------------- */

int tdb_open(const char *filename, tdb_db **ppDb) {
  return tdb_open_v2(filename, ppDb, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
}

int tdb_open_v2(const char *filename, tdb_db **ppDb, int flags) {
  if (!ppDb) return TDB_MISUSE;
  *ppDb = NULL;
  tdb_db *db = (tdb_db *)tdb_calloc(sizeof(*db));
  if (!db) return TDB_NOMEM;
  db->flags = flags;
  db->autocommit = 1;
  db->path = filename ? tdb_strdup(filename) : NULL;

  int rc = tdb_pager_open(NULL, filename, flags, &db->pager);
  if (rc) goto fail;
  rc = tdb_catalog_open(db->pager, &db->cat);
  if (rc) goto fail;
  db->lm = tdb_lockmgr_new();
  rc = tdb_txnmgr_open(db->pager, db->lm, &db->tm);
  if (rc) goto fail;
  rc = tdb_engine_row_open(db->pager, &db->engine);
  if (rc) goto fail;

  *ppDb = db;
  return TDB_OK;
fail:
  tdb_close(db);
  return rc;
}

int tdb_close(tdb_db *db) {
  if (!db) return TDB_OK;
  if (db->txn) tdb_txn_rollback(db->txn);
  if (db->engine) tdb_storage_close(db->engine);
  if (db->tm) tdb_txnmgr_close(db->tm);
  if (db->lm) tdb_lockmgr_free(db->lm);
  if (db->cat) tdb_catalog_close(db->cat);
  if (db->pager) tdb_pager_close(db->pager);
  tdb_mfree(db->path);
  tdb_mfree(db);
  return TDB_OK;
}

const char *tdb_errmsg(tdb_db *db) {
  if (!db) return "out of memory";
  return db->errmsg[0] ? db->errmsg : "not an error";
}

void tdb_free(void *p) { tdb_mfree(p); }

/* ------------------------------ statements ---------------------------- */

void tdb_stmt_clear_results(tdb_stmt *st) {
  for (int r = 0; r < st->nrows; r++) {
    for (int c = 0; c < st->ncol; c++) tdb_value_clear(&st->rows[r][c]);
    tdb_mfree(st->rows[r]);
  }
  tdb_mfree(st->rows);
  st->rows = NULL; st->nrows = st->caprows = 0;
  if (st->colnames) {
    for (int c = 0; c < st->ncol; c++) tdb_mfree(st->colnames[c]);
    tdb_mfree(st->colnames);
    st->colnames = NULL;
  }
  st->ncol = 0;
}

int tdb_prepare_v2(tdb_db *db, const char *sql, int nbyte, tdb_stmt **ppStmt,
                   const char **pzTail) {
  TDB_UNUSED(nbyte);
  if (ppStmt) *ppStmt = NULL;
  if (!db || !sql) return TDB_MISUSE;

  tdb_arena *a = tdb_arena_new(8192);
  if (!a) return TDB_NOMEM;
  tdb_ast_stmt *ast = NULL; char *err = NULL; const char *tail = sql;
  int rc = tdb_parse(a, sql, &ast, &err, &tail);
  if (rc == TDB_DONE) {           /* nothing but whitespace */
    if (pzTail) *pzTail = tail;
    tdb_arena_free(a);
    return TDB_OK;
  }
  if (rc != TDB_OK) {
    tdb_db_seterr(db, "%s", err ? err : "parse error");
    tdb_arena_free(a);
    if (pzTail) *pzTail = tail;
    return rc;
  }
  tdb_stmt *st = (tdb_stmt *)tdb_calloc(sizeof(*st));
  if (!st) { tdb_arena_free(a); return TDB_NOMEM; }
  st->db = db;
  st->arena = a;
  st->ast = ast;
  st->cur = -1;
  st->is_select = (ast->kind == ST_SELECT);
  if (ppStmt) *ppStmt = st;
  if (pzTail) *pzTail = tail;
  return TDB_OK;
}

int tdb_step(tdb_stmt *st) {
  if (!st) return TDB_MISUSE;
  if (!st->executed) {
    int rc = tdb_stmt_execute(st);
    st->executed = 1;
    st->cur = -1;
    if (rc != TDB_OK) return rc;
  }
  if (st->is_select) {
    st->cur++;
    return (st->cur < st->nrows) ? TDB_ROW : TDB_DONE;
  }
  return TDB_DONE;
}

int tdb_reset(tdb_stmt *st) {
  if (!st) return TDB_MISUSE;
  tdb_stmt_clear_results(st);
  st->executed = 0;
  st->cur = -1;
  st->changes = 0;
  return TDB_OK;
}

int tdb_finalize(tdb_stmt *st) {
  if (!st) return TDB_OK;
  tdb_stmt_clear_results(st);
  if (st->params) {
    for (int i = 0; i < st->nparams; i++) tdb_value_clear(&st->params[i]);
    tdb_mfree(st->params);
  }
  if (st->arena) tdb_arena_free(st->arena);
  tdb_mfree(st);
  return TDB_OK;
}

/* ------------------------------ binding ------------------------------- */

static tdb_value *bind_slot(tdb_stmt *st, int idx) {
  if (idx < 1) return NULL;
  if (idx > st->nparams) {
    tdb_value *p = (tdb_value *)tdb_realloc(st->params, sizeof(tdb_value) * (size_t)idx);
    if (!p) return NULL;
    for (int i = st->nparams; i < idx; i++) tdb_value_init(&p[i]);
    st->params = p;
    st->nparams = idx;
  }
  return &st->params[idx - 1];
}

int tdb_bind_int64(tdb_stmt *st, int idx, int64_t v) {
  tdb_value *s = bind_slot(st, idx); if (!s) return TDB_RANGE;
  tdb_value_set_int(s, v); return TDB_OK;
}
int tdb_bind_int(tdb_stmt *st, int idx, int v) { return tdb_bind_int64(st, idx, v); }
int tdb_bind_double(tdb_stmt *st, int idx, double v) {
  tdb_value *s = bind_slot(st, idx); if (!s) return TDB_RANGE;
  tdb_value_set_real(s, v); return TDB_OK;
}
int tdb_bind_text(tdb_stmt *st, int idx, const char *v, int n) {
  tdb_value *s = bind_slot(st, idx); if (!s) return TDB_RANGE;
  return tdb_value_set_text(s, v, n, 1);
}
int tdb_bind_blob(tdb_stmt *st, int idx, const void *v, int n) {
  tdb_value *s = bind_slot(st, idx); if (!s) return TDB_RANGE;
  return tdb_value_set_blob(s, v, n, 1);
}
int tdb_bind_null(tdb_stmt *st, int idx) {
  tdb_value *s = bind_slot(st, idx); if (!s) return TDB_RANGE;
  tdb_value_set_null(s); return TDB_OK;
}
int tdb_bind_parameter_count(tdb_stmt *st) { return st ? st->nparams : 0; }

/* ------------------------------ columns ------------------------------- */

static tdb_value *col(tdb_stmt *st, int i) {
  if (!st || !st->is_select || st->cur < 0 || st->cur >= st->nrows) return NULL;
  if (i < 0 || i >= st->ncol) return NULL;
  return &st->rows[st->cur][i];
}

int tdb_column_count(tdb_stmt *st) { return st ? st->ncol : 0; }

int tdb_column_type(tdb_stmt *st, int i) {
  tdb_value *v = col(st, i);
  if (!v) return TDB_NULL;
  switch (v->type) {
    case TDB_VAL_INT: return TDB_INTEGER;
    case TDB_VAL_REAL: return TDB_FLOAT;
    case TDB_VAL_TEXT: return TDB_TEXT;
    case TDB_VAL_BLOB: return TDB_BLOB;
    default: return TDB_NULL;
  }
}
int64_t tdb_column_int64(tdb_stmt *st, int i) { tdb_value *v = col(st, i); return v ? tdb_value_as_int(v) : 0; }
int tdb_column_int(tdb_stmt *st, int i) { return (int)tdb_column_int64(st, i); }
double tdb_column_double(tdb_stmt *st, int i) { tdb_value *v = col(st, i); return v ? tdb_value_as_real(v) : 0; }
const char *tdb_column_text(tdb_stmt *st, int i) { tdb_value *v = col(st, i); return v ? tdb_value_as_text(v) : NULL; }
const void *tdb_column_blob(tdb_stmt *st, int i) {
  tdb_value *v = col(st, i);
  return (v && (v->type == TDB_VAL_BLOB || v->type == TDB_VAL_TEXT)) ? v->u.s.p : NULL;
}
int tdb_column_bytes(tdb_stmt *st, int i) {
  tdb_value *v = col(st, i);
  return (v && (v->type == TDB_VAL_BLOB || v->type == TDB_VAL_TEXT)) ? v->u.s.n : 0;
}
const char *tdb_column_name(tdb_stmt *st, int i) {
  if (!st || i < 0 || i >= st->ncol || !st->colnames) return NULL;
  return st->colnames[i];
}

/* ------------------------------- exec --------------------------------- */

int tdb_exec(tdb_db *db, const char *sql, tdb_exec_cb cb, void *user,
             char **errmsg) {
  if (errmsg) *errmsg = NULL;
  if (!db || !sql) return TDB_MISUSE;
  const char *tail = sql;
  int rc = TDB_OK;

  while (tail && *tail) {
    tdb_stmt *st = NULL;
    const char *next = NULL;
    rc = tdb_prepare_v2(db, tail, -1, &st, &next);
    if (rc != TDB_OK) { if (errmsg) *errmsg = tdb_strdup(db->errmsg); break; }
    if (!st) { tail = next; continue; }   /* empty / whitespace */

    for (;;) {
      int s = tdb_step(st);
      if (s == TDB_ROW) {
        if (cb) {
          int nc = tdb_column_count(st);
          char **vals = (char **)tdb_malloc(sizeof(char *) * (size_t)(nc ? nc : 1));
          char **names = (char **)tdb_malloc(sizeof(char *) * (size_t)(nc ? nc : 1));
          for (int i = 0; i < nc; i++) {
            vals[i] = (char *)tdb_column_text(st, i);
            names[i] = (char *)tdb_column_name(st, i);
          }
          int abort = cb(user, nc, vals, names);
          tdb_mfree(vals); tdb_mfree(names);
          if (abort) { rc = TDB_ABORT; break; }
        }
        continue;
      }
      if (s == TDB_DONE) { rc = TDB_OK; break; }
      rc = s; /* error */
      if (errmsg) *errmsg = tdb_strdup(db->errmsg);
      break;
    }
    tdb_finalize(st);
    if (rc != TDB_OK) break;
    tail = next;
  }
  return rc;
}

/* ----------------- user-defined functions (Phase 9) ------------------- */

int tdb_create_function(tdb_db *db, const char *name, int nArg, tdb_func fn, void *p) {
  TDB_UNUSED(name); TDB_UNUSED(nArg); TDB_UNUSED(fn); TDB_UNUSED(p);
  tdb_db_seterr(db, "user-defined functions arrive with Lua (Phase 9)");
  return TDB_UNSUPPORTED;
}

/* ---------------------- value / result accessors ---------------------- */

int tdb_value_type(tdb_value *v) {
  if (!v) return TDB_NULL;
  switch (v->type) {
    case TDB_VAL_INT: return TDB_INTEGER;
    case TDB_VAL_REAL: return TDB_FLOAT;
    case TDB_VAL_TEXT: return TDB_TEXT;
    case TDB_VAL_BLOB: return TDB_BLOB;
    default: return TDB_NULL;
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

void tdb_result_int64(tdb_context *c, int64_t v) { TDB_UNUSED(c); TDB_UNUSED(v); }
void tdb_result_double(tdb_context *c, double v) { TDB_UNUSED(c); TDB_UNUSED(v); }
void tdb_result_text(tdb_context *c, const char *v, int n) { TDB_UNUSED(c); TDB_UNUSED(v); TDB_UNUSED(n); }
void tdb_result_null(tdb_context *c) { TDB_UNUSED(c); }
void tdb_result_error(tdb_context *c, const char *m) { TDB_UNUSED(c); TDB_UNUSED(m); }
void *tdb_user_data(tdb_context *c) { TDB_UNUSED(c); return NULL; }
