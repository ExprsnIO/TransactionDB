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
} accum;

typedef struct {
  tdb_value *key;     /* group-by key values (ngroup) */
  tdb_value *rep;     /* representative source row (combined) */
  accum     *acc;     /* nagg accumulators */
} group;

static int const_int(tdb_db *db, tdb_stmt *st, tdb_expr *e, int64_t dflt) {
  if (!e) return (int)dflt;
  ectx ec = { NULL, 0, NULL, 0, st };
  tdb_value v; tdb_value_init(&v);
  eval(db, &ec, e, &v);
  int64_t r = (v.type == TDB_VAL_NULL) ? dflt : tdb_value_as_int(&v);
  tdb_value_clear(&v);
  return (int)r;
}

static void accum_update(accum *ac, tdb_expr *agg, tdb_db *db, ectx *ec) {
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
  } else tdb_value_set_null(out);
}

/* Resolve a FROM item (base table / derived table / view) and materialize its
** rows into `set`, filling Q->src[idx] (caller advances totalcols/nsrc). */
static int materialize_source(tdb_db *db, tdb_stmt *st, qctx *Q, int idx,
                              tdb_src *s, rowset *set) {
  qsrc *qs = &Q->src[idx];
  memset(qs, 0, sizeof(*qs));
  qs->alias = s->alias; qs->base = Q->totalcols; qs->join = s->join; qs->on = s->on;

  tdb_select *sub = s->subquery;
  if (!sub && s->table) {
    tdb_table *t = tdb_catalog_find_table(db->cat, s->table);
    if (t) { qs->tbl = t; qs->ncol = t->ncol; return mat_table(db, t, set); }
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
  int rc = run_subselect(db, st, sub, &tmp);
  if (rc) { tdb_stmt_clear_results(&tmp); return rc; }
  qs->ncol = tmp.ncol;
  qs->colnames = (char **)tdb_arena_alloc(st->arena, sizeof(char *) * (size_t)(tmp.ncol ? tmp.ncol : 1));
  for (int k = 0; k < tmp.ncol; k++) {
    const char *nm = (tmp.colnames && tmp.colnames[k]) ? tmp.colnames[k] : "col";
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

static int exec_select(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  qctx Q; memset(&Q, 0, sizeof(Q));
  rowset sets[MAXSRC];

  /* 1. sources: base table / derived table / view — materialize each */
  for (tdb_src *s = q->from; s; s = s->next) {
    if (Q.nsrc >= MAXSRC) {
      tdb_db_seterr(db, "too many tables in FROM");
      for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]);
      return TDB_ERROR;
    }
    int idx = Q.nsrc;
    int rc = materialize_source(db, st, &Q, idx, s, &sets[idx]);
    if (rc) { for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]); return rc; }
    Q.totalcols += Q.src[idx].ncol;
    Q.nsrc++;
  }

  /* 2. resolve ON conditions + clauses (collect aggregates) */
  #define FAIL_SETS do { for (int j = 0; j < Q.nsrc; j++) rowset_free(&sets[j]); } while (0)
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
    join_build(db, st, &Q, sets, q->where, &in);
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
      ectx ec = { in.rows[r], in.ncol, NULL, 0, st };
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
      ectx ec = { in.rows[r], in.ncol, NULL, 0, st };
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
      ectx ec = { groups[g].rep, in.ncol, aggvals, Q.nagg, st };
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
  st->cur = -1;
  return rc;
  #undef ADD_OUT
  #undef ADD_RESROW
  #undef FAILP
  #undef FAIL_SETS
}
