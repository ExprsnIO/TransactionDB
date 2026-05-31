/* tdb_exec_stream.inc — pull-based (volcano) operator tree.
**
** Included by tdb_exec.c after the materializing executor, this introduces a
** small family of demand-driven operators — Scan, Filter, Sort, Project, Limit
** — wired child-to-parent so each next() pulls one row from below. A SELECT
** that is a single base-table scan with an optional WHERE / ORDER BY /
** projection / LIMIT is compiled into such a tree; the row at a time pull means
** a LIMIT stops the scan early instead of materializing the whole table first.
** Sort is the one blocking operator: it drains its child once, but when ORDER
** BY is paired with LIMIT it keeps only the top (OFFSET+LIMIT) rows in a
** bounded heap rather than buffering the entire input.
**
** tdb_step() pulls one row per call straight through the tree (see
** tdb_stmt_stream_next): nothing is collected into st->rows, so memory stays
** bounded regardless of result size. The statement adopts the auto read
** transaction and holds that snapshot open across steps, releasing it when the
** stream is exhausted, reset, or finalized — so the scan sees a stable snapshot
** even as the connection commits other writes.
*/

typedef enum { OP_SCAN, OP_FILTER, OP_SORT, OP_PROJECT, OP_LIMIT } op_kind;

/* one buffered row inside a Sort operator: the (owned) source row and its
** computed sort-key vector */
typedef struct { tdb_value *row; tdb_value *key; } sortent;

typedef struct tdb_op {
  op_kind        kind;
  struct tdb_op *child;
  tdb_db        *db;
  tdb_stmt      *st;

  /* OP_SCAN */
  tdb_table     *tbl;
  tdb_scan      *scan;
  tdb_value     *scanrow;     /* reused decoded row (tbl->ncol values) */
  tdb_index     *idx;
  tdb_keyrange   range;
  int            has_range;
  uint8_t        colmask[64];
  int            has_mask;

  /* OP_FILTER */
  tdb_expr      *pred;

  /* OP_PROJECT */
  tdb_expr     **oexpr;
  int            nout;
  tdb_value     *outrow;      /* reused projected row (nout values) */

  /* OP_LIMIT */
  int64_t        limit, offset, emitted, skipped;
  int            has_limit;

  /* OP_SORT (blocking; bounded top-N when capN >= 0) */
  tdb_expr     **okeys;       /* ORDER BY key expressions (resolved on source) */
  uint8_t       *odesc;       /* per-key descending flag (points into the AST) */
  int            nkey;
  int64_t        capN;        /* keep only the best capN rows, or -1 = all */
  sortent       *heap;        /* max-heap (root = worst kept) while filling */
  int            nheap, heapcap;
  int            sorted;      /* input consumed + ordered */
  int            emit;        /* next index to emit */
  int            srcncol;     /* width of buffered source rows */
} tdb_op;

/* ---- eligibility: reject anything the simple operators can't stream ---- */

/* True if `e` contains an aggregate or a subquery (which Stage 1 won't stream). */
static int expr_blocks_stream(const tdb_expr *e) {
  if (!e) return 0;
  switch (e->kind) {
    case EX_AGG: case EX_SUBQUERY: case EX_EXISTS:
      return 1;
    case EX_IN:
      if (e->subquery) return 1;
      break;
    default: break;
  }
  if (expr_blocks_stream(e->left) || expr_blocks_stream(e->right)) return 1;
  if (e->args) for (int i = 0; i < e->args->n; i++)
    if (expr_blocks_stream(e->args->items[i])) return 1;
  return 0;
}

static int select_streamable(tdb_db *db, tdb_stmt *st, tdb_select *q, tdb_table **out_tbl) {
  if (q->setop || q->group || q->having || q->distinct) return 0;
  if (q->with) return 0;        /* its CTEs (which may shadow a table) aren't in scope here */
  if (!q->from || q->from->next || q->from->subquery) return 0;
  if (!q->from->table || q->from->as_of) return 0;
  if (find_cte(st, q->from->table)) return 0;            /* CTE name shadows a table */
  tdb_table *t = tdb_catalog_find_table(db->cat, q->from->table);
  if (!t) return 0;                                      /* view / missing: not here */
  if (expr_blocks_stream(q->where)) return 0;
  if (q->cols) for (int i = 0; i < q->cols->n; i++)
    if (expr_blocks_stream(q->cols->items[i])) return 0;
  if (q->order) for (int i = 0; i < q->order->n; i++)   /* ORDER BY streams via a Sort op */
    if (expr_blocks_stream(q->order->exprs[i])) return 0;
  *out_tbl = t;
  return 1;
}

