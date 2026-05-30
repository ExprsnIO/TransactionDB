/* tdb_exec.c — statement execution: resolution, evaluation, materialization.
**
** SELECT is executed by materializing the cross-product of FROM sources
** (INNER/CROSS joins), filtering by the join ON conditions and WHERE,
** optionally grouping/aggregating, projecting, sorting, and applying
** LIMIT/OFFSET — producing a fully materialized result set. DML evaluates
** rows and drives the storage vtable. Outer joins, correlated subqueries,
** and lazy (volcano) streaming are intentionally left for later.
*/
#include "../api/tdb_db.h"
#include "tdb_analyze.h"
#include "tdb_parser.h"
#include "../common/tdb_mem.h"
#include "../value/tdb_record.h"
#include "../value/tdb_sqltype.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------- helpers ------------------------------ */

#define MAXSRC 8

typedef struct { tdb_table *tbl; const char *alias; int base; } qsrc;
typedef struct {
  qsrc      src[MAXSRC];
  int       nsrc;
  int       totalcols;
  tdb_expr *aggs[64];
  int       nagg;
  int       paramcount;
  int       err;
} qctx;

typedef struct { tdb_value **rows; int n, cap, ncol; } rowset;

static void rowset_init(rowset *r, int ncol) { r->rows = NULL; r->n = r->cap = 0; r->ncol = ncol; }
static void rowset_add(rowset *r, tdb_value *row) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 16;
    r->rows = (tdb_value **)tdb_realloc(r->rows, sizeof(void *) * (size_t)r->cap);
  }
  r->rows[r->n++] = row;
}
static void free_row(tdb_value *row, int ncol) {
  if (!row) return;
  for (int i = 0; i < ncol; i++) tdb_value_clear(&row[i]);
  tdb_mfree(row);
}
static void rowset_free(rowset *r) {
  for (int i = 0; i < r->n; i++) free_row(r->rows[i], r->ncol);
  tdb_mfree(r->rows);
}
static tdb_value *row_alloc(int ncol) {
  tdb_value *row = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)ncol);
  for (int i = 0; i < ncol; i++) tdb_value_init(&row[i]);
  return row;
}

/* ---------------------------- materialize ----------------------------- */

/* Materialize all rows of `t` visible to txn into `rs` (deep copies). */
static int mat_table(tdb_db *db, tdb_table *t, rowset *rs) {
  rowset_init(rs, t->ncol);
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, NULL, &sc);
  if (rc) return rc;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = db->engine->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW) {
    tdb_value tmp[64]; int nc = 0;
    if (tdb_record_decode(rec, (size_t)reclen, tmp, 64, &nc)) { rc = TDB_CORRUPT; break; }
    tdb_value *row = row_alloc(t->ncol);
    for (int i = 0; i < t->ncol; i++)
      if (i < nc) tdb_value_copy(&row[i], &tmp[i]);
    for (int i = 0; i < nc; i++) tdb_value_clear(&tmp[i]);
    rowset_add(rs, row);
  }
  db->engine->vtab->scan_close(sc);
  return (rc == TDB_DONE) ? TDB_OK : rc;
}

/* ----------------------------- resolution ----------------------------- */

static int resolve_expr(tdb_db *db, qctx *q, tdb_expr *e);

static int resolve_column(tdb_db *db, qctx *q, tdb_expr *e) {
  for (int s = 0; s < q->nsrc; s++) {
    qsrc *src = &q->src[s];
    if (e->table) {
      const char *nm = src->alias ? src->alias : src->tbl->name;
      if (strcasecmp(nm, e->table) != 0) continue;
    }
    int ci = tdb_table_find_column(src->tbl, e->name);
    if (ci >= 0) { e->col_index = src->base + ci; return TDB_OK; }
  }
  tdb_db_seterr(db, "no such column: %s", e->name);
  q->err = 1;
  return TDB_ERROR;
}

static int resolve_list(tdb_db *db, qctx *q, tdb_exprlist *l) {
  if (!l) return TDB_OK;
  for (int i = 0; i < l->n; i++)
    if (resolve_expr(db, q, l->items[i])) return TDB_ERROR;
  return TDB_OK;
}

