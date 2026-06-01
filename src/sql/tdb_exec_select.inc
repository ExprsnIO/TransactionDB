/* tdb_exec_select.inc — SELECT execution (included by tdb_exec.c). */

/* per-aggregate accumulator within a group */
typedef struct {
  int64_t  count;
  double   sum;
  int      sum_is_int;
  int64_t  isum;
  tdb_value mn, mx;
  int      has;
  tdb_value *seen;          /* DISTINCT: values already counted */
  int       nseen, capseen;
  tdb_buf   gc;             /* GROUP_CONCAT accumulator */
  int       gc_init;
  char     *gc_sep;         /* separator (default ",") */
} accum;

typedef struct {
  tdb_value *key;     /* group-by key values (ngroup) */
  tdb_value *rep;     /* representative source row (combined) */
  accum     *acc;     /* nagg accumulators */
} group;

static int const_int(tdb_db *db, tdb_stmt *st, tdb_expr *e, int64_t dflt) {
  if (!e) return (int)dflt;
  ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
  tdb_value v; tdb_value_init(&v);
  eval(db, &ec, e, &v);
  int64_t r = (v.type == TDB_VAL_NULL) ? dflt : tdb_value_as_int(&v);
  tdb_value_clear(&v);
  return (int)r;
}

static void accum_update(accum *ac, tdb_expr *agg, tdb_db *db, ectx *ec) {
  int is_gc = agg->name && !strcasecmp(agg->name, "group_concat");
  int is_star = agg->args && agg->args->n == 1 && agg->args->items[0]->kind == EX_STAR;
  if (is_star) { ac->count++; return; }
  if (!agg->args || agg->args->n < 1) { ac->count++; return; }
  tdb_value v; tdb_value_init(&v);
  eval(db, ec, agg->args->items[0], &v);
  if (v.type != TDB_VAL_NULL) {
    if (agg->distinct) {
      for (int i = 0; i < ac->nseen; i++)
        if (tdb_value_compare(&v, &ac->seen[i], TDB_COLL_BINARY) == 0) { tdb_value_clear(&v); return; }
      if (ac->nseen == ac->capseen) {
        ac->capseen = ac->capseen ? ac->capseen * 2 : 8;
        ac->seen = (tdb_value *)tdb_realloc(ac->seen, sizeof(tdb_value) * (size_t)ac->capseen);
      }
      tdb_value_init(&ac->seen[ac->nseen]);
      tdb_value_copy(&ac->seen[ac->nseen], &v);
      ac->nseen++;
    }
    ac->count++;
    if (v.type == TDB_VAL_INT) { ac->isum += v.u.i; } else ac->sum_is_int = 0;
    ac->sum += tdb_value_as_real(&v);
    if (!ac->has) { tdb_value_copy(&ac->mn, &v); tdb_value_copy(&ac->mx, &v); ac->has = 1; }
    else {
      if (tdb_value_compare(&v, &ac->mn, TDB_COLL_BINARY) < 0) { tdb_value_clear(&ac->mn); tdb_value_copy(&ac->mn, &v); }
      if (tdb_value_compare(&v, &ac->mx, TDB_COLL_BINARY) > 0) { tdb_value_clear(&ac->mx); tdb_value_copy(&ac->mx, &v); }
    }
    if (is_gc) {
      if (!ac->gc_init) { tdb_buf_init(&ac->gc); ac->gc_init = 1; }
      if (!ac->gc_sep) {
        if (agg->args->n >= 2) {
          tdb_value sv; tdb_value_init(&sv);
          eval(db, ec, agg->args->items[1], &sv);
          const char *st = tdb_value_as_text(&sv);
          ac->gc_sep = tdb_strdup(st ? st : ",");
          tdb_value_clear(&sv);
        } else ac->gc_sep = tdb_strdup(",");
      }
      if (ac->gc.len > 0) tdb_buf_append(&ac->gc, ac->gc_sep, strlen(ac->gc_sep));
      const char *t = tdb_value_as_text(&v);
      if (t) tdb_buf_append(&ac->gc, t, strlen(t));
    }
  }
  tdb_value_clear(&v);
}

static void accum_result(const accum *ac, tdb_expr *agg, tdb_value *out) {
  const char *fn = agg->name;
  tdb_value_init(out);
  if (!strcasecmp(fn, "count")) tdb_value_set_int(out, ac->count);
  else if (!strcasecmp(fn, "sum") || !strcasecmp(fn, "total")) {
    if (ac->count == 0 && !strcasecmp(fn, "sum")) tdb_value_set_null(out);
    else if (ac->sum_is_int) tdb_value_set_int(out, ac->isum);
    else tdb_value_set_real(out, ac->sum);
  } else if (!strcasecmp(fn, "avg")) {
    if (ac->count == 0) tdb_value_set_null(out);
    else tdb_value_set_real(out, ac->sum / (double)ac->count);
  } else if (!strcasecmp(fn, "min")) {
    if (ac->has) tdb_value_copy(out, &ac->mn); else tdb_value_set_null(out);
  } else if (!strcasecmp(fn, "max")) {
    if (ac->has) tdb_value_copy(out, &ac->mx); else tdb_value_set_null(out);
  } else if (!strcasecmp(fn, "group_concat")) {
    if (!ac->gc_init || ac->gc.len == 0) tdb_value_set_null(out);
    else tdb_value_set_text(out, (const char *)ac->gc.data, (int)ac->gc.len, 1);
  } else tdb_value_set_null(out);
}