/* ------------------------------ next() -------------------------------- */

static int op_next(tdb_op *op, tdb_value **row, int *ncol);

static int scan_next(tdb_op *op, tdb_value **row, int *ncol) {
  tdb_rowid rid; const uint8_t *rec; int reclen;
  int rc = op->db->engine->vtab->scan_next(op->scan, &rid, &rec, &reclen);
  if (rc != TDB_ROW) return rc;                          /* TDB_DONE or error */
  tdb_value tmp[64]; int nc = 0;
  if (tdb_record_decode(rec, (size_t)reclen, tmp, 64, &nc)) return TDB_CORRUPT;
  for (int i = 0; i < op->tbl->ncol; i++) {
    tdb_value_clear(&op->scanrow[i]);
    if (i < nc) tdb_value_copy(&op->scanrow[i], &tmp[i]);
    else tdb_value_init(&op->scanrow[i]);
  }
  for (int i = 0; i < nc; i++) tdb_value_clear(&tmp[i]);
  *row = op->scanrow; *ncol = op->tbl->ncol;
  return TDB_ROW;
}

static int filter_next(tdb_op *op, tdb_value **row, int *ncol) {
  for (;;) {
    tdb_value *r; int nc;
    int rc = op_next(op->child, &r, &nc);
    if (rc != TDB_ROW) return rc;
    ectx ec = { r, nc, NULL, 0, op->st, NULL, NULL };
    tdb_value v; tdb_value_init(&v);
    eval(op->db, &ec, op->pred, &v);
    int ok = val_true(&v);
    tdb_value_clear(&v);
    if (ok) { *row = r; *ncol = nc; return TDB_ROW; }
  }
}

static int project_next(tdb_op *op, tdb_value **row, int *ncol) {
  tdb_value *r; int nc;
  int rc = op_next(op->child, &r, &nc);
  if (rc != TDB_ROW) return rc;
  ectx ec = { r, nc, NULL, 0, op->st, NULL, NULL };
  for (int i = 0; i < op->nout; i++) {
    tdb_value_clear(&op->outrow[i]);
    eval(op->db, &ec, op->oexpr[i], &op->outrow[i]);
  }
  *row = op->outrow; *ncol = op->nout;
  return TDB_ROW;
}

static int limit_next(tdb_op *op, tdb_value **row, int *ncol) {
  while (op->skipped < op->offset) {
    tdb_value *r; int nc;
    int rc = op_next(op->child, &r, &nc);
    if (rc != TDB_ROW) return rc;
    op->skipped++;
  }
  if (op->has_limit && op->emitted >= op->limit) return TDB_DONE;
  tdb_value *r; int nc;
  int rc = op_next(op->child, &r, &nc);
  if (rc != TDB_ROW) return rc;
  op->emitted++;
  *row = r; *ncol = nc;
  return TDB_ROW;
}

/* compare two key vectors by the op's ORDER BY keys/desc; <0 if a sorts first */
static int sortkey_cmp(const tdb_op *op, const tdb_value *a, const tdb_value *b) {
  for (int k = 0; k < op->nkey; k++) {
    int c = tdb_value_compare(&a[k], &b[k], TDB_COLL_BINARY);
    if (op->odesc && op->odesc[k]) c = -c;
    if (c) return c;
  }
  return 0;
}

/* max-heap on op->heap[0..nheap): the root is the entry that sorts LAST, i.e.
** the worst one to keep, so a bounded top-N evicts it first. */
static void heap_sift_up(tdb_op *op, int i) {
  while (i > 0) {
    int p = (i - 1) / 2;
    if (sortkey_cmp(op, op->heap[i].key, op->heap[p].key) <= 0) break;
    sortent t = op->heap[i]; op->heap[i] = op->heap[p]; op->heap[p] = t;
    i = p;
  }
}
static void heap_sift_down(tdb_op *op, int i) {
  for (;;) {
    int l = 2 * i + 1, r = 2 * i + 2, m = i;
    if (l < op->nheap && sortkey_cmp(op, op->heap[l].key, op->heap[m].key) > 0) m = l;
    if (r < op->nheap && sortkey_cmp(op, op->heap[r].key, op->heap[m].key) > 0) m = r;
    if (m == i) break;
    sortent t = op->heap[i]; op->heap[i] = op->heap[m]; op->heap[m] = t;
    i = m;
  }
}
static void sortent_free(sortent *e, int ncol, int nkey) {
  if (e->row) { for (int i = 0; i < ncol; i++) tdb_value_clear(&e->row[i]); tdb_mfree(e->row); e->row = NULL; }
  if (e->key) { for (int i = 0; i < nkey; i++) tdb_value_clear(&e->key[i]); tdb_mfree(e->key); e->key = NULL; }
}

