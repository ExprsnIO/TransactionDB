/* tdb_exec_stream.inc — pull-based (volcano) operator tree.
**
** Included by tdb_exec.c after the materializing executor, this introduces a
** small family of demand-driven operators — Scan, Join, Filter, Sort, Project,
** Limit — wired child-to-parent so each next() pulls one row from below. A
** SELECT over one or more base tables (INNER/CROSS joins, left-deep) with an
** optional WHERE / ORDER BY / projection / LIMIT is compiled into such a tree;
** the row at a time pull means a LIMIT stops the scan early instead of
** materializing the whole result first. Join is a nested loop: it re-scans the
** inner table for each outer row, or seeks it via an index when the ON predicate
** is an equijoin on an indexed inner column. Sort is the one blocking operator: it drains
** its child once, but when ORDER BY is paired with LIMIT it keeps only the top
** (OFFSET+LIMIT) rows in a bounded heap rather than buffering the entire input.
**
** tdb_step() pulls one row per call straight through the tree (see
** tdb_stmt_stream_next): nothing is collected into st->rows, so memory stays
** bounded regardless of result size. The statement adopts the auto read
** transaction and holds that snapshot open across steps, releasing it when the
** stream is exhausted, reset, or finalized — so the scan sees a stable snapshot
** even as the connection commits other writes.
*/

typedef enum { OP_SCAN, OP_JOIN, OP_FILTER, OP_SORT, OP_PROJECT, OP_LIMIT } op_kind;

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
  tdb_txn       *txn;         /* read transaction (for re-opening on rewind) */

  /* OP_FILTER */
  tdb_expr      *pred;

  /* OP_JOIN (left-deep nested loop over INNER/CROSS base-table joins). `child`
  ** is the outer (left) subtree; `inner` is the right Scan, re-scanned per
  ** outer row. The join emits a combined row of `joinncol` values: the outer
  ** row's columns followed by the inner table's, at offset `inner_base`. */
  struct tdb_op *inner;
  tdb_expr      *on;          /* INNER join ON predicate (NULL for CROSS) */
  tdb_value     *joinrow;     /* reused combined row (joinncol values) */
  int            joinncol;
  int            inner_base;  /* column offset of the inner table in joinrow */
  int            join_started;/* outer row pulled at least once */
  int            join_eof;
  /* index-driven inner lookup: when the ON predicate has an equijoin on an
  ** indexed inner column, seek the inner table to the outer value each pass
  ** instead of full-scanning. `idx_join_key` is the outer expression supplying
  ** the probe value; `inner_idx` is the inner index to seek. */
  tdb_index     *inner_idx;
  tdb_expr      *idx_join_key;

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

/* A streamable FROM is one or more base tables joined left-deep by INNER or
** CROSS joins (no derived tables, views, outer joins, AS OF, or CTE shadowing).
** Returns the source count (>=1) or 0 if not streamable. */
static int from_streamable(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  int n = 0;
  for (tdb_src *s = q->from; s; s = s->next) {
    if (s->subquery || !s->table || s->as_of) return 0;
    if (find_cte(st, s->table)) return 0;                /* CTE name shadows a table */
    if (!tdb_catalog_find_table(db->cat, s->table)) return 0;   /* view / missing */
    if (n > 0 && s->join != JOIN_INNER && s->join != JOIN_CROSS) return 0;
    if (s->join == JOIN_INNER && expr_blocks_stream(s->on)) return 0;
    if (++n > MAXSRC) return 0;
  }
  return n;
}

