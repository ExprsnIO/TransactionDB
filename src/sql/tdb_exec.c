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
#ifdef TDB_HAVE_LUA
#include "../lua/tdb_lua.h"
#endif
#include "../common/tdb_mem.h"
#include "../value/tdb_record.h"
#include "../value/tdb_sqltype.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------- helpers ------------------------------ */

#define MAXSRC 8

typedef struct {
  tdb_table   *tbl;       /* base table, or NULL for a derived table / view */
  const char  *alias;
  int          base;      /* offset of this source's columns in the combined row */
  int          ncol;
  char       **colnames;  /* column names for a derived/view source (else NULL) */
  int          join;      /* tdb_join_kind: join to the preceding source */
  tdb_expr    *on;        /* ON condition (resolved), or NULL */
} qsrc;
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

/* Materialize the rows of `t` visible to txn into `rs` (deep copies). If
** `idx` is non-NULL the scan is index-driven over `range`. */
static int mat_table(tdb_db *db, tdb_table *t, tdb_index *idx,
                     const tdb_keyrange *range, tdb_txnid as_of,
                     const uint8_t *colmask, rowset *rs) {
  rowset_init(rs, t->ncol);
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, idx, range, as_of, colmask, &sc);
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
      const char *nm = src->alias ? src->alias : (src->tbl ? src->tbl->name : NULL);
      if (!nm || strcasecmp(nm, e->table) != 0) continue;
    }
    if (src->tbl) {
      int ci = tdb_table_find_column(src->tbl, e->name);
      if (ci >= 0) { e->col_index = src->base + ci; return TDB_OK; }
    } else {
      for (int k = 0; k < src->ncol; k++)
        if (src->colnames[k] && strcasecmp(src->colnames[k], e->name) == 0) {
          e->col_index = src->base + k; return TDB_OK;
        }
    }
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
static int exec_select(tdb_db *db, tdb_stmt *st, tdb_select *q);

/* A WITH frame: the CTE list attached to one SELECT, linked to the enclosing
** frame so name lookup can walk outward (lexical scoping). */
typedef struct cte_scope {
  tdb_ctelist      *list;
  struct cte_scope *parent;
} cte_scope;

/* Find an in-scope, non-active CTE by name (case-insensitive). Active CTEs are
** those currently being materialized — skipping them blocks self-reference. */
static tdb_cte *find_cte(tdb_stmt *st, const char *name) {
  for (cte_scope *sc = st->cte_scope; sc; sc = sc->parent) {
    tdb_ctelist *l = sc->list;
    for (int i = 0; l && i < l->n; i++)
      if (!l->items[i].active && strcasecmp(l->items[i].name, name) == 0)
        return &l->items[i];
  }
  return NULL;
}

/* Run an (uncorrelated) subquery into a throwaway statement. The caller reads
** tmp->rows / nrows / ncol, then calls tdb_stmt_clear_results(tmp). The arena
** and bound params are shared with the parent (not freed here). The CTE scope
** is inherited so common table expressions stay visible inside subqueries. */
static int run_subselect(tdb_db *db, tdb_stmt *parent, tdb_select *q, tdb_stmt *tmp) {
  memset(tmp, 0, sizeof(*tmp));
  tmp->db = db; tmp->arena = parent->arena;
  tmp->params = parent->params; tmp->nparams = parent->nparams;
  tmp->cur = -1; tmp->is_select = 1;
  tmp->cte_scope = parent->cte_scope;
  return exec_select(db, tmp, q);
}

static int val_true(const tdb_value *v) {
  if (v->type == TDB_VAL_NULL) return 0;
  if (v->type == TDB_VAL_INT) return v->u.i != 0;
  if (v->type == TDB_VAL_REAL) return v->u.r != 0;
  return v->u.s.n != 0;
}

/* SQL LIKE: '%' matches any run, '_' matches one char (case-insensitive).
** `esc` (0 = none) makes the following pattern char a literal. */
static int like_match(const char *pat, const char *str, int esc) {
  while (*pat) {
    if (esc && (unsigned char)*pat == esc && pat[1]) {
      if (tolower((unsigned char)pat[1]) != tolower((unsigned char)*str)) return 0;
      pat += 2; str++;
    } else if (*pat == '%') {
      while (*pat == '%') pat++;
      if (!*pat) return 1;
      for (; *str; str++) if (like_match(pat, str, esc)) return 1;
      return 0;
    } else if (*pat == '_') {
      if (!*str) return 0;
      pat++; str++;
    } else {
      if (tolower((unsigned char)*pat) != tolower((unsigned char)*str)) return 0;
      pat++; str++;
    }
  }
  return *str == 0;
}