static int resolve_expr(tdb_db *db, qctx *q, tdb_expr *e) {
  if (!e) return TDB_OK;
  switch (e->kind) {
    case EX_COLUMN: return resolve_column(db, q, e);
    case EX_PARAM: e->col_index = q->paramcount++; return TDB_OK;
    case EX_AGG:
      if (q->nagg < 64) { e->agg_index = q->nagg; q->aggs[q->nagg++] = e; }
      return resolve_list(db, q, e->args);
    case EX_BINARY:
    case EX_UNARY:
      if (resolve_expr(db, q, e->left)) return TDB_ERROR;
      return resolve_expr(db, q, e->right);
    case EX_FUNC: case EX_IN: case EX_BETWEEN:
      if (resolve_expr(db, q, e->left)) return TDB_ERROR;
      return resolve_list(db, q, e->args);
    case EX_CASE:
      if (resolve_expr(db, q, e->left)) return TDB_ERROR;
      if (resolve_expr(db, q, e->right)) return TDB_ERROR;
      return resolve_list(db, q, e->args);
    case EX_CAST:
      return resolve_expr(db, q, e->left);
    default: return TDB_OK;
  }
}

/* ----------------------------- evaluation ----------------------------- */

typedef struct {
  tdb_value *row;       /* combined source row */
  int        nrow;
  tdb_value *aggvals;   /* aggregate results (post-agg), or NULL */
  int        naggvals;
  tdb_stmt  *stmt;
} ectx;

static int eval(tdb_db *db, ectx *c, const tdb_expr *e, tdb_value *out);

static int val_true(const tdb_value *v) {
  if (v->type == TDB_VAL_NULL) return 0;
  if (v->type == TDB_VAL_INT) return v->u.i != 0;
  if (v->type == TDB_VAL_REAL) return v->u.r != 0;
  return v->u.s.n != 0;
}

static int eval_binary(tdb_db *db, ectx *c, const tdb_expr *e, tdb_value *out) {
  int op = e->op;
  if (op == TK_AND || op == TK_OR) {
    tdb_value l, r; tdb_value_init(&l); tdb_value_init(&r);
    eval(db, c, e->left, &l);
    int lt = val_true(&l); tdb_value_clear(&l);
    if (op == TK_AND && !lt) { tdb_value_set_int(out, 0); return TDB_OK; }
    if (op == TK_OR && lt)   { tdb_value_set_int(out, 1); return TDB_OK; }
    eval(db, c, e->right, &r);
    int rt = val_true(&r); tdb_value_clear(&r);
    tdb_value_set_int(out, rt ? 1 : 0);
    return TDB_OK;
  }

  tdb_value l, r; tdb_value_init(&l); tdb_value_init(&r);
  eval(db, c, e->left, &l);
  eval(db, c, e->right, &r);
  int rc = TDB_OK;

  if (op == TK_CONCAT) {
    if (l.type == TDB_VAL_NULL || r.type == TDB_VAL_NULL) tdb_value_set_null(out);
    else {
      const char *a = tdb_value_as_text(&l), *b = tdb_value_as_text(&r);
      size_t na = a ? strlen(a) : 0, nb = b ? strlen(b) : 0;
      char *buf = (char *)tdb_malloc(na + nb + 1);
      if (na) memcpy(buf, a, na);
      if (nb) memcpy(buf + na, b, nb);
      buf[na + nb] = '\0';
      tdb_value_set_text(out, buf, (int)(na + nb), 1);
      tdb_mfree(buf);
    }
    goto done;
  }

  if (op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE ||
      op == TK_GT || op == TK_GE) {
    if (l.type == TDB_VAL_NULL || r.type == TDB_VAL_NULL) { tdb_value_set_null(out); goto done; }
    int cmp = tdb_value_compare(&l, &r, TDB_COLL_BINARY);
    int res = 0;
    switch (op) {
      case TK_EQ: res = (cmp == 0); break;
      case TK_NE: res = (cmp != 0); break;
      case TK_LT: res = (cmp < 0); break;
      case TK_LE: res = (cmp <= 0); break;
      case TK_GT: res = (cmp > 0); break;
      case TK_GE: res = (cmp >= 0); break;
    }
    tdb_value_set_int(out, res);
    goto done;
  }

  /* arithmetic */
  if (l.type == TDB_VAL_NULL || r.type == TDB_VAL_NULL) { tdb_value_set_null(out); goto done; }
  if (l.type == TDB_VAL_INT && r.type == TDB_VAL_INT &&
      (op == TK_PLUS || op == TK_MINUS || op == TK_STAR ||
       op == TK_SLASH || op == TK_PERCENT)) {
    int64_t a = l.u.i, b = r.u.i, v = 0;
    switch (op) {
      case TK_PLUS: v = a + b; break;
      case TK_MINUS: v = a - b; break;
      case TK_STAR: v = a * b; break;
      case TK_SLASH: if (b == 0) { tdb_value_set_null(out); goto done; } v = a / b; break;
      case TK_PERCENT: if (b == 0) { tdb_value_set_null(out); goto done; } v = a % b; break;
    }
    tdb_value_set_int(out, v);
    goto done;
  }
  {
    double a = tdb_value_as_real(&l), b = tdb_value_as_real(&r), v = 0;
    switch (op) {
      case TK_PLUS: v = a + b; break;
      case TK_MINUS: v = a - b; break;
      case TK_STAR: v = a * b; break;
      case TK_SLASH: if (b == 0) { tdb_value_set_null(out); goto done; } v = a / b; break;
      case TK_PERCENT: if (b == 0) { tdb_value_set_null(out); goto done; } v = fmod(a, b); break;
      default: rc = TDB_ERROR; break;
    }
    tdb_value_set_real(out, v);
  }
done:
  tdb_value_clear(&l); tdb_value_clear(&r);
  return rc;
}