static int select_streamable(tdb_db *db, tdb_stmt *st, tdb_select *q) {
  if (q->setop || q->group || q->having || q->distinct) return 0;
  if (q->with) return 0;        /* its CTEs (which may shadow a table) aren't in scope here */
  if (from_streamable(db, st, q) < 1) return 0;
  if (expr_blocks_stream(q->where)) return 0;
  if (q->cols) for (int i = 0; i < q->cols->n; i++)
    if (expr_blocks_stream(q->cols->items[i])) return 0;
  if (q->order) for (int i = 0; i < q->order->n; i++)   /* ORDER BY streams via a Sort op */
    if (expr_blocks_stream(q->order->exprs[i])) return 0;
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

/* Re-open a Scan operator's underlying cursor from the start (inner side of a
** nested-loop join, re-scanned once per outer row). */
static int scan_rewind(tdb_op *op) {
  if (op->scan) { op->db->engine->vtab->scan_close(op->scan); op->scan = NULL; }
  return op->db->engine->vtab->scan_open(op->db->engine, op->txn, op->tbl,
                                         NULL, NULL, 0,
                                         op->has_mask ? op->colmask : NULL, &op->scan);
}

/* Re-open a Scan operator seeking `idx` to the equality value `key` (one outer
** probe of an index-driven join). Returns rows whose leading index column
** equals `key`; the join's ON predicate is still re-checked per row. */
static int scan_rewind_seek(tdb_op *op, tdb_index *idx, const tdb_value *key) {
  if (op->scan) { op->db->engine->vtab->scan_close(op->scan); op->scan = NULL; }
  tdb_keyrange kr; memset(&kr, 0, sizeof(kr));
  tdb_value_init(&kr.lo); tdb_value_init(&kr.hi);
  tdb_value_copy(&kr.lo, key); kr.has_lo = 1; kr.lo_incl = 1;
  tdb_value_copy(&kr.hi, key); kr.has_hi = 1; kr.hi_incl = 1;
  int rc = op->db->engine->vtab->scan_open(op->db->engine, op->txn, op->tbl,
                                           idx, &kr, 0, NULL, &op->scan);
  tdb_value_clear(&kr.lo); tdb_value_clear(&kr.hi);
  return rc;
}

/* Nested-loop join: for each outer (left) row, scan the inner (right) table and
** emit the combined rows that satisfy the ON predicate (all rows for CROSS).
** The combined row is the outer row's columns followed by the inner table's. */
static int join_next(tdb_op *op, tdb_value **row, int *ncol) {
  for (;;) {
    if (!op->join_started || op->join_eof) {
      /* advance the outer side and (re)start the inner scan */
      tdb_value *orow; int onc;
      int rc = op_next(op->child, &orow, &onc);
      if (rc != TDB_ROW) return rc;
      for (int i = 0; i < op->inner_base; i++) {
        tdb_value_clear(&op->joinrow[i]);
        if (i < onc) tdb_value_copy(&op->joinrow[i], &orow[i]);
      }
      if (op->inner_idx) {
        /* index-driven probe: seek the inner table to the outer key value */
        ectx ec = { op->joinrow, op->inner_base, NULL, 0, op->st, NULL, NULL };
        tdb_value probe; tdb_value_init(&probe);
        eval(op->db, &ec, op->idx_join_key, &probe);
        if (probe.type == TDB_VAL_NULL) {       /* NULL never equals: skip outer row */
          tdb_value_clear(&probe);
          op->join_eof = 1; op->join_started = 1; continue;
        }
        rc = scan_rewind_seek(op->inner, op->inner_idx, &probe);
        tdb_value_clear(&probe);
      } else {
        rc = scan_rewind(op->inner);
      }
      if (rc) return rc;
      op->join_started = 1; op->join_eof = 0;
    }
    /* pull the next inner row */
    tdb_value *irow; int inc;
    int rc = scan_next(op->inner, &irow, &inc);
    if (rc == TDB_DONE) { op->join_eof = 1; continue; }   /* outer advances */
    if (rc != TDB_ROW) return rc;
    for (int i = 0; i < op->inner->tbl->ncol; i++) {
      tdb_value_clear(&op->joinrow[op->inner_base + i]);
      if (i < inc) tdb_value_copy(&op->joinrow[op->inner_base + i], &irow[i]);
    }
    if (op->on) {
      ectx ec = { op->joinrow, op->joinncol, NULL, 0, op->st, NULL, NULL };
      tdb_value v; tdb_value_init(&v);
      eval(op->db, &ec, op->on, &v);
      int ok = val_true(&v);
      tdb_value_clear(&v);
      if (!ok) continue;
    }
    *row = op->joinrow; *ncol = op->joinncol;
    return TDB_ROW;
  }
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
    case OP_JOIN:    return join_next(op, row, ncol);
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
  } else if (op->kind == OP_JOIN) {
    if (op->inner) op_close(op->inner);
    if (op->joinrow) { for (int i = 0; i < op->joinncol; i++) tdb_value_clear(&op->joinrow[i]); tdb_mfree(op->joinrow); }
  }
  tdb_mfree(op);
}

