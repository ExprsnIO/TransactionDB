/* tdb_exec_dml.inc — DML/DDL execution (included by tdb_exec.c). */

/* parse + resolve a column expression (generated/DEFAULT) against `t` */
static tdb_expr *prepare_col_expr(tdb_db *db, tdb_stmt *st, tdb_table *t,
                                  const char *sql) {
  tdb_expr *e = NULL; char *err = NULL;
  if (tdb_parse_expression(st->arena, sql, &e, &err)) {
    tdb_db_seterr(db, "%s", err ? err : "bad expression");
    return NULL;
  }
  qctx q; memset(&q, 0, sizeof(q));
  q.src[0].tbl = t; q.src[0].base = 0; q.nsrc = 1; q.totalcols = t->ncol;
  if (resolve_expr(db, &q, e)) return NULL;
  return e;
}

/* a single-column INTEGER PRIMARY KEY behaves as the rowid alias */
static int pk_int_col(tdb_table *t) {
  int idx = -1, cnt = 0;
  for (int i = 0; i < t->ncol; i++) if (t->cols[i].pk) { cnt++; idx = i; }
  if (cnt != 1) return -1;
  tdb_typeid id = t->cols[idx].type.id;
  if (id == TDB_T_INTEGER || id == TDB_T_BIGINT || id == TDB_T_SMALLINT ||
      id == TDB_T_TINYINT) return idx;
  return -1;
}

/* finalize a full column row: generated cols, type coercion, NOT NULL, period */
static int finalize_row(tdb_db *db, tdb_stmt *st, tdb_table *t, tdb_value *full) {
  for (int i = 0; i < t->ncol; i++) {
    tdb_column *col = &t->cols[i];
    if (col->generated != TDB_GEN_NONE && col->generated_sql) {
      tdb_expr *ge = prepare_col_expr(db, st, t, col->generated_sql);
      if (!ge) return TDB_ERROR;
      ectx ec = { full, t->ncol, NULL, 0, st, NULL, NULL };
      tdb_value v; tdb_value_init(&v);
      eval(db, &ec, ge, &v);
      tdb_value_clear(&full[i]); full[i] = v;
    }
    const char *why = NULL;
    int rc = tdb_typespec_coerce(&full[i], &col->type, &why);
    if (rc == TDB_MISMATCH || rc == TDB_CONSTRAINT) {
      tdb_db_seterr(db, "column %s: %s", col->name, why ? why : "type error");
      return rc;
    }
    if (col->notnull && full[i].type == TDB_VAL_NULL) {
      tdb_db_seterr(db, "NOT NULL constraint failed: %s", col->name);
      return TDB_CONSTRAINT;
    }
  }
  return TDB_OK;
}

static int store_full(tdb_db *db, tdb_stmt *st, tdb_table *t, tdb_value *row,
                      tdb_rowid rowid, tdb_buf *rec, int is_update) {
  int rc = finalize_row(db, st, t, row);
  if (rc) return rc;
  tdb_buf_reset(rec);
  tdb_record_encode(row, t->ncol, rec);
  return is_update ? db->engine->vtab->update(db->engine, db->txn, t, rowid, rec->data, (int)rec->len)
                   : db->engine->vtab->insert(db->engine, db->txn, t, rowid, rec->data, (int)rec->len);
}

/* ---- RETURNING projection support -------------------------------------- */
typedef struct { tdb_expr **exprs; int n; } ret_proj;

/* Resolve the RETURNING list against table `t`, set up the statement's result
** columns (is_select/ncol/colnames), and capture the resolved output
** expressions in `rp`. The expr array is malloc'd; free with ret_proj_free.
** No-op (rp->exprs = NULL) when the statement carries no RETURNING clause. */
static int returning_setup(tdb_db *db, tdb_stmt *st, tdb_table *t,
                           tdb_exprlist *r, ret_proj *rp) {
  rp->exprs = NULL; rp->n = 0;
  if (!r) return TDB_OK;
  qctx q; memset(&q, 0, sizeof(q));
  q.src[0].tbl = t; q.src[0].base = 0; q.nsrc = 1; q.totalcols = t->ncol;

  tdb_expr **oexpr = NULL; char **onames = NULL; int n = 0, cap = 0;
  #define RET_ADD(E, NM) do { \
    if (n == cap) { cap = cap ? cap*2 : 8; \
      oexpr = (tdb_expr**)tdb_realloc(oexpr, sizeof(void*)*(size_t)cap); \
      onames = (char**)tdb_realloc(onames, sizeof(void*)*(size_t)cap); } \
    oexpr[n] = (E); onames[n] = tdb_strdup((NM) ? (NM) : "column"); n++; \
  } while (0)

  for (int i = 0; i < r->n; i++) {
    tdb_expr *item = r->items[i];
    char *alias = r->aliases ? r->aliases[i] : NULL;
    if (item->kind == EX_STAR) {
      for (int k = 0; k < t->ncol; k++) {
        if (t->cols[k].hidden) continue;
        tdb_expr *col = tdb_expr_new(st->arena, EX_COLUMN);
        col->name = t->cols[k].name; col->col_index = k;
        RET_ADD(col, t->cols[k].name);
      }
    } else {
      if (resolve_expr(db, &q, item)) {
        for (int k = 0; k < n; k++) tdb_mfree(onames[k]);
        tdb_mfree(oexpr); tdb_mfree(onames);
        return TDB_ERROR;
      }
      const char *nm = alias ? alias : (item->kind == EX_COLUMN ? item->name : NULL);
      RET_ADD(item, nm);
    }
  }
  #undef RET_ADD
  st->is_select = 1;
  st->ncol = n;
  st->colnames = onames;
  rp->exprs = oexpr; rp->n = n;
  return TDB_OK;
}