/* ----------------------- rule-based index selection ------------------- */

static int expr_is_const(const tdb_expr *e) {
  return e && (e->kind == EX_LITERAL || e->kind == EX_NULL || e->kind == EX_PARAM);
}

/* Constant-foldable: evaluable without a row — literals, params, and pure
** expressions/functions over foldable operands (e.g. ST_GeomFromText('...'),
** ST_Point(5,5)). Excludes column/aggregate/subquery references. */
static int expr_is_foldable(const tdb_expr *e) {
  if (!e) return 1;
  switch (e->kind) {
    case EX_LITERAL: case EX_NULL: case EX_PARAM: return 1;
    case EX_COLUMN: case EX_AGG: case EX_SUBQUERY: case EX_EXISTS: case EX_STAR:
      return 0;
    case EX_FUNC: case EX_CAST: case EX_UNARY: case EX_BINARY:
    case EX_IN: case EX_BETWEEN: case EX_CASE:
      if (!expr_is_foldable(e->left) || !expr_is_foldable(e->right)) return 0;
      if (e->args) for (int i = 0; i < e->args->n; i++)
        if (!expr_is_foldable(e->args->items[i])) return 0;
      return 1;
    default: return 0;
  }
}
static int flip_op(int op) {
  switch (op) {
    case TK_LT: return TK_GT; case TK_LE: return TK_GE;
    case TK_GT: return TK_LT; case TK_GE: return TK_LE;
    default: return op;
  }
}

/* Examine a predicate (descending top-level ANDs) for `lead OP const` and fold
** the bound into the range; sets *got when any usable bound is found. */
static void apply_pred(tdb_db *db, tdb_stmt *st, const char *lead, tdb_expr *p,
                       tdb_keyrange *r, int *got) {
  if (!p) return;
  if (p->kind == EX_BINARY && p->op == TK_AND) {
    apply_pred(db, st, lead, p->left, r, got);
    apply_pred(db, st, lead, p->right, r, got);
    return;
  }
  if (p->kind != EX_BINARY) return;
  int op = p->op;
  tdb_expr *L = p->left, *R = p->right, *val = NULL;
  if (L && L->kind == EX_COLUMN && !L->table && L->name && !strcasecmp(L->name, lead) && expr_is_const(R)) {
    val = R;
  } else if (R && R->kind == EX_COLUMN && !R->table && R->name && !strcasecmp(R->name, lead) && expr_is_const(L)) {
    val = L; op = flip_op(op);
  } else return;

  tdb_value v; tdb_value_init(&v);
  ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
  eval(db, &ec, val, &v);
  if (v.type == TDB_VAL_NULL) { tdb_value_clear(&v); return; }
  switch (op) {
    case TK_EQ:
      tdb_value_clear(&r->lo); tdb_value_copy(&r->lo, &v); r->has_lo = 1; r->lo_incl = 1;
      tdb_value_clear(&r->hi); tdb_value_copy(&r->hi, &v); r->has_hi = 1; r->hi_incl = 1; *got = 1; break;
    case TK_GT: tdb_value_clear(&r->lo); tdb_value_copy(&r->lo, &v); r->has_lo = 1; r->lo_incl = 0; *got = 1; break;
    case TK_GE: tdb_value_clear(&r->lo); tdb_value_copy(&r->lo, &v); r->has_lo = 1; r->lo_incl = 1; *got = 1; break;
    case TK_LT: tdb_value_clear(&r->hi); tdb_value_copy(&r->hi, &v); r->has_hi = 1; r->hi_incl = 0; *got = 1; break;
    case TK_LE: tdb_value_clear(&r->hi); tdb_value_copy(&r->hi, &v); r->has_hi = 1; r->hi_incl = 1; *got = 1; break;
    default: break;
  }
  tdb_value_clear(&v);
}

/* True if `e` is a reference to the (unqualified or matching) column `col`. */
static int expr_is_col(const tdb_expr *e, const char *col) {
  return e && e->kind == EX_COLUMN && !e->table && e->name && !strcasecmp(e->name, col);
}

/* Evaluate a constant geometry expression to its bounding box. */
static int const_geom_bbox(tdb_db *db, tdb_stmt *st, tdb_expr *e, tdb_bbox *out) {
  if (!expr_is_foldable(e)) return 0;
  ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
  tdb_value v; tdb_value_init(&v);
  eval(db, &ec, e, &v);
  int ok = (v.type == TDB_VAL_BLOB && v.u.s.p &&
            tdb_geom_bbox((const uint8_t *)v.u.s.p, (int)v.u.s.n, out) == TDB_OK);
  tdb_value_clear(&v);
  return ok;
}

