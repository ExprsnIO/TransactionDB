/* tdb_exec_dml.inc — DML/DDL execution (included by tdb_exec.c). */

#include "../catalog/tdb_acl.h"
static int acl_check_or_err(tdb_db *db, const char *priv, int kind, const char *name);

/* -------------------- HASH partitioning helpers ----------------------- */

/* Hash a single tdb_value into 64 bits — used by partition routing. */
static uint64_t pv_hash(const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_NULL: return 0x9e3779b97f4a7c15ULL;
    case TDB_VAL_INT:  return (uint64_t)v->u.i * 0x100000001b3ULL ^ 0xcbf29ce484222325ULL;
    case TDB_VAL_REAL: { uint64_t u; memcpy(&u, &v->u.r, 8); return u * 0x100000001b3ULL; }
    default:
      if (!v->u.s.p) return 0;
      return tdb_fnv1a(v->u.s.p, (size_t)v->u.s.n);
  }
}

/* Pick the child partition table for `t` given the row's partition-key
** column values. Returns the catalog-resolved tdb_table or NULL on error. */
static tdb_table *partition_pick_child(tdb_db *db, tdb_table *t, const tdb_value *row) {
  if (t->partition_kind != TDB_PART_HASH || t->partition_count <= 0) return t;
  uint64_t h = 1469598103934665603ULL;
  for (int i = 0; i < t->npart_col; i++) {
    int ci = tdb_table_find_column(t, t->partition_cols[i]);
    if (ci < 0) {
      tdb_db_seterr(db, "partition column %s missing on %s", t->partition_cols[i], t->name);
      return NULL;
    }
    h = (h ^ pv_hash(&row[ci])) * 1099511628211ULL;
  }
  int k = (int)(h % (uint64_t)t->partition_count);
  char child[256];
  snprintf(child, sizeof(child), "%s__p%d", t->name, k);
  tdb_table *ct = tdb_catalog_find_table(db->cat, child);
  if (!ct) tdb_db_seterr(db, "missing partition %s", child);
  return ct;
}

/* Return non-zero if `t` is the parent of HASH-partitioned children. */
static int is_hash_partitioned(const tdb_table *t) {
  return t && t->partition_kind == TDB_PART_HASH && t->partition_count > 0;
}

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

/* finalize a full column row: generated cols, type coercion, NOT NULL, CHECK,
** period. */
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
  /* Column-level CHECK constraints. NULL (UNKNOWN) is treated as passing,
  ** matching SQLite's behaviour. */
  for (int i = 0; i < t->ncol; i++) {
    if (!t->cols[i].check_sql) continue;
    tdb_expr *ce = prepare_col_expr(db, st, t, t->cols[i].check_sql);
    if (!ce) return TDB_ERROR;
    ectx ec = { full, t->ncol, NULL, 0, st, NULL, NULL };
    tdb_value v; tdb_value_init(&v);
    eval(db, &ec, ce, &v);
    int ok = (v.type == TDB_VAL_NULL) ? 1 : val_true(&v);
    tdb_value_clear(&v);
    if (!ok) {
      tdb_db_seterr(db, "CHECK constraint failed: %s", t->cols[i].name);
      return TDB_CONSTRAINT;
    }
  }
  /* Table-level CHECK (if any). */
  if (t->check_sql) {
    tdb_expr *ce = prepare_col_expr(db, st, t, t->check_sql);
    if (!ce) return TDB_ERROR;
    ectx ec = { full, t->ncol, NULL, 0, st, NULL, NULL };
    tdb_value v; tdb_value_init(&v);
    eval(db, &ec, ce, &v);
    int ok = (v.type == TDB_VAL_NULL) ? 1 : val_true(&v);
    tdb_value_clear(&v);
    if (!ok) { tdb_db_seterr(db, "CHECK constraint failed"); return TDB_CONSTRAINT; }
  }
  return TDB_OK;
}

static int store_full(tdb_db *db, tdb_stmt *st, tdb_table *t, tdb_value *row,
                      tdb_rowid rowid, tdb_buf *rec, int is_update) {
  int rc = finalize_row(db, st, t, row);
  if (rc) return rc;
  /* Route to the correct child for HASH-partitioned parents. */
  tdb_table *target = t;
  if (is_hash_partitioned(t)) {
    target = partition_pick_child(db, t, row);
    if (!target) return TDB_ERROR;
  }
  tdb_buf_reset(rec);
  tdb_record_encode(row, target->ncol, rec);
  return is_update ? db->engine->vtab->update(db->engine, db->txn, target, rowid, rec->data, (int)rec->len)
                   : db->engine->vtab->insert(db->engine, db->txn, target, rowid, rec->data, (int)rec->len);
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
  if (!rc) {
    *applied = 1;
    db->last_insert_rowid = (int64_t)rowid;   /* sqlite3_last_insert_rowid mirror */
    returning_emit(db, st, rp, full, t->ncol);
  }
  free_row(full, t->ncol);
  return rc;
}