static int eval_func(tdb_db *db, ectx *c, const tdb_expr *e, tdb_value *out) {
  const char *fn = e->name;
  int argc = e->args ? e->args->n : 0;
  tdb_value a0; tdb_value_init(&a0);
  if (argc >= 1) eval(db, c, e->args->items[0], &a0);

  if (!strcasecmp(fn, "abs") && argc == 1) {
    if (a0.type == TDB_VAL_REAL) tdb_value_set_real(out, fabs(a0.u.r));
    else tdb_value_set_int(out, llabs((long long)tdb_value_as_int(&a0)));
  } else if (!strcasecmp(fn, "length") && argc == 1) {
    const char *s = tdb_value_as_text(&a0);
    tdb_value_set_int(out, s ? (int64_t)strlen(s) : 0);
  } else if ((!strcasecmp(fn, "upper") || !strcasecmp(fn, "lower")) && argc == 1) {
    const char *s = tdb_value_as_text(&a0);
    if (!s) tdb_value_set_null(out);
    else {
      size_t n = strlen(s); char *b = (char *)tdb_malloc(n + 1);
      int up = !strcasecmp(fn, "upper");
      for (size_t i = 0; i < n; i++) b[i] = (char)(up ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]));
      b[n] = '\0'; tdb_value_set_text(out, b, (int)n, 1); tdb_mfree(b);
    }
  } else if (!strcasecmp(fn, "coalesce")) {
    tdb_value_clear(out); tdb_value_init(out);
    int found = 0;
    for (int i = 0; i < argc; i++) {
      tdb_value v; tdb_value_init(&v);
      eval(db, c, e->args->items[i], &v);
      if (v.type != TDB_VAL_NULL) { tdb_value_copy(out, &v); tdb_value_clear(&v); found = 1; break; }
      tdb_value_clear(&v);
    }
    if (!found) tdb_value_set_null(out);
  } else {
    tdb_db_seterr(db, "no such function: %s", fn);
    tdb_value_clear(&a0);
    return TDB_ERROR;
  }
  tdb_value_clear(&a0);
  return TDB_OK;
}