static void ret_proj_free(ret_proj *rp) { tdb_mfree(rp->exprs); rp->exprs = NULL; }

/* Evaluate the RETURNING projection against `rowvals` (ncol wide) and append a
** result row to the statement. No-op when there is no RETURNING. */
static void returning_emit(tdb_db *db, tdb_stmt *st, ret_proj *rp,
                           tdb_value *rowvals, int ncol) {
  if (!rp->exprs) return;
  ectx ec = { rowvals, ncol, NULL, 0, st, NULL, NULL };
  tdb_value *out = row_alloc(rp->n);
  for (int i = 0; i < rp->n; i++) eval(db, &ec, rp->exprs[i], &out[i]);
  add_result_row(st, out);
}

/* Insert one row: place src[0..nsrc) into the target columns, fill DEFAULTs,
** assign the rowid (INTEGER PRIMARY KEY aliasing), stamp the temporal period,
** run generated columns / type coercion / NOT NULL, then store. Handles PK
** conflicts per the INSERT's OR-action / ON CONFLICT clause. *applied reports
** whether a row was actually written (0 when an IGNORE/DO NOTHING skipped). */
static int insert_row_vals(tdb_db *db, tdb_stmt *st, tdb_table *t, const int *target,
                           int ntarget, tdb_value *src, int nsrc, int pkc,
                           tdb_ast_stmt *a, const int *up_setidx, tdb_buf *rec,
                           ret_proj *rp, int *applied) {
  *applied = 0;
  tdb_value *full = row_alloc(t->ncol);
  for (int i = 0; i < t->ncol; i++) {
    if (t->cols[i].default_sql) {
      tdb_expr *de = prepare_col_expr(db, st, t, t->cols[i].default_sql);
      if (de) { ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL }; eval(db, &ec, de, &full[i]); }
    }
  }
  int nv = nsrc < ntarget ? nsrc : ntarget;
  for (int i = 0; i < nv; i++) tdb_value_copy(&full[target[i]], &src[i]);

  tdb_rowid rowid;
  if (pkc >= 0 && full[pkc].type != TDB_VAL_NULL) {
    rowid = (tdb_rowid)tdb_value_as_int(&full[pkc]);
  } else {
    db->engine->vtab->next_rowid(db->engine, t, &rowid);
    if (pkc >= 0) tdb_value_set_int(&full[pkc], (int64_t)rowid);
  }
  if (t->versioning == TDB_VERSIONING_SYSTEM) {
    if (t->row_start_col >= 0) tdb_value_set_int(&full[t->row_start_col], (int64_t)db->txn->id);
    if (t->row_end_col >= 0)   tdb_value_set_int(&full[t->row_end_col], INT64_MAX);
  }

  /* PK conflict resolution (only user-supplied integer PKs can collide) */
  if (pkc >= 0) {
    const uint8_t *ex; int exn, found = 0;
    db->engine->vtab->seek_rowid(db->engine, db->txn, t, rowid, &ex, &exn, &found);
    if (found) {
      if (a->u.insert.or_action == TDB_OR_IGNORE ||
          (a->u.insert.has_upsert && a->u.insert.upsert_nothing)) {
        free_row(full, t->ncol); return TDB_OK;          /* skip */
      }
      if (a->u.insert.or_action == TDB_OR_REPLACE) {
        int rc = store_full(db, st, t, full, rowid, rec, 1);
        if (!rc) { *applied = 1; returning_emit(db, st, rp, full, t->ncol); }
        free_row(full, t->ncol); return rc;
      }
      if (a->u.insert.has_upsert) {                       /* DO UPDATE SET ... */
        tdb_value exd[64]; int enc = 0;
        tdb_record_decode(ex, (size_t)exn, exd, 64, &enc);
        tdb_value *urow = row_alloc(t->ncol);
        for (int i = 0; i < t->ncol && i < enc; i++) tdb_value_copy(&urow[i], &exd[i]);
        for (int i = 0; i < enc; i++) tdb_value_clear(&exd[i]);
        ectx ec = { urow, t->ncol, NULL, 0, st, NULL, NULL };
        int doit = 1;
        if (a->u.insert.up_where) { tdb_value w; tdb_value_init(&w); eval(db, &ec, a->u.insert.up_where, &w); doit = val_true(&w); tdb_value_clear(&w); }
        int rc = TDB_OK;
        if (doit) {
          for (int i = 0; i < a->u.insert.up_nset; i++) {
            tdb_value v; tdb_value_init(&v);
            eval(db, &ec, a->u.insert.up_vals[i], &v);
            tdb_value_clear(&urow[up_setidx[i]]); urow[up_setidx[i]] = v;
          }
          rc = store_full(db, st, t, urow, rowid, rec, 1);
          if (!rc) { *applied = 1; returning_emit(db, st, rp, urow, t->ncol); }
        }
        free_row(urow, t->ncol); free_row(full, t->ncol);
        return rc;
      }
      tdb_db_seterr(db, "UNIQUE constraint failed: %s primary key", t->name);
      free_row(full, t->ncol);
      return TDB_CONSTRAINT;
    }
  }

  int rc = store_full(db, st, t, full, rowid, rec, 0);
  if (!rc) { *applied = 1; returning_emit(db, st, rp, full, t->ncol); }
  free_row(full, t->ncol);
  return rc;
}