/* Look for a spatial predicate over `col` from which an R-tree query window can
** be derived: ST_Contains/ST_Within/ST_Intersects(col, const-geom) or
** ST_DWithin(col, const-geom, const-dist). Descends top-level ANDs. */
static int spatial_window(tdb_db *db, tdb_stmt *st, const char *col,
                          tdb_expr *p, tdb_bbox *out) {
  if (!p) return 0;
  if (p->kind == EX_BINARY && p->op == TK_AND)
    return spatial_window(db, st, col, p->left, out) ||
           spatial_window(db, st, col, p->right, out);
  if (p->kind != EX_FUNC || !p->name || !p->args || p->args->n < 2) return 0;
  const char *fn = p->name;
  int dwithin = !strcasecmp(fn, "st_dwithin");
  if (!dwithin && strcasecmp(fn, "st_contains") && strcasecmp(fn, "st_within") &&
      strcasecmp(fn, "st_intersects")) return 0;

  tdb_expr *a0 = p->args->items[0], *a1 = p->args->items[1];
  tdb_expr *geomarg;
  if (expr_is_col(a0, col)) geomarg = a1;
  else if (expr_is_col(a1, col)) geomarg = a0;
  else return 0;

  if (!const_geom_bbox(db, st, geomarg, out)) return 0;

  if (dwithin && p->args->n >= 3 && expr_is_foldable(p->args->items[2])) {
    ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
    tdb_value d; tdb_value_init(&d);
    eval(db, &ec, p->args->items[2], &d);
    double dd = tdb_value_as_real(&d);
    tdb_value_clear(&d);
    out->minx -= dd; out->miny -= dd; out->maxx += dd; out->maxy += dd;
  }
  return 1;
}

/* Choose an index for a single-table WHERE: an R-tree for a spatial predicate,
** otherwise a B-tree whose leading column has an equality/range bound.
** Returns 1 and fills *out_idx / *out_range on success. */
static int pick_index(tdb_db *db, tdb_stmt *st, tdb_table *t, tdb_expr *where,
                      tdb_index **out_idx, tdb_keyrange *out_range) {
  if (!where) return 0;
  for (int xi = 0; xi < t->nindex; xi++) {
    tdb_index *ix = &t->indexes[xi];
    if (ix->ncol < 1) continue;
    const char *lead = t->cols[ix->col_idx[0]].name;
    if (ix->kind == TDB_IDX_RTREE) {
      tdb_bbox q;
      if (spatial_window(db, st, lead, where, &q)) {
        memset(out_range, 0, sizeof(*out_range));
        tdb_value_init(&out_range->lo); tdb_value_init(&out_range->hi);
        out_range->has_bbox = 1; out_range->bbox = q;
        *out_idx = ix; return 1;
      }
      continue;
    }
    tdb_keyrange r; memset(&r, 0, sizeof(r));
    tdb_value_init(&r.lo); tdb_value_init(&r.hi);
    int got = 0;
    apply_pred(db, st, lead, where, &r, &got);
    if (got) { *out_idx = ix; *out_range = r; return 1; }
    tdb_value_clear(&r.lo); tdb_value_clear(&r.hi);
  }
  return 0;
}

/* mark columns of `t` referenced by an expression (projection pushdown).
** Conservative: a subquery marks everything (in case it is correlated). */
static void mark_cols(tdb_table *t, tdb_expr *e, uint8_t *mask) {
  if (!e) return;
  switch (e->kind) {
    case EX_STAR:
      for (int i = 0; i < t->ncol; i++) mask[i] = 1;
      return;
    case EX_COLUMN:
      if (e->name) { int ci = tdb_table_find_column(t, e->name); if (ci >= 0) mask[ci] = 1; }
      return;
    case EX_SUBQUERY: case EX_EXISTS:
      for (int i = 0; i < t->ncol; i++) mask[i] = 1;
      return;
    default:
      mark_cols(t, e->left, mask);
      mark_cols(t, e->right, mask);
      if (e->args) for (int i = 0; i < e->args->n; i++) mark_cols(t, e->args->items[i], mask);
      return;
  }
}

/* Build a needed-column mask for a lone base table; returns 1 if computed. */
static int compute_colmask(tdb_table *t, tdb_select *q, uint8_t *mask) {
  if (t->ncol > 64) return 0;
  memset(mask, 0, (size_t)t->ncol);
  if (q->cols) for (int i = 0; i < q->cols->n; i++) mark_cols(t, q->cols->items[i], mask);
  mark_cols(t, q->where, mask);
  if (q->group) for (int i = 0; i < q->group->n; i++) mark_cols(t, q->group->items[i], mask);
  mark_cols(t, q->having, mask);
  if (q->order) for (int i = 0; i < q->order->n; i++) mark_cols(t, q->order->exprs[i], mask);
  return 1;
}

/* Build "<prev>.col = <cur>.col" as a single equi-predicate expression. */
static tdb_expr *make_join_eq(tdb_arena *a, qsrc *prev, qsrc *cur, const char *col) {
  tdb_expr *l = tdb_expr_new(a, EX_COLUMN);
  l->table = (char *)(prev->alias ? prev->alias : (prev->tbl ? prev->tbl->name : ""));
  l->name = tdb_arena_strndup(a, col, strlen(col));
  tdb_expr *r = tdb_expr_new(a, EX_COLUMN);
  r->table = (char *)(cur->alias ? cur->alias : (cur->tbl ? cur->tbl->name : ""));
  r->name = tdb_arena_strndup(a, col, strlen(col));
  tdb_expr *eq = tdb_expr_new(a, EX_BINARY);
  eq->op = TK_EQ; eq->left = l; eq->right = r;
  return eq;
}

/* Conjoin two predicates with AND, allowing either to be NULL. */
static tdb_expr *and_expr(tdb_arena *a, tdb_expr *l, tdb_expr *r) {
  if (!l) return r;
  if (!r) return l;
  tdb_expr *e = tdb_expr_new(a, EX_BINARY);
  e->op = TK_AND; e->left = l; e->right = r;
  return e;
}