static int eval(tdb_db *db, ectx *c, const tdb_expr *e, tdb_value *out) {
  tdb_value_clear(out);
  if (!e) { tdb_value_set_null(out); return TDB_OK; }
  switch (e->kind) {
    case EX_LITERAL: return tdb_value_copy(out, &e->lit);
    case EX_NULL: tdb_value_set_null(out); return TDB_OK;
    case EX_COLUMN:
      if (c->row && e->col_index >= 0 && e->col_index < c->nrow)
        return tdb_value_copy(out, &c->row[e->col_index]);
      tdb_value_set_null(out); return TDB_OK;
    case EX_PARAM:
      if (c->stmt && e->col_index >= 0 && e->col_index < c->stmt->nparams)
        return tdb_value_copy(out, &c->stmt->params[e->col_index]);
      tdb_value_set_null(out); return TDB_OK;
    case EX_AGG:
      if (c->aggvals && e->agg_index >= 0 && e->agg_index < c->naggvals)
        return tdb_value_copy(out, &c->aggvals[e->agg_index]);
      tdb_value_set_null(out); return TDB_OK;
    case EX_UNARY: {
      tdb_value v; tdb_value_init(&v);
      eval(db, c, e->left, &v);
      if (e->op == TK_NOT) tdb_value_set_int(out, !val_true(&v));
      else if (e->op == TK_MINUS) {
        if (v.type == TDB_VAL_REAL) tdb_value_set_real(out, -v.u.r);
        else if (v.type == TDB_VAL_INT) tdb_value_set_int(out, -v.u.i);
        else tdb_value_set_null(out);
      } else tdb_value_copy(out, &v);
      tdb_value_clear(&v);
      return TDB_OK;
    }
    case EX_BINARY: return eval_binary(db, c, e, out);
    case EX_FUNC: return eval_func(db, c, e, out);
    case EX_CAST: {
      tdb_value v; tdb_value_init(&v);
      eval(db, c, e->left, &v);
      const char *why = NULL;
      tdb_typespec_coerce(&v, &e->cast, &why);
      tdb_value_copy(out, &v); tdb_value_clear(&v);
      return TDB_OK;
    }
    case EX_BETWEEN: {
      tdb_value x, lo, hi; tdb_value_init(&x); tdb_value_init(&lo); tdb_value_init(&hi);
      eval(db, c, e->left, &x);
      eval(db, c, e->args->items[0], &lo);
      eval(db, c, e->args->items[1], &hi);
      int res = (tdb_value_compare(&x, &lo, TDB_COLL_BINARY) >= 0 &&
                 tdb_value_compare(&x, &hi, TDB_COLL_BINARY) <= 0);
      if (e->negated) res = !res;
      tdb_value_set_int(out, res);
      tdb_value_clear(&x); tdb_value_clear(&lo); tdb_value_clear(&hi);
      return TDB_OK;
    }
    case EX_IN: {
      tdb_value x; tdb_value_init(&x); eval(db, c, e->left, &x);
      int res = 0;
      if (e->args) for (int i = 0; i < e->args->n; i++) {
        tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[i], &v);
        if (x.type != TDB_VAL_NULL && v.type != TDB_VAL_NULL &&
            tdb_value_compare(&x, &v, TDB_COLL_BINARY) == 0) res = 1;
        tdb_value_clear(&v);
        if (res) break;
      }
      if (e->negated) res = !res;
      tdb_value_set_int(out, res);
      tdb_value_clear(&x);
      return TDB_OK;
    }
    case EX_CASE: {
      tdb_value base; tdb_value_init(&base);
      int have_base = (e->left != NULL);
      if (have_base) eval(db, c, e->left, &base);
      int matched = 0;
      if (e->args) for (int i = 0; i + 1 < e->args->n; i += 2) {
        tdb_value w; tdb_value_init(&w); eval(db, c, e->args->items[i], &w);
        int hit;
        if (have_base) hit = (tdb_value_compare(&base, &w, TDB_COLL_BINARY) == 0);
        else hit = val_true(&w);
        tdb_value_clear(&w);
        if (hit) { eval(db, c, e->args->items[i + 1], out); matched = 1; break; }
      }
      if (!matched) { if (e->right) eval(db, c, e->right, out); else tdb_value_set_null(out); }
      tdb_value_clear(&base);
      return TDB_OK;
    }
    default:
      tdb_value_set_null(out);
      return TDB_OK;
  }
}

/* --------------------------- cross product ---------------------------- */

/* recursively build the combined row across sources; emit when complete */
typedef struct {
  tdb_db *db; qctx *q; rowset *sets; tdb_value *combined; int ncol;
  rowset *out; tdb_expr *filter; tdb_stmt *stmt;
} combine_ctx;