static int exec_insert(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.insert.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.insert.table); return TDB_ERROR; }

  /* target column indices */
  int target[64]; int ntarget;
  if (a->u.insert.ncol > 0) {
    ntarget = a->u.insert.ncol;
    for (int i = 0; i < ntarget; i++) {
      target[i] = tdb_table_find_column(t, a->u.insert.cols[i]);
      if (target[i] < 0) { tdb_db_seterr(db, "no such column: %s", a->u.insert.cols[i]); return TDB_ERROR; }
    }
  } else {
    ntarget = 0;
    for (int i = 0; i < t->ncol; i++)
      if (!t->cols[i].hidden && t->cols[i].generated == TDB_GEN_NONE) target[ntarget++] = i;
  }

  int pkc = pk_int_col(t);
  tdb_buf rec; tdb_buf_init(&rec);
  int rc = TDB_OK;

  ret_proj rp;
  if (returning_setup(db, st, t, a->returning, &rp)) { tdb_buf_free(&rec); return TDB_ERROR; }

  /* Resolve ON CONFLICT DO UPDATE SET targets/expressions against the table.
  ** up_setidx maps each up_cols[i] to its column slot; up_vals and up_where
  ** are resolved as single-source (the conflict row) expressions. */
  int up_setidx[64];
  if (a->u.insert.has_upsert && a->u.insert.up_nset > 0) {
    if (a->u.insert.up_nset > 64) { tdb_db_seterr(db, "too many SET clauses"); tdb_buf_free(&rec); ret_proj_free(&rp); return TDB_ERROR; }
    qctx uq; memset(&uq, 0, sizeof(uq));
    uq.src[0].tbl = t; uq.src[0].base = 0; uq.nsrc = 1; uq.totalcols = t->ncol;
    for (int i = 0; i < a->u.insert.up_nset; i++) {
      up_setidx[i] = tdb_table_find_column(t, a->u.insert.up_cols[i]);
      if (up_setidx[i] < 0) { tdb_db_seterr(db, "no such column: %s", a->u.insert.up_cols[i]); tdb_buf_free(&rec); ret_proj_free(&rp); return TDB_ERROR; }
      if (resolve_expr(db, &uq, a->u.insert.up_vals[i])) { tdb_buf_free(&rec); ret_proj_free(&rp); return TDB_ERROR; }
    }
    if (a->u.insert.up_where && resolve_expr(db, &uq, a->u.insert.up_where)) { tdb_buf_free(&rec); ret_proj_free(&rp); return TDB_ERROR; }
  }

  if (a->u.insert.select) {
    /* INSERT ... SELECT */
    tdb_stmt tmp;
    rc = run_subselect(db, st, a->u.insert.select, &tmp, NULL);
    if (!rc) {
      for (int r = 0; r < tmp.nrows && !rc; r++) {
        int applied = 0;
        rc = insert_row_vals(db, st, t, target, ntarget, tmp.rows[r], tmp.ncol, pkc, a, up_setidx, &rec, &rp, &applied);
        if (!rc && applied) st->changes++;
      }
    }
    tdb_stmt_clear_results(&tmp);
    tdb_buf_free(&rec);
    ret_proj_free(&rp);
    return rc;
  }

  /* INSERT ... VALUES: resolve value expressions (assigns parameter slots) */
  qctx pq; memset(&pq, 0, sizeof(pq));
  for (int r = 0; r < a->u.insert.nrows; r++) {
    tdb_exprlist *row = a->u.insert.rows[r];
    for (int i = 0; i < row->n; i++)
      if (resolve_expr(db, &pq, row->items[i])) { tdb_buf_free(&rec); ret_proj_free(&rp); return TDB_ERROR; }
  }

  for (int r = 0; r < a->u.insert.nrows && !rc; r++) {
    tdb_exprlist *row = a->u.insert.rows[r];
    tdb_value *src = row_alloc(row->n ? row->n : 1);
    ectx ec0 = { NULL, 0, NULL, 0, st, NULL, NULL };
    for (int i = 0; i < row->n; i++) eval(db, &ec0, row->items[i], &src[i]);
    int applied = 0;
    rc = insert_row_vals(db, st, t, target, ntarget, src, row->n, pkc, a, up_setidx, &rec, &rp, &applied);
    free_row(src, row->n ? row->n : 1);
    if (!rc && applied) st->changes++;
  }
  tdb_buf_free(&rec);
  ret_proj_free(&rp);
  return rc;
}