/* True if column `name` exists in qsrc `s` (base table or derived). */
static int qsrc_has_col(qsrc *s, const char *name) {
  if (s->tbl) return tdb_table_find_column(s->tbl, name) >= 0;
  for (int i = 0; i < s->ncol; i++)
    if (s->colnames[i] && !strcasecmp(s->colnames[i], name)) return 1;
  return 0;
}

/* Iterate the column names of qsrc `s` (callback receives name). */
static const char *qsrc_col_name(qsrc *s, int i) {
  if (s->tbl) return (i >= 0 && i < s->tbl->ncol) ? s->tbl->cols[i].name : NULL;
  return (i >= 0 && i < s->ncol) ? s->colnames[i] : NULL;
}

/* Synthesize the join condition for a USING or NATURAL join. Updates qs->on so
** the standard join code path (and resolve_expr) treats it like an ON. */
static void synth_join_on(tdb_stmt *st, tdb_src *s, qctx *Q, int idx) {
  if (idx <= 0) return;
  qsrc *prev = &Q->src[idx - 1];
  qsrc *cur  = &Q->src[idx];
  tdb_expr *cond = NULL;
  if (s->nusing > 0) {
    for (int i = 0; i < s->nusing; i++)
      cond = and_expr(st->arena, cond, make_join_eq(st->arena, prev, cur, s->using_cols[i]));
  } else if (s->natural) {
    int n = cur->tbl ? cur->tbl->ncol : cur->ncol;
    for (int i = 0; i < n; i++) {
      const char *cn = qsrc_col_name(cur, i);
      if (cn && qsrc_has_col(prev, cn))
        cond = and_expr(st->arena, cond, make_join_eq(st->arena, prev, cur, cn));
    }
  }
  if (cond) cur->on = cond;
}

/* Resolve a FROM item (base table / derived table / view) and materialize its
** rows into `set`, filling Q->src[idx] (caller advances totalcols/nsrc). */
static int materialize_source(tdb_db *db, tdb_stmt *st, qctx *Q, int idx,
                              tdb_src *s, tdb_index *use_idx,
                              const tdb_keyrange *range, const uint8_t *colmask,
                              rowset *set) {
  qsrc *qs = &Q->src[idx];
  memset(qs, 0, sizeof(*qs));
  qs->alias = s->alias; qs->base = Q->totalcols; qs->join = s->join; qs->on = s->on;

  tdb_select *sub = s->subquery;
  tdb_cte    *cte = NULL;
  if (!sub && s->table) {
    cte = find_cte(st, s->table);              /* a WITH name shadows a table */
    if (cte) { sub = cte->select; if (!qs->alias) qs->alias = cte->name; }
  }
  if (!sub && s->table) {
    tdb_table *t = tdb_catalog_find_table(db->cat, s->table);
    if (t) {
      qs->tbl = t; qs->ncol = t->ncol;
      tdb_txnid as_of = 0;
      if (s->as_of) {
        ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
        tdb_value v; tdb_value_init(&v);
        eval(db, &ec, s->as_of, &v);
        as_of = (tdb_txnid)tdb_value_as_int(&v);
        tdb_value_clear(&v);
      }
      return mat_table(db, t, use_idx, range, as_of, colmask, set);
    }
    tdb_view *v = tdb_catalog_find_view(db->cat, s->table);
    if (!v) { tdb_db_seterr(db, "no such table: %s", s->table); return TDB_ERROR; }
    if (!qs->alias) qs->alias = v->name;
    tdb_ast_stmt *vast = NULL; char *verr = NULL;
    if (tdb_parse(st->arena, v->select_sql, &vast, &verr, NULL) != TDB_OK ||
        !vast || vast->kind != ST_SELECT) {
      tdb_db_seterr(db, "invalid view: %s", s->table); return TDB_ERROR;
    }
    sub = vast->u.select;
  }
  if (!sub) { tdb_db_seterr(db, "invalid FROM item"); return TDB_ERROR; }

  tdb_stmt tmp;
  if (cte) cte->active = 1;                  /* block self-reference while running */
  int rc = run_subselect(db, st, sub, &tmp, NULL);
  if (cte) cte->active = 0;
  if (rc) { tdb_stmt_clear_results(&tmp); return rc; }
  qs->ncol = tmp.ncol;
  qs->colnames = (char **)tdb_arena_alloc(st->arena, sizeof(char *) * (size_t)(tmp.ncol ? tmp.ncol : 1));
  for (int k = 0; k < tmp.ncol; k++) {
    /* WITH name(col, ...) column aliases override the body's output names */
    const char *nm = (cte && k < cte->ncolname) ? cte->colnames[k]
                   : (tmp.colnames && tmp.colnames[k]) ? tmp.colnames[k] : "col";
    qs->colnames[k] = tdb_arena_strndup(st->arena, nm, strlen(nm));
  }
  rowset_init(set, tmp.ncol ? tmp.ncol : 1);
  for (int r = 0; r < tmp.nrows; r++) {
    tdb_value *row = row_alloc(tmp.ncol ? tmp.ncol : 1);
    for (int k = 0; k < tmp.ncol; k++) tdb_value_copy(&row[k], &tmp.rows[r][k]);
    rowset_add(set, row);
  }
  tdb_stmt_clear_results(&tmp);
  return TDB_OK;
}

static int exec_setop_chain(tdb_db *db, tdb_stmt *st, tdb_select *q, ectx *outer);
static int exec_select_body(tdb_db *db, tdb_stmt *st, tdb_select *q, ectx *outer);