static int exec_insert(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.insert.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.insert.table); return TDB_ERROR; }
  if (acl_check_or_err(db, "INSERT", TDB_ACL_TABLE, a->u.insert.table) != TDB_OK) return TDB_ERROR;

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

/* materialize (rowid, values) pairs for UPDATE/DELETE. For a HASH-partitioned
** parent we sweep every child, tagging each row with its source table so the
** caller can hand the update/delete back to the right b-tree. */
typedef struct {
  tdb_rowid *rids;
  tdb_value **rows;
  tdb_table **srcs;       /* per-row source table (== caller's `t` for unpartitioned) */
  int n, cap, ncol;
} rrowset;

static int mat_rid_one(tdb_db *db, tdb_table *t, tdb_table *parent, rrowset *rr) {
  tdb_scan *sc;
  int rc = db->engine->vtab->scan_open(db->engine, db->txn, t, NULL, NULL, 0, NULL, &sc);
  if (rc) return rc;
  tdb_rowid rid; const uint8_t *rec; int reclen;
  while ((rc = db->engine->vtab->scan_next(sc, &rid, &rec, &reclen)) == TDB_ROW) {
    tdb_value tmp[64]; int nc = 0;
    if (tdb_record_decode(rec, (size_t)reclen, tmp, 64, &nc)) { rc = TDB_CORRUPT; break; }
    tdb_value *row = row_alloc(parent->ncol);
    for (int i = 0; i < parent->ncol && i < nc; i++) tdb_value_copy(&row[i], &tmp[i]);
    for (int i = 0; i < nc; i++) tdb_value_clear(&tmp[i]);
    if (rr->n == rr->cap) {
      rr->cap = rr->cap ? rr->cap * 2 : 16;
      rr->rids = (tdb_rowid *)tdb_realloc(rr->rids, sizeof(tdb_rowid) * (size_t)rr->cap);
      rr->rows = (tdb_value **)tdb_realloc(rr->rows, sizeof(void *) * (size_t)rr->cap);
      rr->srcs = (tdb_table **)tdb_realloc(rr->srcs, sizeof(void *) * (size_t)rr->cap);
    }
    rr->rids[rr->n] = rid; rr->rows[rr->n] = row; rr->srcs[rr->n] = t; rr->n++;
  }
  db->engine->vtab->scan_close(sc);
  return (rc == TDB_DONE) ? TDB_OK : rc;
}

static int mat_rid(tdb_db *db, tdb_table *t, rrowset *rr) {
  memset(rr, 0, sizeof(*rr)); rr->ncol = t->ncol;
  if (is_hash_partitioned(t)) {
    for (int k = 0; k < t->partition_count; k++) {
      char child[256];
      snprintf(child, sizeof(child), "%s__p%d", t->name, k);
      tdb_table *ct = tdb_catalog_find_table(db->cat, child);
      if (!ct) continue;
      int rc = mat_rid_one(db, ct, t, rr);
      if (rc) return rc;
    }
    return TDB_OK;
  }
  return mat_rid_one(db, t, t, rr);
}

static void rrowset_free(rrowset *rr) {
  for (int i = 0; i < rr->n; i++) free_row(rr->rows[i], rr->ncol);
  tdb_mfree(rr->rows); tdb_mfree(rr->rids); tdb_mfree(rr->srcs);
}

static int exec_update(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.update.table);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.update.table); return TDB_ERROR; }
  if (acl_check_or_err(db, "UPDATE", TDB_ACL_TABLE, a->u.update.table) != TDB_OK) return TDB_ERROR;

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
    /* For HASH-partitioned parents the row may need to migrate between
    ** children if the update changed a partition key. We always remove
    ** from the row's current source and let store_full route the new
    ** value to the (possibly different) target child. */
    tdb_table *src = rr.srcs[r];
    if (is_hash_partitioned(t)) {
      tdb_table *target = partition_pick_child(db, t, full);
      if (!target) { free_row(full, t->ncol); rc = TDB_ERROR; break; }
      if (target == src) {
        rc = db->engine->vtab->update(db->engine, db->txn, src, rr.rids[r], rec.data, (int)rec.len);
      } else {
        rc = db->engine->vtab->remove(db->engine, db->txn, src, rr.rids[r]);
        if (!rc) rc = db->engine->vtab->insert(db->engine, db->txn, target, rr.rids[r], rec.data, (int)rec.len);
      }
    } else {
      rc = db->engine->vtab->update(db->engine, db->txn, src, rr.rids[r], rec.data, (int)rec.len);
    }
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
  if (acl_check_or_err(db, "DELETE", TDB_ACL_TABLE, a->u.del.table) != TDB_OK) return TDB_ERROR;
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
    rc = db->engine->vtab->remove(db->engine, db->txn, rr.srcs[r], rr.rids[r]);
    if (rc) break;
    st->changes++;
    returning_emit(db, st, &rp, rr.rows[r], t->ncol);  /* RETURNING old row */
  }
  rrowset_free(&rr);
  ret_proj_free(&rp);
  return rc;
}