/* materialize (rowid, values) pairs for UPDATE/DELETE */
typedef struct { tdb_rowid *rids; tdb_value **rows; int n, cap, ncol; } rrowset;
static int mat_rid(tdb_db *db, tdb_table *t, rrowset *rr) {
  memset(rr, 0, sizeof(*rr)); rr->ncol = t->ncol;
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, NULL, NULL, 0, NULL, &sc);
  if (rc) return rc;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = db->engine->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW) {
    tdb_value tmp[64]; int nc = 0;
    if (tdb_record_decode(rec, (size_t)reclen, tmp, 64, &nc)) { rc = TDB_CORRUPT; break; }
    tdb_value *row = row_alloc(t->ncol);
    for (int i = 0; i < t->ncol && i < nc; i++) tdb_value_copy(&row[i], &tmp[i]);
    for (int i = 0; i < nc; i++) tdb_value_clear(&tmp[i]);
    if (rr->n == rr->cap) {
      rr->cap = rr->cap ? rr->cap * 2 : 16;
      rr->rids = (tdb_rowid *)tdb_realloc(rr->rids, sizeof(tdb_rowid) * (size_t)rr->cap);
      rr->rows = (tdb_value **)tdb_realloc(rr->rows, sizeof(void *) * (size_t)rr->cap);
    }
    rr->rids[rr->n] = rid; rr->rows[rr->n] = row; rr->n++;
  }
  db->engine->vtab->scan_close(sc);
  return (rc == TDB_DONE) ? TDB_OK : rc;
}
static void rrowset_free(rrowset *rr) {
  for (int i = 0; i < rr->n; i++) free_row(rr->rows[i], rr->ncol);
  tdb_mfree(rr->rows); tdb_mfree(rr->rids);
}

static int exec_update(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.update.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.update.table); return TDB_ERROR; }

  qctx q; memset(&q, 0, sizeof(q));
  q.src[0].tbl = t; q.src[0].base = 0; q.nsrc = 1; q.totalcols = t->ncol;
  if (a->u.update.where && resolve_expr(db, &q, a->u.update.where)) return TDB_ERROR;
  int setidx[64];
  for (int i = 0; i < a->u.update.nset; i++) {
    setidx[i] = tdb_table_find_column(t, a->u.update.set_cols[i]);
    if (setidx[i] < 0) { tdb_db_seterr(db, "no such column: %s", a->u.update.set_cols[i]); return TDB_ERROR; }
    if (resolve_expr(db, &q, a->u.update.set_vals[i])) return TDB_ERROR;
  }

  ret_proj rp;
  if (returning_setup(db, st, t, a->returning, &rp)) return TDB_ERROR;

  rrowset rr; int rc = mat_rid(db, t, &rr);
  if (rc) { rrowset_free(&rr); ret_proj_free(&rp); return rc; }
  tdb_buf rec; tdb_buf_init(&rec);
  for (int r = 0; r < rr.n; r++) {
    ectx ec = { rr.rows[r], t->ncol, NULL, 0, st, NULL, NULL };
    if (a->u.update.where) {
      tdb_value w; tdb_value_init(&w); eval(db, &ec, a->u.update.where, &w);
      int ok = val_true(&w); tdb_value_clear(&w);
      if (!ok) continue;
    }
    tdb_value *full = row_alloc(t->ncol);
    for (int i = 0; i < t->ncol; i++) tdb_value_copy(&full[i], &rr.rows[r][i]);
    for (int i = 0; i < a->u.update.nset; i++) {
      tdb_value v; tdb_value_init(&v);
      eval(db, &ec, a->u.update.set_vals[i], &v);
      tdb_value_clear(&full[setidx[i]]); full[setidx[i]] = v;
    }
    rc = finalize_row(db, st, t, full);
    if (rc) { free_row(full, t->ncol); break; }
    tdb_buf_reset(&rec);
    tdb_record_encode(full, t->ncol, &rec);
    rc = db->engine->vtab->update(db->engine, db->txn, t, rr.rids[r], rec.data, (int)rec.len);
    if (!rc) { st->changes++; returning_emit(db, st, &rp, full, t->ncol); }
    free_row(full, t->ncol);
    if (rc) break;
  }
  tdb_buf_free(&rec);
  rrowset_free(&rr);
  ret_proj_free(&rp);
  return rc;
}