/* Push this SELECT's WITH frame (if any) so its CTEs are visible to the body
** and any nested subqueries, run the body, then pop the frame. `outer` is the
** enclosing query's per-row context for correlated subqueries (NULL at top). */
static int exec_select(tdb_db *db, tdb_stmt *st, tdb_select *q, ectx *outer) {
  cte_scope scope; cte_scope *saved = st->cte_scope;
  if (q->with) { scope.list = q->with; scope.parent = saved; st->cte_scope = &scope; }
  int rc = exec_select_body(db, st, q, outer);
  st->cte_scope = saved;
  return rc;
}

static int exec_select_body(tdb_db *db, tdb_stmt *st, tdb_select *q, ectx *outer) {
  if (q->setop) return exec_setop_chain(db, st, q, outer);

  qctx Q; memset(&Q, 0, sizeof(Q));
  Q.outer = outer ? outer->qc : NULL;   /* correlated name resolution */
  rowset sets[MAXSRC];

  /* rule-based index pick: a lone base table with an indexable WHERE */
  tdb_index *use_idx = NULL; tdb_keyrange krange; int have_idx = 0;
  tdb_value_init(&krange.lo); tdb_value_init(&krange.hi);
  uint8_t colmask[64]; int have_mask = 0;
  if (q->from && !q->from->next && !q->from->subquery && q->from->table &&
      !find_cte(st, q->from->table)) {
    tdb_table *bt = tdb_catalog_find_table(db->cat, q->from->table);
    if (bt && bt->nindex > 0 && pick_index(db, st, bt, q->where, &use_idx, &krange))
      have_idx = 1;
    if (bt) have_mask = compute_colmask(bt, q, colmask);
  }

  /* 1. sources: base table / derived table / view — materialize each */
  for (tdb_src *s = q->from; s; s = s->next) {
    if (Q.nsrc >= MAXSRC) {
      tdb_db_seterr(db, "too many tables in FROM");
      for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]);
      tdb_value_clear(&krange.lo); tdb_value_clear(&krange.hi);
      return TDB_ERROR;
    }
    int idx = Q.nsrc;
    tdb_index *si = (idx == 0 && have_idx) ? use_idx : NULL;
    const tdb_keyrange *sr = (idx == 0 && have_idx) ? &krange : NULL;
    const uint8_t *cm = (idx == 0 && have_mask) ? colmask : NULL;
    int rc = materialize_source(db, st, &Q, idx, s, si, sr, cm, &sets[idx]);
    if (rc) { for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]);
              tdb_value_clear(&krange.lo); tdb_value_clear(&krange.hi); return rc; }
    Q.totalcols += Q.src[idx].ncol;
    Q.nsrc++;
    /* USING (..) / NATURAL JOIN: build the implicit ON from column names. */
    if ((s->nusing > 0 || s->natural) && !Q.src[idx].on)
      synth_join_on(st, s, &Q, idx);
  }

  /* 2. resolve ON conditions + clauses (collect aggregates) */
  #define FAIL_SETS do { for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]); \
    tdb_value_clear(&krange.lo); tdb_value_clear(&krange.hi); } while (0)
  for (int s = 0; s < Q.nsrc; s++)
    if (Q.src[s].on && resolve_expr(db, &Q, Q.src[s].on)) { FAIL_SETS; return TDB_ERROR; }
  if (q->where && resolve_expr(db, &Q, q->where)) { FAIL_SETS; return TDB_ERROR; }
  if (q->group && resolve_list(db, &Q, q->group)) { FAIL_SETS; return TDB_ERROR; }

  /* projection: expand '*', resolve explicit exprs, build names */
  tdb_expr **oexpr = NULL; char **onames = NULL; int nout = 0, ocap = 0;
  #define ADD_OUT(E, NM) do { \
    if (nout == ocap) { ocap = ocap ? ocap*2 : 8; \
      oexpr = (tdb_expr**)tdb_realloc(oexpr, sizeof(void*)*(size_t)ocap); \
      onames = (char**)tdb_realloc(onames, sizeof(void*)*(size_t)ocap); } \
    oexpr[nout] = (E); onames[nout] = tdb_strdup((NM) ? (NM) : "column"); nout++; \
  } while (0)
  #define FAILP do { FAIL_SETS; tdb_mfree(oexpr); tdb_mfree(onames); return TDB_ERROR; } while (0)

  for (int i = 0; i < q->cols->n; i++) {
    tdb_expr *item = q->cols->items[i];
    char *alias = q->cols->aliases[i];
    if (item->kind == EX_STAR) {
      for (int s = 0; s < Q.nsrc; s++) {
        qsrc *qs = &Q.src[s];
        if (item->table) {
          const char *nm = qs->alias ? qs->alias : (qs->tbl ? qs->tbl->name : NULL);
          if (!nm || strcasecmp(nm, item->table) != 0) continue;
        }
        for (int k = 0; k < qs->ncol; k++) {
          const char *cn;
          if (qs->tbl) { if (qs->tbl->cols[k].hidden) continue; cn = qs->tbl->cols[k].name; }
          else cn = qs->colnames[k];
          tdb_expr *col = tdb_expr_new(st->arena, EX_COLUMN);
          col->name = (char *)cn;
          col->col_index = qs->base + k;
          ADD_OUT(col, cn);
        }
      }
    } else {
      if (resolve_expr(db, &Q, item)) FAILP;
      const char *nm = alias ? alias : (item->kind == EX_COLUMN ? item->name : NULL);
      ADD_OUT(item, nm);
    }
  }
  if (q->having && resolve_expr(db, &Q, q->having)) FAILP;
  int norder = q->order ? q->order->n : 0;
  for (int i = 0; i < norder; i++)
    if (resolve_expr(db, &Q, q->order->exprs[i])) FAILP;

  int aggregated = (Q.nagg > 0 || q->group != NULL);

  /* 3. join the materialized sources (INNER/CROSS/LEFT/RIGHT/FULL + WHERE) */
  rowset in; rowset_init(&in, Q.totalcols ? Q.totalcols : 1);
  if (Q.nsrc == 0) {
    rowset_add(&in, row_alloc(1));   /* SELECT with no FROM: single empty row */
  } else {
    join_build(db, st, &Q, sets, q->where, &in, outer);
  }
  for (int s = 0; s < Q.nsrc; s++) rowset_free(&sets[s]);

  /* 4. build output rows (+ order keys) */
  tdb_value **outrows = NULL; tdb_value **outkeys = NULL; int non = 0, oncap = 0;
  #define ADD_RESROW(VR, KR) do { \
    if (non == oncap) { oncap = oncap ? oncap*2 : 16; \
      outrows = (tdb_value**)tdb_realloc(outrows, sizeof(void*)*(size_t)oncap); \
      outkeys = (tdb_value**)tdb_realloc(outkeys, sizeof(void*)*(size_t)oncap); } \
    outrows[non] = (VR); outkeys[non] = (KR); non++; \
  } while (0)

  int rc = TDB_OK;
  int ngroup = q->group ? q->group->n : 0;

  if (!aggregated) {
    for (int r = 0; r < in.n; r++) {
      ectx ec = { in.rows[r], in.ncol, NULL, 0, st, &Q, outer };
      tdb_value *vr = row_alloc(nout);
      for (int i = 0; i < nout; i++) eval(db, &ec, oexpr[i], &vr[i]);
      tdb_value *kr = norder ? row_alloc(norder) : NULL;
      for (int i = 0; i < norder; i++) eval(db, &ec, q->order->exprs[i], &kr[i]);
      ADD_RESROW(vr, kr);
    }
  } else {
    /* group rows */
    group *groups = NULL; int ng = 0, gcap = 0;
    for (int r = 0; r < in.n; r++) {
      ectx ec = { in.rows[r], in.ncol, NULL, 0, st, &Q, outer };
      /* compute group key */
      tdb_value *key = ngroup ? row_alloc(ngroup) : NULL;
      for (int i = 0; i < ngroup; i++) eval(db, &ec, q->group->items[i], &key[i]);
      int gi = -1;
      for (int g = 0; g < ng; g++) {
        int same = 1;
        for (int i = 0; i < ngroup; i++)
          if (tdb_value_compare(&groups[g].key[i], &key[i], TDB_COLL_BINARY) != 0) { same = 0; break; }
        if (same) { gi = g; break; }
      }
      if (gi < 0) {
        if (ng == gcap) { gcap = gcap ? gcap*2 : 8; groups = (group*)tdb_realloc(groups, sizeof(group)*(size_t)gcap); }
        gi = ng++;
        groups[gi].key = key; key = NULL;
        groups[gi].rep = in.rows[r];
        groups[gi].acc = (accum*)tdb_calloc(sizeof(accum) * (size_t)(Q.nagg ? Q.nagg : 1));
        for (int j = 0; j < Q.nagg; j++) { groups[gi].acc[j].sum_is_int = 1; tdb_value_init(&groups[gi].acc[j].mn); tdb_value_init(&groups[gi].acc[j].mx); }
      }
      if (key) { for (int i = 0; i < ngroup; i++) tdb_value_clear(&key[i]); tdb_mfree(key); }
      for (int j = 0; j < Q.nagg; j++) accum_update(&groups[gi].acc[j], Q.aggs[j], db, &ec);
    }
    /* aggregates with no GROUP BY and no rows still yield one row */
    if (ng == 0 && ngroup == 0) {
      groups = (group*)tdb_calloc(sizeof(group));
      groups[0].acc = (accum*)tdb_calloc(sizeof(accum) * (size_t)(Q.nagg ? Q.nagg : 1));
      for (int j = 0; j < Q.nagg; j++) groups[0].acc[j].sum_is_int = 1;
      groups[0].rep = NULL; /* no rows: aggregates don't read a source row */
      ng = 1;
    }
    for (int g = 0; g < ng; g++) {
      tdb_value *aggvals = Q.nagg ? row_alloc(Q.nagg) : NULL;
      for (int j = 0; j < Q.nagg; j++) accum_result(&groups[g].acc[j], Q.aggs[j], &aggvals[j]);
      ectx ec = { groups[g].rep, in.ncol, aggvals, Q.nagg, st, &Q, outer };
      int keep = 1;
      if (q->having) { tdb_value hv; tdb_value_init(&hv); eval(db, &ec, q->having, &hv); keep = val_true(&hv); tdb_value_clear(&hv); }
      if (keep) {
        tdb_value *vr = row_alloc(nout);
        for (int i = 0; i < nout; i++) eval(db, &ec, oexpr[i], &vr[i]);
        tdb_value *kr = norder ? row_alloc(norder) : NULL;
        for (int i = 0; i < norder; i++) eval(db, &ec, q->order->exprs[i], &kr[i]);
        ADD_RESROW(vr, kr);
      }
      if (aggvals) free_row(aggvals, Q.nagg);
      /* free group */
      if (groups[g].key) { for (int i = 0; i < ngroup; i++) tdb_value_clear(&groups[g].key[i]); tdb_mfree(groups[g].key); }
      for (int j = 0; j < Q.nagg; j++) {
        tdb_value_clear(&groups[g].acc[j].mn); tdb_value_clear(&groups[g].acc[j].mx);
        for (int k = 0; k < groups[g].acc[j].nseen; k++) tdb_value_clear(&groups[g].acc[j].seen[k]);
        tdb_mfree(groups[g].acc[j].seen);
        if (groups[g].acc[j].gc_init) tdb_buf_free(&groups[g].acc[j].gc);
        tdb_mfree(groups[g].acc[j].gc_sep);
      }
      tdb_mfree(groups[g].acc);
    }
    tdb_mfree(groups);
  }

  rowset_free(&in);

  /* 4b. DISTINCT — drop duplicate output rows (NULLs compare equal) */
  if (q->distinct && nout > 0 && non > 1) {
    for (int i = 0; i < non; i++) {
      for (int j = i + 1; j < non; ) {
        int same = 1;
        for (int k = 0; k < nout; k++)
          if (tdb_value_compare(&outrows[i][k], &outrows[j][k], TDB_COLL_BINARY) != 0) { same = 0; break; }
        if (same) {
          free_row(outrows[j], nout);
          if (outkeys[j]) free_row(outkeys[j], norder);
          for (int m = j; m < non - 1; m++) { outrows[m] = outrows[m + 1]; outkeys[m] = outkeys[m + 1]; }
          non--;
        } else j++;
      }
    }
  }

  /* 5. ORDER BY */
  if (norder > 0 && non > 1) {
    int *idx = (int *)tdb_malloc(sizeof(int) * (size_t)non);
    for (int i = 0; i < non; i++) idx[i] = i;
    sortinfo si; si.keys = outkeys; si.desc = q->order->desc; si.nkey = norder; si.nrows = non;
    g_sort = &si;
    qsort(idx, (size_t)non, sizeof(int), sort_cmp);
    g_sort = NULL;
    tdb_value **nr = (tdb_value **)tdb_malloc(sizeof(void *) * (size_t)non);
    for (int i = 0; i < non; i++) nr[i] = outrows[idx[i]];
    tdb_mfree(outrows); outrows = nr;
    tdb_mfree(idx);
  }

  /* 6. LIMIT / OFFSET, then publish */
  int limit = q->has_limit ? const_int(db, st, q->limit, -1) : -1;
  int offset = q->offset ? const_int(db, st, q->offset, 0) : 0;
  if (offset < 0) offset = 0;

  st->ncol = nout;
  st->colnames = onames;
  for (int i = 0; i < non; i++) {
    if (i < offset) { free_row(outrows[i], nout); if (outkeys[i]) free_row(outkeys[i], norder); continue; }
    if (limit >= 0 && st->nrows >= limit) { free_row(outrows[i], nout); if (outkeys[i]) free_row(outkeys[i], norder); continue; }
    add_result_row(st, outrows[i]);
    if (outkeys[i]) free_row(outkeys[i], norder);
  }
  tdb_mfree(outrows); tdb_mfree(outkeys);
  tdb_mfree(oexpr);
  tdb_value_clear(&krange.lo); tdb_value_clear(&krange.hi);
  st->cur = -1;
  return rc;
  #undef ADD_OUT
  #undef ADD_RESROW
  #undef FAILP
  #undef FAIL_SETS
}