/* Consume the child once, buffering rows (bounded to capN via the heap when
** capN >= 0), then order the buffer ascending in place for emission. */
static int sort_materialize(tdb_op *op) {
  for (;;) {
    tdb_value *r; int nc;
    int rc = op_next(op->child, &r, &nc);
    if (rc == TDB_DONE) break;
    if (rc != TDB_ROW) return rc;
    op->srcncol = nc;
    ectx ec = { r, nc, NULL, 0, op->st, NULL, NULL };
    tdb_value *key = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)(op->nkey ? op->nkey : 1));
    for (int k = 0; k < op->nkey; k++) { tdb_value_init(&key[k]); eval(op->db, &ec, op->okeys[k], &key[k]); }

    if (op->capN >= 0 && op->nheap == (int)op->capN) {
      /* heap full: keep this row only if it sorts before the current worst */
      if (op->capN == 0 || sortkey_cmp(op, key, op->heap[0].key) >= 0) {
        for (int k = 0; k < op->nkey; k++) tdb_value_clear(&key[k]);
        tdb_mfree(key);
        continue;
      }
      sortent_free(&op->heap[0], op->srcncol, op->nkey);
      op->heap[0].row = dup_row(r, nc); op->heap[0].key = key;
      heap_sift_down(op, 0);
    } else {
      if (op->nheap == op->heapcap) {
        op->heapcap = op->heapcap ? op->heapcap * 2 : 64;
        op->heap = (sortent *)tdb_realloc(op->heap, sizeof(sortent) * (size_t)op->heapcap);
      }
      op->heap[op->nheap].row = dup_row(r, nc);
      op->heap[op->nheap].key = key;
      op->nheap++;
      heap_sift_up(op, op->nheap - 1);
    }
  }
  /* heapsort: repeatedly move the max (root) to the shrinking tail, leaving the
  ** buffer ordered ascending for emission */
  int n = op->nheap;
  for (int end = n - 1; end > 0; end--) {
    sortent t = op->heap[0]; op->heap[0] = op->heap[end]; op->heap[end] = t;
    op->nheap = end;
    heap_sift_down(op, 0);
  }
  op->nheap = n;
  op->sorted = 1; op->emit = 0;
  return TDB_OK;
}

static int sort_next(tdb_op *op, tdb_value **row, int *ncol) {
  if (!op->sorted) { int rc = sort_materialize(op); if (rc) return rc; }
  if (op->emit >= op->nheap) return TDB_DONE;
  *row = op->heap[op->emit].row;
  *ncol = op->srcncol;
  op->emit++;
  return TDB_ROW;
}

static int op_next(tdb_op *op, tdb_value **row, int *ncol) {
  switch (op->kind) {
    case OP_SCAN:    return scan_next(op, row, ncol);
    case OP_FILTER:  return filter_next(op, row, ncol);
    case OP_SORT:    return sort_next(op, row, ncol);
    case OP_PROJECT: return project_next(op, row, ncol);
    case OP_LIMIT:   return limit_next(op, row, ncol);
  }
  return TDB_DONE;
}

/* ------------------------------ close() ------------------------------- */

static void op_close(tdb_op *op) {
  if (!op) return;
  if (op->child) op_close(op->child);
  if (op->kind == OP_SCAN) {
    if (op->scan) op->db->engine->vtab->scan_close(op->scan);
    if (op->scanrow) { for (int i = 0; i < op->tbl->ncol; i++) tdb_value_clear(&op->scanrow[i]); tdb_mfree(op->scanrow); }
  } else if (op->kind == OP_PROJECT) {
    if (op->outrow) { for (int i = 0; i < op->nout; i++) tdb_value_clear(&op->outrow[i]); tdb_mfree(op->outrow); }
    tdb_mfree(op->oexpr);    /* the array (its exprs are arena-owned) */
  } else if (op->kind == OP_SORT) {
    for (int i = 0; i < op->nheap; i++) sortent_free(&op->heap[i], op->srcncol, op->nkey);
    tdb_mfree(op->heap);     /* okeys/odesc point into the AST (arena) */
  }
  tdb_mfree(op);
}