static int exec_delete(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.del.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.del.table); return TDB_ERROR; }
  qctx q; memset(&q, 0, sizeof(q));
  q.src[0].tbl = t; q.src[0].base = 0; q.nsrc = 1; q.totalcols = t->ncol;
  if (a->u.del.where && resolve_expr(db, &q, a->u.del.where)) return TDB_ERROR;

  ret_proj rp;
  if (returning_setup(db, st, t, a->returning, &rp)) return TDB_ERROR;

  rrowset rr; int rc = mat_rid(db, t, &rr);
  if (rc) { rrowset_free(&rr); ret_proj_free(&rp); return rc; }
  for (int r = 0; r < rr.n; r++) {
    ectx ec = { rr.rows[r], t->ncol, NULL, 0, st, NULL, NULL };
    if (a->u.del.where) {
      tdb_value w; tdb_value_init(&w); eval(db, &ec, a->u.del.where, &w);
      int ok = val_true(&w); tdb_value_clear(&w);
      if (!ok) continue;
    }
    rc = db->engine->vtab->remove(db->engine, db->txn, t, rr.rids[r]);
    if (rc) break;
    st->changes++;
    returning_emit(db, st, &rp, rr.rows[r], t->ncol);  /* RETURNING old row */
  }
  rrowset_free(&rr);
  ret_proj_free(&rp);
  return rc;
}

static int exec_create_table(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  tdb_create_table *ct = a->u.create_table;
  if (tdb_catalog_find_table(db->cat, ct->name)) {
    if (ct->if_not_exists) return TDB_OK;
    tdb_db_seterr(db, "table %s already exists", ct->name);
    return TDB_ERROR;
  }
  char *err = NULL;
  tdb_table *t = tdb_ast_to_table(ct, &err);
  if (!t) { tdb_db_seterr(db, "%s", err ? err : "invalid table"); tdb_mfree(err); return TDB_ERROR; }
  int rc = db->engine->vtab->create_table(db->engine, db->txn, t);
  if (rc) { tdb_table_free(t); return rc; }
  rc = tdb_catalog_add_table(db->cat, t);
  if (rc) { tdb_table_free(t); return rc; }
  tdb_pager_set_schema_cookie(db->pager, tdb_pager_schema_cookie(db->pager) + 1);
  return TDB_OK;
}