/* ------------------------- set operations ----------------------------- */

static int row_equal(tdb_value *a, tdb_value *b, int ncol) {
  for (int i = 0; i < ncol; i++)
    if (tdb_value_compare(&a[i], &b[i], TDB_COLL_BINARY) != 0) return 0;
  return 1;
}
static int in_rows(tdb_value **rows, int n, tdb_value *row, int ncol) {
  for (int i = 0; i < n; i++) if (row_equal(rows[i], row, ncol)) return 1;
  return 0;
}
static tdb_value *dup_row(tdb_value *r, int ncol) {
  tdb_value *o = row_alloc(ncol);
  for (int i = 0; i < ncol; i++) tdb_value_copy(&o[i], &r[i]);
  return o;
}

/* run one SELECT node ignoring its set-op chain, into `tmp` */
static int run_single(tdb_db *db, tdb_stmt *parent, tdb_select *sel, tdb_stmt *tmp,
                      ectx *outer) {
  int sop = sel->setop; tdb_select *nx = sel->setop_next;
  sel->setop = 0; sel->setop_next = NULL;
  int rc = run_subselect(db, parent, sel, tmp, outer);
  sel->setop = sop; sel->setop_next = nx;
  return rc;
}

typedef struct { tdb_value **rows; int n, cap; } accset;
static void acc_add(accset *a, tdb_value *row) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16;
    a->rows = (tdb_value **)tdb_realloc(a->rows, sizeof(void *) * (size_t)a->cap); }
  a->rows[a->n++] = row;
}