static int combine_rec(combine_ctx *cc, int level) {
  if (level == cc->q->nsrc) {
    if (cc->filter) {
      ectx ec = { cc->combined, cc->ncol, NULL, 0, cc->stmt };
      tdb_value v; tdb_value_init(&v);
      eval(cc->db, &ec, cc->filter, &v);
      int ok = val_true(&v);
      tdb_value_clear(&v);
      if (!ok) return TDB_OK;
    }
    tdb_value *row = row_alloc(cc->ncol);
    for (int i = 0; i < cc->ncol; i++) tdb_value_copy(&row[i], &cc->combined[i]);
    rowset_add(cc->out, row);
    return TDB_OK;
  }
  qsrc *src = &cc->q->src[level];
  rowset *rs = &cc->sets[level];
  for (int i = 0; i < rs->n; i++) {
    for (int k = 0; k < src->tbl->ncol; k++)
      tdb_value_copy(&cc->combined[src->base + k], &rs->rows[i][k]);
    int rc = combine_rec(cc, level + 1);
    if (rc) return rc;
  }
  return TDB_OK;
}

/* ------------------------------ SELECT -------------------------------- */

/* combine all ON conditions of inner joins + WHERE into one predicate list */
static tdb_expr *and_join(tdb_arena *a, tdb_expr *x, tdb_expr *y) {
  if (!x) return y;
  if (!y) return x;
  tdb_expr *e = tdb_expr_new(a, EX_BINARY);
  e->op = TK_AND; e->left = x; e->right = y;
  return e;
}

static void add_result_row(tdb_stmt *st, tdb_value *vals) {
  if (st->nrows == st->caprows) {
    st->caprows = st->caprows ? st->caprows * 2 : 16;
    st->rows = (tdb_value **)tdb_realloc(st->rows, sizeof(void *) * (size_t)st->caprows);
  }
  st->rows[st->nrows++] = vals;
}

/* order-by sort state (module-local, executor is single-threaded per stmt) */
typedef struct { tdb_value **keys; uint8_t *desc; int nkey; int nrows; } sortinfo;
static sortinfo *g_sort;
static int sort_cmp(const void *pa, const void *pb) {
  int ia = *(const int *)pa, ib = *(const int *)pb;
  for (int k = 0; k < g_sort->nkey; k++) {
    int c = tdb_value_compare(&g_sort->keys[ia][k], &g_sort->keys[ib][k], TDB_COLL_BINARY);
    if (g_sort->desc[k]) c = -c;
    if (c) return c;
  }
  return 0;
}

static int exec_select(tdb_db *db, tdb_stmt *st, tdb_select *q);

static int run_select(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  return exec_select(db, st, q);
}

#include "tdb_exec_select.inc"

/* ------------------------------- DML ---------------------------------- */

#include "tdb_exec_dml.inc"

/* ------------------------------ dispatch ------------------------------ */

int tdb_stmt_execute(tdb_stmt *st) {
  tdb_db *db = st->db;
  tdb_ast_stmt *a = st->ast;
  if (!a) return TDB_OK;

  /* transaction-control statements manage db->txn directly */
  if (a->kind == ST_BEGIN || a->kind == ST_COMMIT || a->kind == ST_ROLLBACK)
    return exec_txn(db, st, a);

  /* otherwise run inside the explicit txn, or an auto txn we open/close here */
  int started = 0;
  if (!db->txn) {
    int writable = (a->kind != ST_SELECT);
    int rc = tdb_txn_begin(db->tm, TDB_ISO_SNAPSHOT, writable, &db->txn);
    if (rc) return rc;
    started = 1;
  }

  int rc;
  switch (a->kind) {
    case ST_SELECT: st->is_select = 1; rc = run_select(db, st, a->u.select); break;
    case ST_INSERT: rc = exec_insert(db, st, a); break;
    case ST_UPDATE: rc = exec_update(db, st, a); break;
    case ST_DELETE: rc = exec_delete(db, st, a); break;
    case ST_CREATE_TABLE: rc = exec_create_table(db, st, a); break;
    case ST_CREATE_INDEX: rc = exec_create_index(db, st, a); break;
    case ST_DROP_TABLE: case ST_DROP_INDEX: case ST_DROP_VIEW: rc = exec_drop(db, st, a); break;
    case ST_CREATE_VIEW: rc = exec_create_view(db, st, a); break;
    default:
      tdb_db_seterr(db, "statement type not yet executable");
      rc = TDB_UNSUPPORTED;
      break;
  }

  if (started) {
    if (a->kind != ST_SELECT && rc == TDB_OK) {
      int crc = tdb_txn_commit(db->txn);
      if (crc) rc = crc;
    } else {
      tdb_txn_rollback(db->txn);
    }
    db->txn = NULL;
  }
  return rc;
}