static int exec_create_index(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  tdb_create_index *ci = a->u.create_index;
  tdb_table *t = tdb_catalog_find_table(db->cat, ci->table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", ci->table); return TDB_ERROR; }
  if (ci->kind == TDB_IDX_RTREE) {
    if (ci->ncol != 1) {
      tdb_db_seterr(db, "spatial index requires exactly one geometry column");
      return TDB_ERROR;
    }
    if (t->columnar) {
      tdb_db_seterr(db, "spatial index requires a row-organized table");
      return TDB_ERROR;
    }
  }
  tdb_index ix; memset(&ix, 0, sizeof(ix));
  ix.name = tdb_strdup(ci->name);
  ix.unique = ci->unique;
  ix.kind = ci->kind;
  ix.ncol = ci->ncol;
  ix.col_idx = (int *)tdb_malloc(sizeof(int) * (size_t)ci->ncol);
  ix.desc = (uint8_t *)tdb_malloc((size_t)ci->ncol);
  for (int i = 0; i < ci->ncol; i++) {
    ix.col_idx[i] = tdb_table_find_column(t, ci->cols[i]);
    ix.desc[i] = ci->desc[i];
    if (ix.col_idx[i] < 0) {
      tdb_db_seterr(db, "no such column: %s", ci->cols[i]);
      tdb_mfree(ix.name); tdb_mfree(ix.col_idx); tdb_mfree(ix.desc);
      return TDB_ERROR;
    }
  }
  if (ci->kind == TDB_IDX_RTREE) {
    tdb_typeid id = t->cols[ix.col_idx[0]].type.id;
    if (id != TDB_T_GEOMETRY && id != TDB_T_POINT && id != TDB_T_GEOGRAPHY) {
      tdb_db_seterr(db, "spatial index column must be a geometry type");
      tdb_mfree(ix.name); tdb_mfree(ix.col_idx); tdb_mfree(ix.desc);
      return TDB_ERROR;
    }
  }
  /* append to the in-memory table so it is maintained this session */
  tdb_index *grown = (tdb_index *)tdb_realloc(t->indexes, sizeof(tdb_index) * (size_t)(t->nindex + 1));
  t->indexes = grown;
  t->indexes[t->nindex] = ix;
  t->nindex++;
  int rc = db->engine->vtab->create_index(db->engine, db->txn, t, &t->indexes[t->nindex - 1]);
  /* persist the new index by rewriting the table's catalog row */
  if (!rc) rc = tdb_catalog_update_table(db->cat, t);
  return rc;
}

static int exec_drop(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  if (a->kind == ST_DROP_TABLE) {
    tdb_table *t = tdb_catalog_find_table(db->cat, a->u.drop.name);
    if (!t) {
      if (a->u.drop.if_exists) return TDB_OK;
      tdb_db_seterr(db, "no such table: %s", a->u.drop.name);
      return TDB_ERROR;
    }
    int rc = db->engine->vtab->drop_table(db->engine, db->txn, t);  /* reclaim pages */
    if (rc) return rc;
    tdb_catalog_drop_table(db->cat, a->u.drop.name);
    return TDB_OK;
  }
  if (a->kind == ST_DROP_INDEX) {
    /* index names are unique across the database; find the owning table */
    tdb_table *owner = NULL; int pos = -1;
    int nt = tdb_catalog_table_count(db->cat);
    for (int i = 0; i < nt && !owner; i++) {
      tdb_table *t = tdb_catalog_table_at(db->cat, i);
      for (int k = 0; k < t->nindex; k++)
        if (t->indexes[k].name && strcasecmp(t->indexes[k].name, a->u.drop.name) == 0) {
          owner = t; pos = k; break;
        }
    }
    if (!owner) {
      if (a->u.drop.if_exists) return TDB_OK;
      tdb_db_seterr(db, "no such index: %s", a->u.drop.name);
      return TDB_ERROR;
    }
    int rc = db->engine->vtab->drop_index(db->engine, db->txn, owner, &owner->indexes[pos]);
    if (rc) return rc;
    tdb_table_drop_index(owner, pos);
    return tdb_catalog_update_table(db->cat, owner);  /* persist the smaller index set */
  }
  if (a->kind == ST_DROP_VIEW) {
    if (!tdb_catalog_find_view(db->cat, a->u.drop.name)) {
      if (a->u.drop.if_exists) return TDB_OK;
      tdb_db_seterr(db, "no such view: %s", a->u.drop.name);
      return TDB_ERROR;
    }
    tdb_catalog_drop_view(db->cat, a->u.drop.name);  /* drops the persisted catalog row */
    return TDB_OK;
  }
  return TDB_OK;
}

static int exec_create_view(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  if (tdb_catalog_find_view(db->cat, a->u.create_view.name)) {
    if (a->u.create_view.if_not_exists) return TDB_OK;
    tdb_db_seterr(db, "view %s already exists", a->u.create_view.name);
    return TDB_ERROR;
  }
  tdb_view *v = (tdb_view *)tdb_calloc(sizeof(*v));
  v->name = tdb_strdup(a->u.create_view.name);
  v->select_sql = tdb_strdup(a->u.create_view.select_sql);
  v->materialized = a->u.create_view.materialized;
  return tdb_catalog_add_view(db->cat, v);
}

static int exec_create_routine(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  const char *lang = a->u.create_routine.lang;
  if (lang && (!strcasecmp(lang, "plsql") || !strcasecmp(lang, "plpgsql"))) {
    tdb_plsql_proc *proc = NULL;
    char err[160] = {0};
    int rc = tdb_plsql_parse(a->u.create_routine.lua_src ? a->u.create_routine.lua_src : "",
                             (const char *const *)a->u.create_routine.params,
                             a->u.create_routine.nparams, &proc, err, sizeof(err));
    if (rc != TDB_OK) { tdb_db_seterr(db, "PL/SQL compile error: %s", err); return TDB_ERROR; }
    return tdb_db_add_plsql(db, a->u.create_routine.name, proc,
                            a->u.create_routine.is_function, a->u.create_routine.nparams);
  }
#ifdef TDB_HAVE_LUA
  /* wrap the body as a function value: function(<params>) <body> end */
  tdb_buf w; tdb_buf_init(&w);
  tdb_buf_append(&w, "function(", 9);
  for (int i = 0; i < a->u.create_routine.nparams; i++) {
    if (i) tdb_buf_putc(&w, ',');
    tdb_buf_append(&w, a->u.create_routine.params[i], strlen(a->u.create_routine.params[i]));
  }
  tdb_buf_append(&w, ") ", 2);
  const char *body = a->u.create_routine.lua_src ? a->u.create_routine.lua_src : "";
  tdb_buf_append(&w, body, strlen(body));
  tdb_buf_append(&w, " end", 4);
  tdb_buf_putc(&w, '\0');

  char *err = NULL;
  int rc = tdb_lua_define(db->lua, a->u.create_routine.name, (char *)w.data, &err);
  if (rc) { tdb_db_seterr(db, "%s", err ? err : "lua error"); tdb_mfree(err); tdb_buf_free(&w); return TDB_ERROR; }

  tdb_routine *rt = (tdb_routine *)tdb_calloc(sizeof(*rt));
  rt->name = tdb_strdup(a->u.create_routine.name);
  rt->lua_src = tdb_strdup((char *)w.data);
  rt->is_function = a->u.create_routine.is_function;
  rt->nargs = a->u.create_routine.nparams;
  tdb_buf_free(&w);
  return tdb_catalog_add_routine(db->cat, rt);
#else
  TDB_UNUSED(a);
  tdb_db_seterr(db, "Lua is not enabled in this build");
  return TDB_UNSUPPORTED;
#endif
}

static int exec_call(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
#ifdef TDB_HAVE_LUA
  if (!tdb_catalog_find_routine(db->cat, a->u.call.name)) {
    tdb_db_seterr(db, "no such routine: %s", a->u.call.name);
    return TDB_ERROR;
  }
  qctx q; memset(&q, 0, sizeof(q));
  int argc = a->u.call.args ? a->u.call.args->n : 0;
  for (int i = 0; i < argc; i++)
    if (resolve_expr(db, &q, a->u.call.args->items[i])) return TDB_ERROR;
  tdb_value *args = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)(argc ? argc : 1));
  ectx ec = { NULL, 0, NULL, 0, st, NULL, NULL };
  for (int i = 0; i < argc; i++) { tdb_value_init(&args[i]); eval(db, &ec, a->u.call.args->items[i], &args[i]); }
  char *err = NULL;
  int rc = tdb_lua_call_proc(db->lua, a->u.call.name, args, argc, &err);
  for (int i = 0; i < argc; i++) tdb_value_clear(&args[i]);
  tdb_mfree(args);
  if (rc) { tdb_db_seterr(db, "%s", err ? err : "lua error"); tdb_mfree(err); return TDB_ERROR; }
  return TDB_OK;