/* sort the combined result by a trailing ORDER BY whose terms reference output
** columns by name or 1-based ordinal */
static void sort_result(tdb_value **rows, int n, int ncol, char **names,
                        tdb_orderby *ob) {
  int nk = ob->n;
  tdb_value **keys = (tdb_value **)tdb_malloc(sizeof(void *) * (size_t)n);
  for (int r = 0; r < n; r++) {
    keys[r] = row_alloc(nk);
    for (int k = 0; k < nk; k++) {
      tdb_expr *e = ob->exprs[k];
      int ci = -1;
      if (e->kind == EX_LITERAL && e->lit.type == TDB_VAL_INT) ci = (int)e->lit.u.i - 1;
      else if (e->kind == EX_COLUMN && e->name)
        for (int c = 0; c < ncol; c++) if (names[c] && !strcasecmp(names[c], e->name)) { ci = c; break; }
      if (ci >= 0 && ci < ncol) tdb_value_copy(&keys[r][k], &rows[r][ci]);
    }
  }
  int *idx = (int *)tdb_malloc(sizeof(int) * (size_t)n);
  for (int i = 0; i < n; i++) idx[i] = i;
  sortinfo si; si.keys = keys; si.desc = ob->desc; si.nkey = nk; si.nrows = n;
  g_sort = &si; qsort(idx, (size_t)n, sizeof(int), sort_cmp); g_sort = NULL;
  tdb_value **tmp = (tdb_value **)tdb_malloc(sizeof(void *) * (size_t)n);
  for (int i = 0; i < n; i++) tmp[i] = rows[idx[i]];
  for (int i = 0; i < n; i++) rows[i] = tmp[i];
  for (int i = 0; i < n; i++) free_row(keys[i], nk);
  tdb_mfree(keys); tdb_mfree(idx); tdb_mfree(tmp);
}