/* SQL GLOB: case-sensitive Unix-style wildcards — '*' matches any run, '?'
** matches one char, and '[...]' a char class ('^' negates, 'a-z' ranges). */
static int glob_match(const char *pat, const char *str) {
  while (*pat) {
    if (*pat == '*') {
      while (*pat == '*') pat++;
      if (!*pat) return 1;
      for (; *str; str++) if (glob_match(pat, str)) return 1;
      return 0;
    } else if (*pat == '?') {
      if (!*str) return 0;
      pat++; str++;
    } else if (*pat == '[') {
      const char *p = pat + 1;
      int neg = 0;
      if (*p == '^') { neg = 1; p++; }
      const char *first = p;
      int matched = 0;
      while (*p && (p == first || *p != ']')) {
        if (p[1] == '-' && p[2] && p[2] != ']') {
          if ((unsigned char)*str >= (unsigned char)p[0] &&
              (unsigned char)*str <= (unsigned char)p[2]) matched = 1;
          p += 3;
        } else {
          if (*str == *p) matched = 1;
          p++;
        }
      }
      if (*p != ']') return 0;              /* unterminated class */
      if (!*str || matched == neg) return 0;
      pat = p + 1; str++;
    } else {
      if (*pat != *str) return 0;
      pat++; str++;
    }
  }
  return *str == 0;
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

  if (op == TK_LIKE || op == TK_GLOB) {
    if (l.type == TDB_VAL_NULL || r.type == TDB_VAL_NULL) { tdb_value_set_null(out); goto done; }
    const char *s = tdb_value_as_text(&l), *p = tdb_value_as_text(&r);
    int m;
    if (op == TK_GLOB) {
      m = glob_match(p ? p : "", s ? s : "");
    } else {
      int esc = 0;
      if (e->args && e->args->n > 0) {     /* ESCAPE 'c' */
        tdb_value ev; tdb_value_init(&ev);
        eval(db, c, e->args->items[0], &ev);
        const char *es = tdb_value_as_text(&ev);
        if (es && es[0]) esc = (unsigned char)es[0];
        tdb_value_clear(&ev);
      }
      m = like_match(p ? p : "", s ? s : "", esc);
    }
    if (e->negated) m = !m;
    tdb_value_set_int(out, m);
    goto done;
  }

  if (op == TK_IS) {  /* NULL-aware: NULL IS NULL is true */
    int eq;
    if (l.type == TDB_VAL_NULL && r.type == TDB_VAL_NULL) eq = 1;
    else if (l.type == TDB_VAL_NULL || r.type == TDB_VAL_NULL) eq = 0;
    else eq = (tdb_value_compare(&l, &r, TDB_COLL_BINARY) == 0);
    if (e->negated) eq = !eq;
    tdb_value_set_int(out, eq);
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
  } else if (!strcasecmp(fn, "ifnull") && argc == 2) {
    if (a0.type != TDB_VAL_NULL) tdb_value_copy(out, &a0);
    else { tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[1], &v); tdb_value_copy(out, &v); tdb_value_clear(&v); }
  } else if (!strcasecmp(fn, "nullif") && argc == 2) {
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[1], &v);
    if (a0.type != TDB_VAL_NULL && v.type != TDB_VAL_NULL && tdb_value_compare(&a0, &v, TDB_COLL_BINARY) == 0) tdb_value_set_null(out);
    else tdb_value_copy(out, &a0);
    tdb_value_clear(&v);
  } else if (!strcasecmp(fn, "typeof") && argc == 1) {
    const char *t = a0.type == TDB_VAL_NULL ? "null" : a0.type == TDB_VAL_INT ? "integer"
                  : a0.type == TDB_VAL_REAL ? "real" : a0.type == TDB_VAL_BLOB ? "blob" : "text";
    tdb_value_set_text(out, t, -1, 1);
  } else if (!strcasecmp(fn, "round") && (argc == 1 || argc == 2)) {
    double x = tdb_value_as_real(&a0); int nd = 0;
    if (argc == 2) { tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[1], &v); nd = (int)tdb_value_as_int(&v); tdb_value_clear(&v); }
    double p = pow(10.0, (double)nd);
    tdb_value_set_real(out, round(x * p) / p);
  } else if (!strcasecmp(fn, "substr") && (argc == 2 || argc == 3)) {
    const char *s = tdb_value_as_text(&a0);
    if (!s) tdb_value_set_null(out);
    else {
      int slen = (int)strlen(s);
      tdb_value v1; tdb_value_init(&v1); eval(db, c, e->args->items[1], &v1);
      int start = (int)tdb_value_as_int(&v1); tdb_value_clear(&v1);
      int len = slen;
      if (argc == 3) { tdb_value v2; tdb_value_init(&v2); eval(db, c, e->args->items[2], &v2); len = (int)tdb_value_as_int(&v2); tdb_value_clear(&v2); }
      if (start < 0) start = slen + start + 1;
      if (start < 1) start = 1;
      int begin = start - 1; if (begin > slen) begin = slen;
      int avail = slen - begin; if (len < 0) len = 0; if (len > avail) len = avail;
      tdb_value_set_text(out, s + begin, len, 1);
    }
  } else if (!strcasecmp(fn, "replace") && argc == 3) {
    const char *s = tdb_value_as_text(&a0);
    tdb_value vf, vt; tdb_value_init(&vf); tdb_value_init(&vt);
    eval(db, c, e->args->items[1], &vf); eval(db, c, e->args->items[2], &vt);
    const char *from = tdb_value_as_text(&vf), *to = tdb_value_as_text(&vt);
    if (!s || !from || !*from) { if (s) tdb_value_set_text(out, s, -1, 1); else tdb_value_set_null(out); }
    else {
      tdb_buf b; tdb_buf_init(&b);
      size_t fl = strlen(from);
      for (const char *p = s; *p; ) {
        if (strncmp(p, from, fl) == 0) { if (to) tdb_buf_append(&b, to, strlen(to)); p += fl; }
        else { tdb_buf_putc(&b, (uint8_t)*p); p++; }
      }
      tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
      tdb_buf_free(&b);
    }
    tdb_value_clear(&vf); tdb_value_clear(&vt);
  } else if ((!strcasecmp(fn, "trim") || !strcasecmp(fn, "ltrim") || !strcasecmp(fn, "rtrim")) && argc == 1) {
    const char *s = tdb_value_as_text(&a0);
    if (!s) tdb_value_set_null(out);
    else {
      int n = (int)strlen(s), i = 0, j = n;
      if (strcasecmp(fn, "rtrim")) while (i < j && isspace((unsigned char)s[i])) i++;
      if (strcasecmp(fn, "ltrim")) while (j > i && isspace((unsigned char)s[j - 1])) j--;
      tdb_value_set_text(out, s + i, j - i, 1);
    }
  } else if (!strcasecmp(fn, "instr") && argc == 2) {
    const char *s = tdb_value_as_text(&a0);
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[1], &v);
    const char *sub = tdb_value_as_text(&v);
    if (!s || !sub) tdb_value_set_int(out, 0);
    else { const char *hit = strstr(s, sub); tdb_value_set_int(out, hit ? (int64_t)(hit - s) + 1 : 0); }
    tdb_value_clear(&v);
  } else if (!strcasecmp(fn, "current_version") && argc == 0) {
    tdb_value_set_int(out, (int64_t)tdb_pager_max_txnid(db->pager)); /* system-time clock */
  } else {
#ifdef TDB_HAVE_LUA
    if (db->lua && tdb_catalog_find_routine(db->cat, fn)) {
      tdb_value *args = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)(argc ? argc : 1));
      for (int i = 0; i < argc; i++) { tdb_value_init(&args[i]); eval(db, c, e->args->items[i], &args[i]); }
      char *lerr = NULL;
      int rc = tdb_lua_call_scalar(db->lua, fn, args, argc, out, &lerr);
      for (int i = 0; i < argc; i++) tdb_value_clear(&args[i]);
      tdb_mfree(args);
      tdb_value_clear(&a0);
      if (rc) { tdb_db_seterr(db, "%s", lerr ? lerr : "lua error"); tdb_mfree(lerr); return TDB_ERROR; }
      return TDB_OK;
    }
