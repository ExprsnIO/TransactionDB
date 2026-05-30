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

static int exec_select(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  qctx Q; memset(&Q, 0, sizeof(Q));

  /* 1. sources */
  tdb_expr *filter = q->where;
  for (tdb_src *s = q->from; s; s = s->next) {
    if (Q.nsrc >= MAXSRC) { tdb_db_seterr(db, "too many tables in FROM"); return TDB_ERROR; }
    if (s->subquery) { tdb_db_seterr(db, "subqueries in FROM not yet supported"); return TDB_UNSUPPORTED; }
    if (s->join == JOIN_LEFT || s->join == JOIN_RIGHT || s->join == JOIN_FULL) {
      tdb_db_seterr(db, "outer joins not yet supported"); return TDB_UNSUPPORTED;
    }
    tdb_table *t = tdb_catalog_find_table(db->cat, s->table);
    if (!t) { tdb_db_seterr(db, "no such table: %s", s->table); return TDB_ERROR; }
    Q.src[Q.nsrc].tbl = t;
    Q.src[Q.nsrc].alias = s->alias;
    Q.src[Q.nsrc].base = Q.totalcols;
    Q.totalcols += t->ncol;
    Q.nsrc++;
    if (s->on) filter = and_join(st->arena, filter, s->on);
  }

  /* 2. resolve clauses (collect aggregates) */
  if (filter && resolve_expr(db, &Q, filter)) return TDB_ERROR;
  if (q->group && resolve_list(db, &Q, q->group)) return TDB_ERROR;

  /* projection: expand '*', resolve explicit exprs, build names */
  tdb_expr **oexpr = NULL; char **onames = NULL; int nout = 0, ocap = 0;
  #define ADD_OUT(E, NM) do { \
    if (nout == ocap) { ocap = ocap ? ocap*2 : 8; \
      oexpr = (tdb_expr**)tdb_realloc(oexpr, sizeof(void*)*(size_t)ocap); \
      onames = (char**)tdb_realloc(onames, sizeof(void*)*(size_t)ocap); } \
    oexpr[nout] = (E); onames[nout] = tdb_strdup((NM) ? (NM) : "column"); nout++; \
  } while (0)

  for (int i = 0; i < q->cols->n; i++) {
    tdb_expr *item = q->cols->items[i];
    char *alias = q->cols->aliases[i];
    if (item->kind == EX_STAR) {
      for (int s = 0; s < Q.nsrc; s++) {
        tdb_table *t = Q.src[s].tbl;
        if (item->table) {
          const char *nm = Q.src[s].alias ? Q.src[s].alias : t->name;
          if (strcasecmp(nm, item->table) != 0) continue;
        }
        for (int k = 0; k < t->ncol; k++) {
          if (t->cols[k].hidden) continue;
          tdb_expr *col = tdb_expr_new(st->arena, EX_COLUMN);
          col->name = t->cols[k].name;
          col->col_index = Q.src[s].base + k;
          ADD_OUT(col, t->cols[k].name);
        }
      }
    } else {
      if (resolve_expr(db, &Q, item)) { tdb_mfree(oexpr); tdb_mfree(onames); return TDB_ERROR; }
      const char *nm = alias ? alias : (item->kind == EX_COLUMN ? item->name : NULL);
      ADD_OUT(item, nm);
    }
  }
  if (q->having && resolve_expr(db, &Q, q->having)) { tdb_mfree(oexpr); tdb_mfree(onames); return TDB_ERROR; }
  int norder = q->order ? q->order->n : 0;
  for (int i = 0; i < norder; i++)
    if (resolve_expr(db, &Q, q->order->exprs[i])) { tdb_mfree(oexpr); tdb_mfree(onames); return TDB_ERROR; }

  int aggregated = (Q.nagg > 0 || q->group != NULL);

  /* 3. materialize sources + cross product (filtered) */
  rowset sets[MAXSRC];
  for (int s = 0; s < Q.nsrc; s++) {
    int rc = mat_table(db, Q.src[s].tbl, &sets[s]);
    if (rc) { for (int j = 0; j < s; j++) rowset_free(&sets[j]); tdb_mfree(oexpr); tdb_mfree(onames); return rc; }
  }
  rowset in; rowset_init(&in, Q.totalcols ? Q.totalcols : 1);
  if (Q.nsrc == 0) {
    /* SELECT with no FROM: single empty row */
    rowset_add(&in, row_alloc(1));
  } else {
    tdb_value *combined = row_alloc(Q.totalcols);
    combine_ctx cc = { db, &Q, sets, combined, Q.totalcols, &in, filter, st };
    combine_rec(&cc, 0);
    free_row(combined, Q.totalcols);
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
}