#else
  TDB_UNUSED(st); TDB_UNUSED(a);
  tdb_db_seterr(db, "Lua is not enabled in this build");
  return TDB_UNSUPPORTED;
#endif
}

static int exec_alter(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.alter.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.alter.table); return TDB_ERROR; }

  switch (a->u.alter.action) {
    case TDB_ALTER_ADD_COLUMN: {
      tdb_coldef *cd = a->u.alter.add;
      if (tdb_table_find_column(t, cd->name) >= 0) {
        tdb_db_seterr(db, "duplicate column name: %s", cd->name); return TDB_ERROR;
      }
      tdb_column c;
      tdb_column_init(&c, cd->name, cd->type);
      c.coll = cd->coll; c.notnull = cd->notnull; c.unique = cd->unique;
      c.generated = cd->generated;
      if (cd->generated_sql) c.generated_sql = tdb_strdup(cd->generated_sql);
      if (cd->default_sql) c.default_sql = tdb_strdup(cd->default_sql);
      if (cd->check_sql) c.check_sql = tdb_strdup(cd->check_sql);
      if (tdb_table_add_column(t, &c) != TDB_OK) return TDB_NOMEM;
      int rc = db->engine->vtab->add_column(db->engine, db->txn, t);   /* new column store (columnar) */
      if (rc) return rc;
      t->schema_version++;
      return tdb_catalog_update_table(db->cat, t);
    }
    case TDB_ALTER_RENAME_COLUMN: {
      int ci = tdb_table_find_column(t, a->u.alter.col);
      if (ci < 0) { tdb_db_seterr(db, "no such column: %s", a->u.alter.col); return TDB_ERROR; }
      tdb_mfree(t->cols[ci].name);
      t->cols[ci].name = tdb_strdup(a->u.alter.newname);
      t->schema_version++;
      return tdb_catalog_update_table(db->cat, t);
    }
    case TDB_ALTER_RENAME_TABLE: {
      if (tdb_catalog_find_table(db->cat, a->u.alter.newname)) {
        tdb_db_seterr(db, "table %s already exists", a->u.alter.newname); return TDB_ERROR;
      }
      char *oldname = tdb_strdup(t->name);
      tdb_mfree(t->name);
      t->name = tdb_strdup(a->u.alter.newname);
      int rc = tdb_catalog_update_table_as(db->cat, oldname, t);
      tdb_mfree(oldname);
      return rc;
    }
    case TDB_ALTER_DROP_COLUMN: {
      int ci = tdb_table_find_column(t, a->u.alter.col);
      if (ci < 0) { tdb_db_seterr(db, "no such column: %s", a->u.alter.col); return TDB_ERROR; }
      if (t->ncol <= 1) { tdb_db_seterr(db, "cannot drop the only column of %s", t->name); return TDB_ERROR; }
      if (t->cols[ci].pk) { tdb_db_seterr(db, "cannot drop PRIMARY KEY column: %s", a->u.alter.col); return TDB_ERROR; }
      if (t->cols[ci].hidden || ci == t->row_start_col || ci == t->row_end_col) {
        tdb_db_seterr(db, "cannot drop system column: %s", a->u.alter.col); return TDB_ERROR;
      }
      for (int i = 0; i < t->nindex; i++)
        for (int k = 0; k < t->indexes[i].ncol; k++)
          if (t->indexes[i].col_idx[k] == ci) {
            tdb_db_seterr(db, "cannot drop column %s: used by index %s",
                          a->u.alter.col, t->indexes[i].name);
            return TDB_ERROR;
          }
      int rc = db->engine->vtab->drop_column(db->engine, db->txn, t, ci);  /* rewrite/discard storage */
      if (rc) return rc;
      tdb_table_drop_column(t, ci);
      t->schema_version++;
      return tdb_catalog_update_table(db->cat, t);
    }
    default:
      tdb_db_seterr(db, "unsupported ALTER TABLE action");
      return TDB_UNSUPPORTED;
  }
}

/* ------------------------ Phase 11: utility DDL ------------------------ */

