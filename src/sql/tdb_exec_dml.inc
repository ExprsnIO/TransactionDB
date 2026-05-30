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
      ectx ec = { full, t->ncol, NULL, 0, st };
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

static int exec_insert(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.insert.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.insert.table); return TDB_ERROR; }
  if (a->u.insert.select) { tdb_db_seterr(db, "INSERT...SELECT not yet supported"); return TDB_UNSUPPORTED; }

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

  /* resolve value expressions (assigns parameter slots in order) */
  qctx pq; memset(&pq, 0, sizeof(pq));
  for (int r = 0; r < a->u.insert.nrows; r++) {
    tdb_exprlist *row = a->u.insert.rows[r];
    for (int i = 0; i < row->n; i++)
      if (resolve_expr(db, &pq, row->items[i])) return TDB_ERROR;
  }

  int pkc = pk_int_col(t);
  tdb_buf rec; tdb_buf_init(&rec);
  int rc = TDB_OK;

  for (int r = 0; r < a->u.insert.nrows; r++) {
    tdb_exprlist *row = a->u.insert.rows[r];
    tdb_value *full = row_alloc(t->ncol);

    /* DEFAULTs first */
    for (int i = 0; i < t->ncol; i++) {
      if (t->cols[i].default_sql) {
        tdb_expr *de = prepare_col_expr(db, st, t, t->cols[i].default_sql);
        if (de) { ectx ec = { NULL, 0, NULL, 0, st }; eval(db, &ec, de, &full[i]); }
      }
    }
    /* provided values */
    int nv = row->n < ntarget ? row->n : ntarget;
    ectx ec0 = { NULL, 0, NULL, 0, st };
    for (int i = 0; i < nv; i++) eval(db, &ec0, row->items[i], &full[target[i]]);

    /* rowid + integer-pk aliasing */
    tdb_rowid rowid;
    if (pkc >= 0 && full[pkc].type != TDB_VAL_NULL) {
      rowid = (tdb_rowid)tdb_value_as_int(&full[pkc]);
    } else {
      db->engine->vtab->next_rowid(db->engine, t, &rowid);
      if (pkc >= 0) tdb_value_set_int(&full[pkc], (int64_t)rowid);
    }

    /* system-versioned: stamp the period start with the txn id */
    if (t->versioning == TDB_VERSIONING_SYSTEM && t->row_start_col >= 0)
      tdb_value_set_int(&full[t->row_start_col], (int64_t)st->db->txn->id);

    rc = finalize_row(db, st, t, full);
    if (rc) { free_row(full, t->ncol); break; }

    tdb_buf_reset(&rec);
    tdb_record_encode(full, t->ncol, &rec);
    rc = db->engine->vtab->insert(db->engine, db->txn, t, rowid, rec.data, (int)rec.len);
    free_row(full, t->ncol);
    if (rc) break;
    st->changes++;
  }
  tdb_buf_free(&rec);
  return rc;
}

/* materialize (rowid, values) pairs for UPDATE/DELETE */
typedef struct { tdb_rowid *rids; tdb_value **rows; int n, cap, ncol; } rrowset;
static int mat_rid(tdb_db *db, tdb_table *t, rrowset *rr) {
  memset(rr, 0, sizeof(*rr)); rr->ncol = t->ncol;
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, NULL, &sc);
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

  rrowset rr; int rc = mat_rid(db, t, &rr);
  if (rc) { rrowset_free(&rr); return rc; }
  tdb_buf rec; tdb_buf_init(&rec);
  for (int r = 0; r < rr.n; r++) {
    ectx ec = { rr.rows[r], t->ncol, NULL, 0, st };
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
    free_row(full, t->ncol);
    if (rc) break;
    st->changes++;
  }
  tdb_buf_free(&rec);
  rrowset_free(&rr);
  return rc;
}

static int exec_delete(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.del.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.del.table); return TDB_ERROR; }
  qctx q; memset(&q, 0, sizeof(q));
  q.src[0].tbl = t; q.src[0].base = 0; q.nsrc = 1; q.totalcols = t->ncol;
  if (a->u.del.where && resolve_expr(db, &q, a->u.del.where)) return TDB_ERROR;

  rrowset rr; int rc = mat_rid(db, t, &rr);
  if (rc) { rrowset_free(&rr); return rc; }
  for (int r = 0; r < rr.n; r++) {
    ectx ec = { rr.rows[r], t->ncol, NULL, 0, st };
    if (a->u.del.where) {
      tdb_value w; tdb_value_init(&w); eval(db, &ec, a->u.del.where, &w);
      int ok = val_true(&w); tdb_value_clear(&w);
      if (!ok) continue;
    }
    rc = db->engine->vtab->remove(db->engine, db->txn, t, rr.rids[r]);
    if (rc) break;
    st->changes++;
  }
  rrowset_free(&rr);
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
  tdb_index ix; memset(&ix, 0, sizeof(ix));
  ix.name = tdb_strdup(ci->name);
  ix.unique = ci->unique;
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
  /* append to the in-memory table so it is maintained this session */
  tdb_index *grown = (tdb_index *)tdb_realloc(t->indexes, sizeof(tdb_index) * (size_t)(t->nindex + 1));
  t->indexes = grown;
  t->indexes[t->nindex] = ix;
  t->nindex++;
  int rc = db->engine->vtab->create_index(db->engine, db->txn, t, &t->indexes[t->nindex - 1]);
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
    tdb_catalog_drop_table(db->cat, a->u.drop.name);
    return TDB_OK;
  }
  /* DROP INDEX / VIEW: in-memory only for now */
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
      if (db->txn) { rc = tdb_txn_commit(db->txn); db->txn = NULL; }
      db->autocommit = 1;
      break;
    case ST_ROLLBACK:
      if (db->txn) { rc = tdb_txn_rollback(db->txn); db->txn = NULL; }
      db->autocommit = 1;
      break;
    default: break;
  }
  return rc;
}