/* ------------------------------ build --------------------------------- */

/* True if every column reference in `e` resolves to a slot < `limit` (i.e. it
** depends only on already-produced outer columns, not the inner table). */
static int expr_below(const tdb_expr *e, int limit) {
  if (!e) return 1;
  if (e->kind == EX_COLUMN) return e->col_index >= 0 && e->col_index < limit;
  if (e->kind == EX_AGG || e->kind == EX_SUBQUERY || e->kind == EX_EXISTS) return 0;
  if (!expr_below(e->left, limit) || !expr_below(e->right, limit)) return 0;
  if (e->args) for (int i = 0; i < e->args->n; i++)
    if (!expr_below(e->args->items[i], limit)) return 0;
  return 1;
}

/* If `on` is (or AND-contains) an equality `outer_expr = inner.col` where
** inner.col (a column of `inner_src`, base `inner_base`) carries a usable
** single-column index and outer_expr depends only on outer columns, return that
** index and set *probe to the outer-side expression. Else NULL. */
static tdb_index *find_join_index(tdb_db *db, const tdb_src *fsrc,
                                  const qsrc *inner_src, int inner_base,
                                  tdb_expr *on, tdb_expr **probe) {
  TDB_UNUSED(db);
  if (!on) return NULL;
  if (on->kind == EX_BINARY && on->op == TK_AND) {
    tdb_index *l = find_join_index(db, fsrc, inner_src, inner_base, on->left, probe);
    if (l) return l;
    return find_join_index(db, fsrc, inner_src, inner_base, on->right, probe);
  }
  if (on->kind != EX_BINARY || on->op != TK_EQ) return NULL;
  tdb_table *it = inner_src->tbl;
  int hi = inner_base + it->ncol;   /* inner column slots: [inner_base, hi) */

  for (int side = 0; side < 2; side++) {
    tdb_expr *icol = side ? on->right : on->left;
    tdb_expr *ocol = side ? on->left  : on->right;
    if (icol->kind != EX_COLUMN) continue;
    if (icol->col_index < inner_base || icol->col_index >= hi) continue; /* not inner */
    if (!expr_below(ocol, inner_base)) continue;                         /* probe = outer only */
    int incol = icol->col_index - inner_base;                            /* inner column index */
    for (int xi = 0; xi < it->nindex; xi++) {
      tdb_index *ix = &it->indexes[xi];
      if (ix->kind == TDB_IDX_RTREE) continue;
      if (ix->ncol >= 1 && ix->col_idx[0] == incol) { *probe = ocol; return ix; }
    }
  }
  return NULL;
}

static tdb_op *op_new(op_kind k, tdb_db *db, tdb_stmt *st) {
  tdb_op *op = (tdb_op *)tdb_calloc(sizeof(*op));
  if (op) { op->kind = k; op->db = db; op->st = st; }
  return op;
}