/* Pick a declared SQL type that fits the source value (best-effort, used by
** CREATE TABLE AS SELECT to derive a schema from the first projected row). */
static const char *infer_decl_type(const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_INT:  return "INTEGER";
    case TDB_VAL_REAL: return "REAL";
    case TDB_VAL_BLOB: return "BLOB";
    case TDB_VAL_COMPOSITE: return "COMPOSITE";
    default: return "TEXT";
  }
}

static int exec_create_table(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_create_table *ct = a->u.create_table;
  if (tdb_catalog_find_table(db->cat, ct->name)) {
    if (ct->if_not_exists) return TDB_OK;
    tdb_db_seterr(db, "table %s already exists", ct->name);
    return TDB_ERROR;
  }

  /* CREATE TABLE name AS SELECT ... — derive columns from the SELECT result. */
  if (ct->as_select) {
    tdb_stmt tmp;
    int rc = run_subselect(db, st, ct->as_select, &tmp, NULL);
    if (rc) { tdb_stmt_clear_results(&tmp); return rc; }
    /* synthesize columns named after the projection */
    tdb_create_table syn = *ct;
    syn.cols = (tdb_coldef *)tdb_calloc(sizeof(tdb_coldef) * (size_t)(tmp.ncol ? tmp.ncol : 1));
    syn.ncol = tmp.ncol;
    for (int i = 0; i < tmp.ncol; i++) {
      const char *nm = (tmp.colnames && tmp.colnames[i]) ? tmp.colnames[i] : "col";
      syn.cols[i].name = tdb_strdup(nm);
      syn.cols[i].coll = TDB_COLL_BINARY;
      const char *tn = "TEXT";
      if (tmp.nrows > 0) tn = infer_decl_type(&tmp.rows[0][i]);
      syn.cols[i].type = tdb_typespec_parse(tn);
    }
    char *err = NULL;
    tdb_table *t = tdb_ast_to_table(&syn, &err);
    for (int i = 0; i < syn.ncol; i++) tdb_mfree(syn.cols[i].name);
    tdb_mfree(syn.cols);
    if (!t) { tdb_db_seterr(db, "%s", err ? err : "invalid table"); tdb_mfree(err); tdb_stmt_clear_results(&tmp); return TDB_ERROR; }
    rc = db->engine->vtab->create_table(db->engine, db->txn, t);
    if (rc) { tdb_table_free(t); tdb_stmt_clear_results(&tmp); return rc; }
    rc = tdb_catalog_add_table(db->cat, t);
    if (rc) { tdb_table_free(t); tdb_stmt_clear_results(&tmp); return rc; }
    tdb_pager_set_schema_cookie(db->pager, tdb_pager_schema_cookie(db->pager) + 1);

    /* now bulk-insert the materialized rows */
    tdb_buf rec; tdb_buf_init(&rec);
    for (int r = 0; r < tmp.nrows && !rc; r++) {
      tdb_rowid rowid;
      db->engine->vtab->next_rowid(db->engine, t, &rowid);
      tdb_buf_reset(&rec);
      tdb_record_encode(tmp.rows[r], tmp.ncol, &rec);
      rc = db->engine->vtab->insert(db->engine, db->txn, t, rowid, rec.data, (int)rec.len);
      if (!rc) { st->changes++; db->last_insert_rowid = (int64_t)rowid; }
    }
    tdb_buf_free(&rec);
    tdb_stmt_clear_results(&tmp);
    return rc;
  }

  char *err = NULL;
  tdb_table *t = tdb_ast_to_table(ct, &err);
  if (!t) { tdb_db_seterr(db, "%s", err ? err : "invalid table"); tdb_mfree(err); return TDB_ERROR; }
  int rc = db->engine->vtab->create_table(db->engine, db->txn, t);
  if (rc) { tdb_table_free(t); return rc; }
  rc = tdb_catalog_add_table(db->cat, t);
  if (rc) { tdb_table_free(t); return rc; }
  tdb_pager_set_schema_cookie(db->pager, tdb_pager_schema_cookie(db->pager) + 1);

  /* HASH-partitioned parents own their data through N child tables named
  ** "<parent>__pK". Each child has the parent's column schema but no
  ** partitioning metadata of its own (data lives only in children). */
  if (t->partition_kind == TDB_PART_HASH && t->partition_count > 0) {
    for (int k = 0; k < t->partition_count; k++) {
      char child[256];
      snprintf(child, sizeof(child), "%s__p%d", t->name, k);
      if (tdb_catalog_find_table(db->cat, child)) continue;
      tdb_table *ck = tdb_table_new(child);
      if (!ck) return TDB_NOMEM;
      for (int i = 0; i < t->ncol; i++) {
        tdb_column col;
        tdb_column_init(&col, t->cols[i].name, t->cols[i].type);
        col.coll = t->cols[i].coll;
        col.notnull = t->cols[i].notnull;
        col.pk = t->cols[i].pk;
        col.unique = t->cols[i].unique;
        col.hidden = t->cols[i].hidden;
        if (t->cols[i].default_sql) col.default_sql = tdb_strdup(t->cols[i].default_sql);
        if (t->cols[i].check_sql) col.check_sql = tdb_strdup(t->cols[i].check_sql);
        if (tdb_table_add_column(ck, &col) != TDB_OK) { tdb_table_free(ck); return TDB_NOMEM; }
      }
      int rc2 = db->engine->vtab->create_table(db->engine, db->txn, ck);
      if (!rc2) rc2 = tdb_catalog_add_table(db->cat, ck);
      if (rc2) { tdb_table_free(ck); return rc2; }
    }
  }
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
  if (acl_check_or_err(db, "EXECUTE", TDB_ACL_PROCEDURE, a->u.call.name) != TDB_OK) return TDB_ERROR;
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
static int truncate_one(tdb_db *db, tdb_stmt *st, tdb_table *t) {
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

static int exec_truncate(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  tdb_table *t = tdb_catalog_find_table(db->cat, a->u.truncate.name);
  if (!t) { tdb_db_seterr(db, "no such table: %s", a->u.truncate.name); return TDB_ERROR; }
  if (is_hash_partitioned(t)) {
    for (int k = 0; k < t->partition_count; k++) {
      char child[256];
      snprintf(child, sizeof(child), "%s__p%d", t->name, k);
      tdb_table *ct = tdb_catalog_find_table(db->cat, child);
      if (!ct) continue;
      int rc = truncate_one(db, st, ct);
      if (rc) return rc;
    }
    return TDB_OK;
  }
  return truncate_one(db, st, t);
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
  TDB_UNUSED(st);
  const char *target = a->u.comment_on.target;
  int kind = a->u.comment_on.on_kind;
  if (!target || !*target) {
    tdb_db_seterr(db, "COMMENT ON requires a target name");
    return TDB_ERROR;
  }
  /* For the kinds we can introspect, refuse comments on unknown objects so
  ** typos surface immediately. INDEX/DATABASE pass through unchecked. */
  if (kind == 1) {                                            /* TABLE */
    if (!tdb_catalog_find_table(db->cat, target)) {
      tdb_db_seterr(db, "no such table: %s", target); return TDB_ERROR;
    }
  } else if (kind == 4) {                                     /* VIEW */
    if (!tdb_catalog_find_view(db->cat, target)) {
      tdb_db_seterr(db, "no such view: %s", target); return TDB_ERROR;
    }
  } else if (kind == 2) {                                     /* COLUMN tbl.col */
    const char *dot = strchr(target, '.');
    if (!dot || dot == target || !dot[1]) {
      tdb_db_seterr(db, "COMMENT ON COLUMN requires table.column"); return TDB_ERROR;
    }
    char *tbl = tdb_strndup(target, (size_t)(dot - target));
    tdb_table *t = tdb_catalog_find_table(db->cat, tbl);
    int rc = TDB_OK;
    if (!t) { tdb_db_seterr(db, "no such table: %s", tbl); rc = TDB_ERROR; }
    else if (tdb_table_find_column(t, dot + 1) < 0) {
      tdb_db_seterr(db, "no such column: %s.%s", tbl, dot + 1); rc = TDB_ERROR;
    }
    tdb_mfree(tbl);
    if (rc) return rc;
  }
  return tdb_catalog_set_comment(db->cat, kind, target, a->u.comment_on.body);
}

static int exec_tablespace_create(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}
static int exec_tablespace_drop(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st); TDB_UNUSED(db); TDB_UNUSED(a);
  return TDB_OK;
}

/* Split a comma/space-delimited privilege list into uppercase tokens. */
static char **split_privs(const char *src, int *out_n) {
  *out_n = 0;
  if (!src) return NULL;
  char **arr = NULL; int cap = 0, n = 0;
  const char *p = src;
  while (*p) {
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
    if (!*p) break;
    const char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
    int len = (int)(p - start);
    if (len > 0) {
      if (n == cap) { cap = cap ? cap * 2 : 4; arr = (char **)tdb_realloc(arr, sizeof(char *) * (size_t)cap); }
      char *tok = tdb_strndup(start, (size_t)len);
      for (char *q = tok; *q; q++) *q = (char)toupper((unsigned char)*q);
      arr[n++] = tok;
    }
  }
  *out_n = n;
  return arr;
}

/* Parse "TABLE name" / "INDEX name" / "FUNCTION name" / etc from grant.object;
** returns the TDB_ACL_* kind and a malloc'd object name (caller frees). */
static int parse_grant_object(const char *src, int *out_kind, char **out_name) {
  *out_kind = TDB_ACL_TABLE; *out_name = NULL;
  if (!src) return TDB_MISUSE;
  while (*src && isspace((unsigned char)*src)) src++;
  int kind = tdb_acl_kind_of(src);
  if (kind == 0) {
    /* bare name — default to TABLE */
    while (*src && isspace((unsigned char)*src)) src++;
    *out_name = tdb_strdup(src);
    return TDB_OK;
  }
  *out_kind = kind;
  const char *p = src + strlen(tdb_acl_kind_name(kind));
  while (*p && isspace((unsigned char)*p)) p++;
  *out_name = tdb_strdup(p);
  return TDB_OK;
}

static int exec_grant(tdb_db *db, tdb_stmt *st, tdb_ast_stmt *a) {
  TDB_UNUSED(st);
  int npriv = 0;
  char **privs = split_privs(a->u.grant.privs, &npriv);
  int kind = 0; char *obj = NULL;
  if (parse_grant_object(a->u.grant.object, &kind, &obj) != TDB_OK) {
    for (int i = 0; i < npriv; i++) tdb_mfree(privs[i]);
    tdb_mfree(privs);
    tdb_db_seterr(db, "bad object spec in %s", a->kind == ST_GRANT ? "GRANT" : "REVOKE");
    return TDB_ERROR;
  }
  if (!obj || !*obj) {
    for (int i = 0; i < npriv; i++) tdb_mfree(privs[i]);
    tdb_mfree(privs); tdb_mfree(obj);
    tdb_db_seterr(db, "missing object name in %s", a->kind == ST_GRANT ? "GRANT" : "REVOKE");
    return TDB_ERROR;
  }
  tdb_acl *acl = tdb_catalog_acl(db->cat);
  const char *grantee = a->u.grant.grantee ? a->u.grant.grantee : "PUBLIC";
  int rc = TDB_OK;
  if (a->kind == ST_GRANT) {
    for (int i = 0; i < npriv && !rc; i++) {
      rc = tdb_acl_grant(acl, grantee, privs[i], kind, obj);
      if (!rc) {
        tdb_acl_entry e = { (char *)grantee, privs[i], kind, obj, NULL };
        rc = tdb_catalog_acl_persist(db->cat, &e);
      }
    }
  } else {
    for (int i = 0; i < npriv && !rc; i++) {
      tdb_acl_revoke(acl, grantee, privs[i], kind, obj);
      rc = tdb_catalog_acl_unpersist(db->cat, grantee, privs[i], kind, obj, NULL);
    }
  }
  for (int i = 0; i < npriv; i++) tdb_mfree(privs[i]);
  tdb_mfree(privs); tdb_mfree(obj);
  return rc;
}

/* Privilege gate for SELECT/INSERT/UPDATE/DELETE/CALL/etc.
** Returns TDB_OK if the current connection user has the privilege, else
** TDB_ERROR with an error message set. */
static int acl_check_or_err(tdb_db *db, const char *priv, int kind, const char *name) {
  if (!db->current_user) return TDB_OK;       /* open / superuser mode */
  if (tdb_acl_check(tdb_catalog_acl(db->cat), db->current_user, priv, kind, name))
    return TDB_OK;
  tdb_db_seterr(db, "permission denied: %s on %s '%s' for user '%s'",
                priv ? priv : "?", tdb_acl_kind_name(kind),
                name ? name : "?", db->current_user);
  return TDB_ERROR;
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