/* ------------------------------ build --------------------------------- */

static tdb_op *op_new(op_kind k, tdb_db *db, tdb_stmt *st) {
  tdb_op *op = (tdb_op *)tdb_calloc(sizeof(*op));
  if (op) { op->kind = k; op->db = db; op->st = st; }
  return op;
}

/* Build a Scan -> [Filter] -> Project -> [Limit] tree for a streamable SELECT,
** or return NULL (caller falls back to the materializing executor). On success
** st->ncol / st->colnames are set and the scan is opened against `txn`. */
static tdb_op *build_stream_plan(tdb_db *db, tdb_stmt *st, tdb_select *q, tdb_txn *txn) {
  tdb_table *t = NULL;
  if (!select_streamable(db, st, q, &t)) return NULL;

  /* resolve WHERE + projection against a single-source context */
  qctx Q; memset(&Q, 0, sizeof(Q));
  Q.src[0].tbl = t; Q.src[0].base = 0; Q.src[0].ncol = t->ncol;
  Q.nsrc = 1; Q.totalcols = t->ncol;

  if (q->where && resolve_expr(db, &Q, q->where)) return NULL;
  if (q->order) for (int i = 0; i < q->order->n; i++)
    if (resolve_expr(db, &Q, q->order->exprs[i])) return NULL;

  /* projection: expand '*', resolve exprs, collect output names */
  tdb_expr **oexpr = NULL; char **onames = NULL; int nout = 0, ocap = 0;
  #define SADD_OUT(E, NM) do { \
    if (nout == ocap) { ocap = ocap ? ocap*2 : 8; \
      oexpr = (tdb_expr**)tdb_realloc(oexpr, sizeof(void*)*(size_t)ocap); \
      onames = (char**)tdb_realloc(onames, sizeof(void*)*(size_t)ocap); } \
    oexpr[nout] = (E); onames[nout] = tdb_strdup((NM) ? (NM) : "column"); nout++; \
  } while (0)
  #define SFAIL do { for (int _si = 0; _si < nout; _si++) tdb_mfree(onames[_si]); \
    tdb_mfree(oexpr); tdb_mfree(onames); return NULL; } while (0)

  for (int i = 0; i < q->cols->n; i++) {
    tdb_expr *item = q->cols->items[i];
    char *alias = q->cols->aliases[i];
    if (item->kind == EX_STAR) {
      if (item->table) {
        const char *nm = t->name;
        if (strcasecmp(nm, item->table) != 0) SFAIL;
      }
      for (int k = 0; k < t->ncol; k++) {
        if (t->cols[k].hidden) continue;
        tdb_expr *col = tdb_expr_new(st->arena, EX_COLUMN);
        col->name = (char *)t->cols[k].name;
        col->col_index = k;
        SADD_OUT(col, t->cols[k].name);
      }
    } else {
      if (resolve_expr(db, &Q, item)) SFAIL;
      const char *nm = alias ? alias : (item->kind == EX_COLUMN ? item->name : NULL);
      SADD_OUT(item, nm);
    }
  }

  /* index / spatial pick + projection pushdown mask for the scan */
  tdb_index *idx = NULL; tdb_keyrange kr; int have_idx = 0;
  tdb_value_init(&kr.lo); tdb_value_init(&kr.hi);
  if (t->nindex > 0 && pick_index(db, st, t, q->where, &idx, &kr)) have_idx = 1;

  /* The scan op holds the colmask (the columnar engine keeps the pointer and
  ** dereferences it during the drain — it must outlive this function). */
  tdb_op *scan = op_new(OP_SCAN, db, st);
  scan->tbl = t;
  scan->has_mask = compute_colmask(t, q, scan->colmask);

  tdb_scan *sc = NULL;
  int rc = db->engine->vtab->scan_open(db->engine, txn, t, have_idx ? idx : NULL,
                                       have_idx ? &kr : NULL, 0,
                                       scan->has_mask ? scan->colmask : NULL, &sc);
  tdb_value_clear(&kr.lo); tdb_value_clear(&kr.hi);
  if (rc) { op_close(scan); SFAIL; }

  /* assemble the operator tree */
  scan->scan = sc;
  scan->scanrow = row_alloc(t->ncol);

  tdb_op *src = scan;
  if (q->where) {
    tdb_op *flt = op_new(OP_FILTER, db, st);
    flt->child = src; flt->pred = q->where;
    src = flt;
  }

  /* ORDER BY: sort before projection so keys can reference any source column.
  ** With a LIMIT the sort is bounded to the top (OFFSET+LIMIT) rows. */
  if (q->order) {
    tdb_op *sort = op_new(OP_SORT, db, st);
    sort->child = src;
    sort->okeys = q->order->exprs;
    sort->odesc = q->order->desc;
    sort->nkey  = q->order->n;
    if (q->has_limit) {
      int64_t lim = const_int(db, st, q->limit, -1);
      int64_t off = q->offset ? const_int(db, st, q->offset, 0) : 0;
      sort->capN = (lim < 0) ? -1 : (lim + (off > 0 ? off : 0));
    } else {
      sort->capN = -1;
    }
    src = sort;
  }

  tdb_op *proj = op_new(OP_PROJECT, db, st);
  proj->child = src; proj->oexpr = oexpr; proj->nout = nout;
  proj->outrow = row_alloc(nout);
  src = proj;

  if (q->has_limit || q->offset) {
    tdb_op *lim = op_new(OP_LIMIT, db, st);
    lim->child = src;
    lim->has_limit = q->has_limit;
    lim->limit = q->has_limit ? const_int(db, st, q->limit, -1) : -1;
    lim->offset = q->offset ? const_int(db, st, q->offset, 0) : 0;
    if (lim->limit < 0) lim->has_limit = 0;
    if (lim->offset < 0) lim->offset = 0;
    src = lim;
  }

  st->ncol = nout;
  st->colnames = onames;     /* ownership transfers to the statement */
  #undef SADD_OUT
  #undef SFAIL
  return src;
}