/* Build a Scan -> [Filter] -> Project -> [Limit] tree for a streamable SELECT,
** or return NULL (caller falls back to the materializing executor). On success
** st->ncol / st->colnames are set and the scan is opened against `txn`. */
static tdb_op *build_stream_plan(tdb_db *db, tdb_stmt *st, tdb_select *q, tdb_txn *txn) {
  if (!select_streamable(db, st, q)) return NULL;

  /* resolve every clause against the combined multi-source context */
  qctx Q; memset(&Q, 0, sizeof(Q));
  for (tdb_src *s = q->from; s; s = s->next) {
    tdb_table *t = tdb_catalog_find_table(db->cat, s->table);
    qsrc *qs = &Q.src[Q.nsrc];
    qs->tbl = t; qs->alias = s->alias; qs->base = Q.totalcols; qs->ncol = t->ncol;
    qs->join = s->join; qs->on = s->on;
    Q.totalcols += t->ncol;
    Q.nsrc++;
  }
  tdb_table *t = Q.src[0].tbl;   /* the (left-most) driving table */

  /* ON conditions, WHERE, ORDER BY all resolve against the full row */
  for (int s = 0; s < Q.nsrc; s++)
    if (Q.src[s].on && resolve_expr(db, &Q, Q.src[s].on)) return NULL;
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
      /* '*' or 't.*' expands to the columns of the matching source(s), in the
      ** combined-row order */
      for (int s = 0; s < Q.nsrc; s++) {
        qsrc *qs = &Q.src[s];
        if (item->table) {
          const char *nm = qs->alias ? qs->alias : qs->tbl->name;
          if (strcasecmp(nm, item->table) != 0) continue;
        }
        for (int k = 0; k < qs->tbl->ncol; k++) {
          if (qs->tbl->cols[k].hidden) continue;
          tdb_expr *col = tdb_expr_new(st->arena, EX_COLUMN);
          col->name = (char *)qs->tbl->cols[k].name;
          col->col_index = qs->base + k;
          SADD_OUT(col, qs->tbl->cols[k].name);
        }
      }
    } else {
      if (resolve_expr(db, &Q, item)) SFAIL;
      const char *nm = alias ? alias : (item->kind == EX_COLUMN ? item->name : NULL);
      SADD_OUT(item, nm);
    }
  }

  /* Driving (left-most) scan. With a single source we push an index pick and
  ** projection mask into it; with a join the driving and inner scans are full
  ** scans (columns from several tables are needed, and pushdown across tables
  ** would be unsound here). */
  tdb_index *idx = NULL; tdb_keyrange kr; int have_idx = 0;
  tdb_value_init(&kr.lo); tdb_value_init(&kr.hi);
  if (Q.nsrc == 1 && t->nindex > 0 && pick_index(db, st, t, q->where, &idx, &kr)) have_idx = 1;

  tdb_op *scan = op_new(OP_SCAN, db, st);
  scan->tbl = t; scan->txn = txn;
  if (Q.nsrc == 1) scan->has_mask = compute_colmask(t, q, scan->colmask);

  tdb_scan *sc = NULL;
  int rc = db->engine->vtab->scan_open(db->engine, txn, t, have_idx ? idx : NULL,
                                       have_idx ? &kr : NULL, 0,
                                       scan->has_mask ? scan->colmask : NULL, &sc);
  tdb_value_clear(&kr.lo); tdb_value_clear(&kr.hi);
  if (rc) { op_close(scan); SFAIL; }
  scan->scan = sc;
  scan->scanrow = row_alloc(t->ncol);

  /* chain a nested-loop join for each remaining source */
  tdb_op *src = scan;
  int s_idx = 1;
  for (tdb_src *s = q->from->next; s; s = s->next, s_idx++) {
    qsrc *qs = &Q.src[s_idx];
    tdb_op *inner = op_new(OP_SCAN, db, st);
    inner->tbl = qs->tbl; inner->txn = txn;
    inner->scanrow = row_alloc(qs->tbl->ncol);
    /* inner scan is (re)opened on first pull / rewind; open it once now so an
    ** early teardown has something well-formed to close */
    rc = db->engine->vtab->scan_open(db->engine, txn, qs->tbl, NULL, NULL, 0, NULL, &inner->scan);
    if (rc) { op_close(inner); op_close(src); SFAIL; }

    tdb_op *join = op_new(OP_JOIN, db, st);
    join->child = src;
    join->inner = inner;
    join->on = (s->join == JOIN_INNER) ? s->on : NULL;   /* CROSS has no ON */
    join->inner_base = qs->base;
    join->joinncol = qs->base + qs->tbl->ncol;
    join->joinrow = row_alloc(join->joinncol);
    /* index-driven inner lookup when the ON is an equijoin on an indexed inner
    ** column (the ON is still re-checked per row, so this is a pure accelerator) */
    if (join->on) {
      tdb_expr *probe = NULL;
      tdb_index *jidx = find_join_index(db, s, qs, qs->base, join->on, &probe);
      if (jidx) { join->inner_idx = jidx; join->idx_join_key = probe; }
    }
    src = join;
  }

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