static int exec_setop_chain(tdb_db *db, tdb_stmt *st, tdb_select *q, ectx *outer) {
  /* the trailing ORDER BY/LIMIT (parsed onto the last SELECT) binds to the
  ** whole result: strip it, combine operands, then apply it */
  tdb_select *last = q;
  while (last->setop_next) last = last->setop_next;
  tdb_orderby *o = last->order; int hl = last->has_limit;
  tdb_expr *lim = last->limit, *off = last->offset;
  last->order = NULL; last->has_limit = 0; last->limit = NULL; last->offset = NULL;

  accset acc; memset(&acc, 0, sizeof(acc));
  char **names = NULL; int ncol = 0; int rc;

  tdb_stmt t0;
  rc = run_single(db, st, q, &t0, outer);
  if (rc) { tdb_stmt_clear_results(&t0); goto restore; }
  ncol = t0.ncol;
  names = (char **)tdb_calloc(sizeof(char *) * (size_t)(ncol ? ncol : 1));
  for (int i = 0; i < ncol; i++) names[i] = tdb_strdup(t0.colnames[i] ? t0.colnames[i] : "col");
  for (int r = 0; r < t0.nrows; r++) acc_add(&acc, dup_row(t0.rows[r], ncol));
  tdb_stmt_clear_results(&t0);

  for (tdb_select *node = q; node->setop; node = node->setop_next) {
    tdb_stmt tb;
    rc = run_single(db, st, node->setop_next, &tb, outer);
    if (rc) { tdb_stmt_clear_results(&tb); goto done; }
    int op = node->setop, all = node->setop_all;

    accset out; memset(&out, 0, sizeof(out));
    if (op == TK_UNION) {
      for (int i = 0; i < acc.n; i++)
        if (all || !in_rows(out.rows, out.n, acc.rows[i], ncol)) acc_add(&out, dup_row(acc.rows[i], ncol));
      for (int r = 0; r < tb.nrows; r++)
        if (all || !in_rows(out.rows, out.n, tb.rows[r], ncol)) acc_add(&out, dup_row(tb.rows[r], ncol));
    } else if (op == TK_INTERSECT) {
      for (int i = 0; i < acc.n; i++)
        if (in_rows(tb.rows, tb.nrows, acc.rows[i], ncol) &&
            !in_rows(out.rows, out.n, acc.rows[i], ncol)) acc_add(&out, dup_row(acc.rows[i], ncol));
    } else { /* EXCEPT */
      for (int i = 0; i < acc.n; i++)
        if (!in_rows(tb.rows, tb.nrows, acc.rows[i], ncol) &&
            !in_rows(out.rows, out.n, acc.rows[i], ncol)) acc_add(&out, dup_row(acc.rows[i], ncol));
    }
    for (int i = 0; i < acc.n; i++) free_row(acc.rows[i], ncol);
    tdb_mfree(acc.rows);
    acc = out;
    tdb_stmt_clear_results(&tb);
  }

  if (o) sort_result(acc.rows, acc.n, ncol, names, o);
  int limit = hl ? const_int(db, st, lim, -1) : -1;
  int offset = off ? const_int(db, st, off, 0) : 0;
  if (offset < 0) offset = 0;
  st->ncol = ncol; st->colnames = names; names = NULL;
  for (int i = 0; i < acc.n; i++) {
    if (i < offset || (limit >= 0 && st->nrows >= limit)) { free_row(acc.rows[i], ncol); continue; }
    add_result_row(st, acc.rows[i]);
  }
  tdb_mfree(acc.rows);
  st->cur = -1;
  rc = TDB_OK;
  goto restore;

done:
  for (int i = 0; i < acc.n; i++) free_row(acc.rows[i], ncol);
  tdb_mfree(acc.rows);
restore:
  if (names) { for (int i = 0; i < ncol; i++) tdb_mfree(names[i]); tdb_mfree(names); }
  last->order = o; last->has_limit = hl; last->limit = lim; last->offset = off;
  return rc;
}