/* Entry point from statement execution: if `q` is streamable, build the
** operator tree and arm the statement for lazy pulling (tdb_step pulls one row
** per call). Returns 1 if streaming was set up (rc in *out_rc), 0 to fall back
** to the materializing executor.
**
** `txn` is the transaction the scan reads through (the connection's current
** txn during execution). When `own` is true that txn is an auto-transaction the
** statement adopts and keeps open across steps; the caller has already detached
** it from the connection. */
static int setup_stream(tdb_db *db, tdb_stmt *st, tdb_select *q, tdb_txn *txn,
                        int own, int *out_rc) {
  tdb_op *root = build_stream_plan(db, st, q, txn);
  if (!root) return 0;
  st->plan = root;
  st->streaming = 1;
  st->stream_done = 0;
  st->own_txn = own ? txn : NULL;
  st->stream_row = row_alloc(st->ncol);
  st->cur = -1;
  *out_rc = TDB_OK;
  return 1;
}

/* Tear down a streaming statement: close the operator tree and release the
** statement-held read snapshot. Idempotent. Touches the shared pager cache
** (scan close), so it runs under the env guard. */
void tdb_stmt_stream_close(tdb_stmt *st) {
  if (!st) return;
  tdb_env_enter(st->db->env);
  if (st->plan) { op_close((tdb_op *)st->plan); st->plan = NULL; }
  if (st->own_txn) { tdb_txn_rollback(st->own_txn); st->own_txn = NULL; }
  tdb_env_leave(st->db->env);
  if (st->stream_row) {
    for (int i = 0; i < st->ncol; i++) tdb_value_clear(&st->stream_row[i]);
    tdb_mfree(st->stream_row);
    st->stream_row = NULL;
  }
  st->streaming = 0;
  st->stream_done = 0;
}

/* Pull the next row through the operator tree into st->stream_row. Returns
** TDB_ROW / TDB_DONE / an error code. On end-of-stream or error the tree and
** snapshot are released eagerly (the result is no longer needed). */
int tdb_stmt_stream_next(tdb_stmt *st) {
  if (!st->plan || st->stream_done) return TDB_DONE;
  tdb_db *db = st->db;
  tdb_env_enter(db->env);
  tdb_value *row; int nc;
  int rc = op_next((tdb_op *)st->plan, &row, &nc);
  if (rc == TDB_ROW) {
    for (int i = 0; i < st->ncol && i < nc; i++) {
      tdb_value_clear(&st->stream_row[i]);
      tdb_value_copy(&st->stream_row[i], &row[i]);
    }
  }
  tdb_env_leave(db->env);
  if (rc != TDB_ROW) {
    st->stream_done = 1;
    tdb_stmt_stream_close(st);   /* release the snapshot promptly */
  }
  return rc;
}