#endif
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
      if (e->subquery) {
        tdb_stmt tmp;
        if (run_subselect(db, c->stmt, e->subquery, &tmp) == TDB_OK) {
          for (int i = 0; i < tmp.nrows && !res; i++)
            if (x.type != TDB_VAL_NULL && tmp.ncol > 0 && tmp.rows[i][0].type != TDB_VAL_NULL &&
                tdb_value_compare(&x, &tmp.rows[i][0], TDB_COLL_BINARY) == 0) res = 1;
        }
        tdb_stmt_clear_results(&tmp);
      } else if (e->args) for (int i = 0; i < e->args->n; i++) {
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
    case EX_SUBQUERY: {
      tdb_stmt tmp;
      if (run_subselect(db, c->stmt, e->subquery, &tmp) == TDB_OK && tmp.nrows > 0 && tmp.ncol > 0)
        tdb_value_copy(out, &tmp.rows[0][0]);
      else tdb_value_set_null(out);
      tdb_stmt_clear_results(&tmp);
      return TDB_OK;
    }
    case EX_EXISTS: {
      tdb_stmt tmp;
      int ex = 0;
      if (run_subselect(db, c->stmt, e->subquery, &tmp) == TDB_OK) ex = (tmp.nrows > 0);
      tdb_stmt_clear_results(&tmp);
      tdb_value_set_int(out, ex);
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

/* ------------------------- left-deep nested join ---------------------- */

/* Fold the per-source row sets left-to-right into combined rows, applying each
** source's join kind + ON, then the WHERE predicate. Supports INNER/CROSS and
** LEFT/RIGHT/FULL outer joins (unmatched sides get NULL-padded). */
static int join_build(tdb_db *db, tdb_stmt *st, qctx *q, rowset *sets,
                      tdb_expr *where, rowset *out) {
  int total = q->totalcols;
  rowset acc; rowset_init(&acc, total);

  for (int i = 0; i < sets[0].n; i++) {
    tdb_value *row = row_alloc(total);
    for (int k = 0; k < q->src[0].ncol; k++)
      tdb_value_copy(&row[q->src[0].base + k], &sets[0].rows[i][k]);
    rowset_add(&acc, row);
  }

  for (int s = 1; s < q->nsrc; s++) {
    qsrc *src = &q->src[s];
    int rn = sets[s].n;
    char *rmatched = (char *)tdb_calloc((size_t)(rn ? rn : 1));
    rowset next; rowset_init(&next, total);

    for (int li = 0; li < acc.n; li++) {
      int lmatched = 0;
      for (int ri = 0; ri < rn; ri++) {
        tdb_value *cand = acc.rows[li];
        for (int k = 0; k < src->ncol; k++)
          tdb_value_copy(&cand[src->base + k], &sets[s].rows[ri][k]);
        int ok = 1;
        if (src->on) {
          ectx ec = { cand, total, NULL, 0, st };
          tdb_value v; tdb_value_init(&v);
          eval(db, &ec, src->on, &v);
          ok = val_true(&v);
          tdb_value_clear(&v);
        }
        if (ok) {
          tdb_value *nr = row_alloc(total);
          for (int k = 0; k < total; k++) tdb_value_copy(&nr[k], &cand[k]);
          rowset_add(&next, nr);
          lmatched = 1; rmatched[ri] = 1;
        }
      }
      for (int k = 0; k < src->ncol; k++) tdb_value_set_null(&acc.rows[li][src->base + k]);
      if (!lmatched && (src->join == JOIN_LEFT || src->join == JOIN_FULL)) {
        tdb_value *nr = row_alloc(total);
        for (int k = 0; k < total; k++) tdb_value_copy(&nr[k], &acc.rows[li][k]);
        rowset_add(&next, nr);
      }
    }
    if (src->join == JOIN_RIGHT || src->join == JOIN_FULL) {
      for (int ri = 0; ri < rn; ri++) if (!rmatched[ri]) {
        tdb_value *nr = row_alloc(total);
        for (int k = 0; k < src->ncol; k++)
          tdb_value_copy(&nr[src->base + k], &sets[s].rows[ri][k]);
        rowset_add(&next, nr);
      }
    }
    tdb_mfree(rmatched);
    rowset_free(&acc);
    acc = next;
  }

  for (int i = 0; i < acc.n; i++) {
    int ok = 1;
    if (where) {
      ectx ec = { acc.rows[i], total, NULL, 0, st };
      tdb_value v; tdb_value_init(&v);
      eval(db, &ec, where, &v);
      ok = val_true(&v);
      tdb_value_clear(&v);
    }
    if (ok) {
      tdb_value *nr = row_alloc(total);
      for (int k = 0; k < total; k++) tdb_value_copy(&nr[k], &acc.rows[i][k]);
      rowset_add(out, nr);
    }
  }
  rowset_free(&acc);
  return TDB_OK;
}

/* ------------------------------ SELECT -------------------------------- */

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

/* ------------------------------- EXPLAIN ------------------------------ */

static void explain_line(tdb_stmt *st, const char *text) {
  tdb_value *row = row_alloc(1);
  tdb_value_set_text(&row[0], text, -1, 1);
  add_result_row(st, row);
}

static void explain_select(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  char line[256];
  if (q->setop) {
    const char *op = q->setop == TK_UNION ? "UNION" : q->setop == TK_EXCEPT ? "EXCEPT" : "INTERSECT";
    snprintf(line, sizeof(line), "COMPOUND %s%s", op, q->setop_all ? " ALL" : "");
    explain_line(st, line);
  }
  for (tdb_src *s = q->from; s; s = s->next) {
    if (s->subquery) { explain_line(st, "SCAN SUBQUERY"); explain_select(db, st, s->subquery); continue; }
    tdb_table *t = tdb_catalog_find_table(db->cat, s->table);
    const char *colmark = (t && t->columnar) ? " (columnar)" : "";
    tdb_index *idx = NULL; tdb_keyrange kr; int chosen = 0;
    if (t && !q->from->next && t->nindex > 0) {
      tdb_value_init(&kr.lo); tdb_value_init(&kr.hi);
      chosen = pick_index(db, st, t, q->where, &idx, &kr);
      tdb_value_clear(&kr.lo); tdb_value_clear(&kr.hi);
    }
    if (chosen) snprintf(line, sizeof(line), "SEARCH %s USING INDEX %s%s", s->table, idx->name, colmark);
    else        snprintf(line, sizeof(line), "SCAN %s%s", s->table ? s->table : "?", colmark);
    explain_line(st, line);
  }
  if (q->setop_next) explain_select(db, st, q->setop_next);
  if (q->group || q->cols) {
    /* note aggregation if any aggregate appears (cheap check: scan projection) */
  }
  if (q->order) explain_line(st, "USE TEMP B-TREE FOR ORDER BY");
}

static int exec_explain(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *inner) {
  st->is_select = 1;
  st->ncol = 1;
  st->colnames = (char **)tdb_malloc(sizeof(char *));
  st->colnames[0] = tdb_strdup("plan");
  char line[256];
  if (!inner) { explain_line(st, "(empty)"); return TDB_OK; }
  switch (inner->kind) {
    case ST_SELECT: explain_select(db, st, inner->u.select); break;
    case ST_INSERT:
      snprintf(line, sizeof(line), "INSERT INTO %s", inner->u.insert.table);
      explain_line(st, line);
      if (inner->u.insert.select) explain_select(db, st, inner->u.insert.select);
      break;
    case ST_UPDATE:
      snprintf(line, sizeof(line), "UPDATE %s (scan)", inner->u.update.table);
      explain_line(st, line); break;
    case ST_DELETE:
      snprintf(line, sizeof(line), "DELETE FROM %s (scan)", inner->u.del.table);
      explain_line(st, line); break;
    case ST_CREATE_TABLE: explain_line(st, "CREATE TABLE"); break;
    case ST_CREATE_INDEX: explain_line(st, "CREATE INDEX (build)"); break;
    default: explain_line(st, "DDL/UTILITY"); break;
  }
  st->cur = -1;
  return TDB_OK;
}

/* ------------------------------ dispatch ------------------------------ */

int tdb_stmt_execute(tdb_stmt *st) {
  tdb_db *db = st->db;
  tdb_ast_stmt *a = st->ast;
  if (!a) return TDB_OK;

  /* transaction-control statements manage db->txn directly */
  if (a->kind == ST_BEGIN || a->kind == ST_COMMIT || a->kind == ST_ROLLBACK ||
      a->kind == ST_SAVEPOINT || a->kind == ST_RELEASE || a->kind == ST_ROLLBACK_TO)
    return exec_txn(db, st, a);

  /* EXPLAIN describes a plan (no execution); VACUUM forces a checkpoint */
  if (a->kind == ST_EXPLAIN) { st->is_select = 1; return exec_explain(db, st, a->u.explain.inner); }
  if (a->kind == ST_VACUUM) {
    if (db->txn) { tdb_db_seterr(db, "cannot VACUUM within a transaction"); return TDB_ERROR; }
    return tdb_pager_checkpoint(db->pager);
  }

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
    case ST_CREATE_ROUTINE: rc = exec_create_routine(db, st, a); break;
    case ST_CALL: rc = exec_call(db, st, a); break;
    case ST_ALTER_TABLE: rc = exec_alter(db, st, a); break;
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