/* TRUNCATE TABLE — delete every visible row. (Identity reset / CASCADE
** are parsed and remembered on the AST but not yet executed.) */
static int exec_truncate(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.truncate.name);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.truncate.name); return TDB_ERROR; }
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, NULL, NULL, 0, NULL, &sc);
  if (rc) return rc;
  tdb_rowid *ids = NULL; int nid = 0, capid = 0;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = db->engine->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW) {
    if (nid == capid) {
      capid = capid ? capid * 2 : 64;
      ids = (tdb_rowid *)tdb_realloc(ids, sizeof(tdb_rowid) * (size_t)capid);
    }
    ids[nid++] = rid;
  }
  db->engine->vtab->scan_close(sc);
  if (rc != TDB_DONE) { tdb_mfree(ids); return rc; }
  for (int i = 0; i < nid; i++) {
    int dr = db->engine->vtab->remove(db->engine, db->txn, t, ids[i]);
    if (dr) { tdb_mfree(ids); return dr; }
    st->changes++;
  }
  tdb_mfree(ids);
  return TDB_OK;
}

/* ANALYZE / REINDEX / COMMENT / LOCK / GRANT / REVOKE / ATTACH / DETACH —
** parsing is the main deliverable; semantic effects are stubs at this
** point (no stats catalog, ACL system, or attach manager exists yet). */
static int exec_analyze(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  if (a->u.analyze.name && !tdb_catalog_find_table(db->cat, a->u.analyze.name)) {
    tdb_db_seterr(db, "no such table: %s", a->u.analyze.name);
    return TDB_ERROR;
  }
  return TDB_OK;
}

static int exec_reindex(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  if (a->u.reindex.name && !a->u.reindex.is_index) {
    if (!tdb_catalog_find_table(db->cat, a->u.reindex.name)) {
      tdb_db_seterr(db, "no such table: %s", a->u.reindex.name);
      return TDB_ERROR;
    }
  }
  return TDB_OK;
}

static int exec_comment(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}

static int exec_tablespace_create(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}
static int exec_tablespace_drop(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}

static int exec_grant(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}

static int exec_attach(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(a);
  tdb_db_seterr(db, "ATTACH DATABASE is parsed but not yet executable");
  return TDB_UNSUPPORTED;
}
static int exec_detach(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(a);
  tdb_db_seterr(db, "DETACH DATABASE is parsed but not yet executable");
  return TDB_UNSUPPORTED;
}

static int exec_lock(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  if (a->u.lock_tbl.table && !tdb_catalog_find_table(db->cat, a->u.lock_tbl.table)) {
    tdb_db_seterr(db, "no such table: %s", a->u.lock_tbl.table);
    return TDB_ERROR;
  }
  /* pager is single-writer: the surrounding auto-txn already holds the write
  ** slot, so LOCK TABLE is a no-op at this level. */
  return TDB_OK;
}

static int exec_txn(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  int rc = TDB_OK;
  switch (a->kind) {
    case ST_BEGIN:
      if (db->txn) { tdb_db_seterr(db, "cannot start a transaction within a transaction"); return TDB_ERROR; }
      rc = tdb_txn_begin(db->tm, TDB_ISO_SNAPSHOT, 1, &db->txn);
      db->autocommit = 0;
      break;
    case ST_COMMIT:
      if (db->txn) {
        tdb_txnid wid = db->txn->id;
        rc = tdb_txn_commit(db->txn); db->txn = NULL;
        if (db->env) tdb_env_release_writer(db->env, wid);
      }
      db->autocommit = 1;
      break;
    case ST_ROLLBACK:
      if (db->txn) {
        tdb_txnid wid = db->txn->id;
        rc = tdb_txn_rollback(db->txn); db->txn = NULL;
        if (db->env) tdb_env_release_writer(db->env, wid);
      }
      db->autocommit = 1;
      break;
    case ST_SAVEPOINT:
      if (!db->txn) {  /* implicitly start a transaction, like SQLite */
        rc = tdb_txn_begin(db->tm, TDB_ISO_SNAPSHOT, 1, &db->txn);
        db->autocommit = 0;
        if (rc) break;
      }
      rc = tdb_txn_savepoint(db->txn, a->u.savepoint.name);
      break;
    case ST_RELEASE:
      if (!db->txn) { tdb_db_seterr(db, "no such savepoint: %s", a->u.savepoint.name); rc = TDB_ERROR; break; }
      rc = tdb_txn_release(db->txn, a->u.savepoint.name);
      if (rc == TDB_NOTFOUND) tdb_db_seterr(db, "no such savepoint: %s", a->u.savepoint.name);
      break;
    case ST_ROLLBACK_TO:
      if (!db->txn) { tdb_db_seterr(db, "no such savepoint: %s", a->u.savepoint.name); rc = TDB_ERROR; break; }
      rc = tdb_txn_rollback_to(db->txn, a->u.savepoint.name);
      if (rc == TDB_NOTFOUND) tdb_db_seterr(db, "no such savepoint: %s", a->u.savepoint.name);
      break;
    default: break;
  }
  return rc;
}
