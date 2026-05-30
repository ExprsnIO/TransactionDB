/* tdb_catalog.c — schema serialization, persistence, and in-memory cache. */
#include "tdb_catalog.h"
#include "../storage/tdb_btree.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"
#include "../common/tdb_buf.h"

#include <string.h>

#define CAT_TABLE   'T'
#define CAT_VIEW    'V'
#define CAT_ROUTINE 'R'

struct tdb_catalog {
  tdb_pager   *pager;
  tdb_pgno     root;
  tdb_table  **tables;  int ntable, captable;
  tdb_view   **views;   int nview, capview;
  tdb_routine**routines;int nroutine, caproutine;
};

/* ----------------------------- writer/reader -------------------------- */

static void w_u8(tdb_buf *b, uint8_t v) { tdb_buf_putc(b, v); }
static void w_u32(tdb_buf *b, uint32_t v) { uint8_t t[4]; tdb_put_u32(t, v); tdb_buf_append(b, t, 4); }
static void w_var(tdb_buf *b, uint64_t v) { tdb_buf_put_varint(b, v); }
static void w_str(tdb_buf *b, const char *s) {
  if (!s) { w_var(b, 0); return; }
  size_t n = strlen(s);
  w_var(b, n + 1);
  tdb_buf_append(b, s, n);
}

typedef struct { const uint8_t *p; int len; int pos; int err; } rd;
static uint64_t r_var(rd *r) {
  uint64_t v = 0;
  int a = tdb_get_varint(r->p + r->pos, (size_t)(r->len - r->pos), &v);
  if (a <= 0) { r->err = 1; return 0; }
  r->pos += a; return v;
}
static uint32_t r_u32(rd *r) {
  if (r->pos + 4 > r->len) { r->err = 1; return 0; }
  uint32_t v = tdb_get_u32(r->p + r->pos); r->pos += 4; return v;
}
static uint8_t r_u8(rd *r) {
  if (r->pos >= r->len) { r->err = 1; return 0; }
  return r->p[r->pos++];
}
static char *r_str(rd *r) {
  uint64_t L = r_var(r);
  if (L == 0) return NULL;
  int n = (int)(L - 1);
  if (r->pos + n > r->len) { r->err = 1; return NULL; }
  char *s = tdb_strndup((const char *)r->p + r->pos, (size_t)n);
  r->pos += n;
  return s;
}

/* ------------------------------ serialize ----------------------------- */

static void ser_table(const tdb_table *t, tdb_buf *b) {
  w_u8(b, CAT_TABLE);
  w_str(b, t->name);
  w_u32(b, t->root);
  w_u32(b, t->history_root);
  w_u32(b, t->schema_version);
  w_u8(b, (uint8_t)t->versioning);
  w_var(b, (uint64_t)(t->row_start_col + 1));
  w_var(b, (uint64_t)(t->row_end_col + 1));
  w_str(b, t->check_sql);
  w_str(b, t->before_insert_lua);
  w_str(b, t->before_update_lua);
  w_str(b, t->before_delete_lua);

  w_var(b, (uint64_t)t->ncol);
  for (int i = 0; i < t->ncol; i++) {
    const tdb_column *c = &t->cols[i];
    w_str(b, c->name);
    w_u8(b, (uint8_t)c->type.id);
    w_var(b, (uint64_t)c->type.length);
    w_var(b, (uint64_t)c->type.precision);
    w_var(b, (uint64_t)c->type.scale);
    w_u8(b, (uint8_t)c->coll);
    uint8_t flags = (uint8_t)((c->notnull ? 1 : 0) | (c->pk ? 2 : 0) |
                              (c->unique ? 4 : 0) | (c->hidden ? 8 : 0));
    w_u8(b, flags);
    w_u8(b, (uint8_t)c->generated);
    w_str(b, c->default_sql);
    w_str(b, c->check_sql);
    w_str(b, c->generated_sql);
    w_str(b, c->lua_compute);
  }

  w_var(b, (uint64_t)t->nindex);
  for (int i = 0; i < t->nindex; i++) {
    const tdb_index *ix = &t->indexes[i];
    w_str(b, ix->name);
    w_u32(b, ix->root);
    w_u8(b, (uint8_t)ix->unique);
    w_var(b, (uint64_t)ix->ncol);
    for (int j = 0; j < ix->ncol; j++) {
      w_var(b, (uint64_t)ix->col_idx[j]);
      w_u8(b, ix->desc ? ix->desc[j] : 0);
    }
  }
}

static tdb_table *deser_table(rd *r) {
  char *name = r_str(r);
  tdb_table *t = tdb_table_new(name ? name : "");
  tdb_mfree(name);
  if (!t) return NULL;
  t->root = r_u32(r);
  t->history_root = r_u32(r);
  t->schema_version = r_u32(r);
  t->versioning = (tdb_versioning)r_u8(r);
  t->row_start_col = (int)r_var(r) - 1;
  t->row_end_col = (int)r_var(r) - 1;
  t->check_sql = r_str(r);
  t->before_insert_lua = r_str(r);
  t->before_update_lua = r_str(r);
  t->before_delete_lua = r_str(r);

  int ncol = (int)r_var(r);
  for (int i = 0; i < ncol && !r->err; i++) {
    tdb_column c;
    char *cn = r_str(r);
    tdb_typespec ts; memset(&ts, 0, sizeof(ts));
    ts.id = (tdb_typeid)r_u8(r);
    ts.length = (int)r_var(r);
    ts.precision = (int)r_var(r);
    ts.scale = (int)r_var(r);
    tdb_column_init(&c, cn ? cn : "", ts);
    tdb_mfree(cn);
    c.coll = (tdb_collation)r_u8(r);
    uint8_t flags = r_u8(r);
    c.notnull = (flags & 1) ? 1 : 0;
    c.pk = (flags & 2) ? 1 : 0;
    c.unique = (flags & 4) ? 1 : 0;
    c.hidden = (flags & 8) ? 1 : 0;
    c.generated = (tdb_generated_kind)r_u8(r);
    c.default_sql = r_str(r);
    c.check_sql = r_str(r);
    c.generated_sql = r_str(r);
    c.lua_compute = r_str(r);
    tdb_table_add_column(t, &c);
  }

  int nindex = (int)r_var(r);
  for (int i = 0; i < nindex && !r->err; i++) {
    tdb_index ix; memset(&ix, 0, sizeof(ix));
    ix.name = r_str(r);
    ix.root = r_u32(r);
    ix.unique = r_u8(r);
    ix.ncol = (int)r_var(r);
    if (ix.ncol > 0) {
      ix.col_idx = (int *)tdb_malloc(sizeof(int) * (size_t)ix.ncol);
      ix.desc = (uint8_t *)tdb_malloc((size_t)ix.ncol);
      for (int j = 0; j < ix.ncol; j++) {
        ix.col_idx[j] = (int)r_var(r);
        ix.desc[j] = r_u8(r);
      }
    }
    tdb_index *grown = (tdb_index *)tdb_realloc(t->indexes,
        sizeof(tdb_index) * (size_t)(t->nindex + 1));
    if (grown) { t->indexes = grown; t->indexes[t->nindex++] = ix; }
  }
  return t;
}

static void ser_view(const tdb_view *v, tdb_buf *b) {
  w_u8(b, CAT_VIEW);
  w_str(b, v->name);
  w_str(b, v->select_sql);
  w_u8(b, (uint8_t)v->materialized);
  w_u32(b, v->root);
}
static void ser_routine(const tdb_routine *r, tdb_buf *b) {
  w_u8(b, CAT_ROUTINE);
  w_str(b, r->name);
  w_str(b, r->lua_src);
  w_u8(b, (uint8_t)r->is_function);
  w_var(b, (uint64_t)(r->nargs + 1));
}

/* ------------------------------ in-memory ----------------------------- */

static void cache_add_table(tdb_catalog *c, tdb_table *t) {
  if (c->ntable == c->captable) {
    int cap = c->captable ? c->captable * 2 : 8;
    c->tables = (tdb_table **)tdb_realloc(c->tables, sizeof(void *) * (size_t)cap);
    c->captable = cap;
  }
  c->tables[c->ntable++] = t;
}
static void cache_add_view(tdb_catalog *c, tdb_view *v) {
  if (c->nview == c->capview) {
    int cap = c->capview ? c->capview * 2 : 8;
    c->views = (tdb_view **)tdb_realloc(c->views, sizeof(void *) * (size_t)cap);
    c->capview = cap;
  }
  c->views[c->nview++] = v;
}
static void cache_add_routine(tdb_catalog *c, tdb_routine *r) {
  if (c->nroutine == c->caproutine) {
    int cap = c->caproutine ? c->caproutine * 2 : 8;
    c->routines = (tdb_routine **)tdb_realloc(c->routines, sizeof(void *) * (size_t)cap);
    c->caproutine = cap;
  }
  c->routines[c->nroutine++] = r;
}

/* ------------------------------- persist ------------------------------ */

static int catalog_put(tdb_catalog *c, const tdb_buf *blob) {
  tdb_btree *bt;
  int rc = tdb_btree_open(c->pager, c->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_rowid mx = 0;
  rc = tdb_btree_max_rowid(bt, &mx);
  if (!rc) rc = tdb_btree_put(bt, mx + 1, blob->data, (int)blob->len);
  tdb_btree_close(bt);
  return rc;
}

int tdb_catalog_add_table(tdb_catalog *c, tdb_table *t) {
  tdb_buf b; tdb_buf_init(&b);
  ser_table(t, &b);
  int rc = catalog_put(c, &b);
  tdb_buf_free(&b);
  if (rc) return rc;
  cache_add_table(c, t);
  return TDB_OK;
}
int tdb_catalog_add_view(tdb_catalog *c, tdb_view *v) {
  tdb_buf b; tdb_buf_init(&b);
  ser_view(v, &b);
  int rc = catalog_put(c, &b);
  tdb_buf_free(&b);
  if (rc) return rc;
  cache_add_view(c, v);
  return TDB_OK;
}
int tdb_catalog_add_routine(tdb_catalog *c, tdb_routine *r) {
  tdb_buf b; tdb_buf_init(&b);
  ser_routine(r, &b);
  int rc = catalog_put(c, &b);
  tdb_buf_free(&b);
  if (rc) return rc;
  cache_add_routine(c, r);
  return TDB_OK;
}

static int catalog_load(tdb_catalog *c) {
  tdb_btree *bt;
  int rc = tdb_btree_open(c->pager, c->root, TDB_BT_TABLE, NULL, &bt);
  if (rc) return rc;
  tdb_cursor *cur;
  rc = tdb_cursor_open(bt, &cur);
  if (rc) { tdb_btree_close(bt); return rc; }
  for (tdb_cursor_first(cur); !tdb_cursor_eof(cur); tdb_cursor_next(cur)) {
    const uint8_t *v; int n;
    if (tdb_cursor_data(cur, &v, &n)) break;
    rd r = { v, n, 0, 0 };
    uint8_t type = r_u8(&r);
    if (type == CAT_TABLE) {
      tdb_table *t = deser_table(&r);
      if (t && !r.err) cache_add_table(c, t);
      else tdb_table_free(t);
    } else if (type == CAT_VIEW) {
      tdb_view *vw = (tdb_view *)tdb_calloc(sizeof(*vw));
      vw->name = r_str(&r);
      vw->select_sql = r_str(&r);
      vw->materialized = r_u8(&r);
      vw->root = r_u32(&r);
      if (!r.err) cache_add_view(c, vw); else tdb_view_free(vw);
    } else if (type == CAT_ROUTINE) {
      tdb_routine *rt = (tdb_routine *)tdb_calloc(sizeof(*rt));
      rt->name = r_str(&r);
      rt->lua_src = r_str(&r);
      rt->is_function = r_u8(&r);
      rt->nargs = (int)r_var(&r) - 1;
      if (!r.err) cache_add_routine(c, rt); else tdb_routine_free(rt);
    }
  }
  tdb_cursor_close(cur);
  tdb_btree_close(bt);
  return TDB_OK;
}

int tdb_catalog_open(tdb_pager *p, tdb_catalog **out) {
  tdb_catalog *c = (tdb_catalog *)tdb_calloc(sizeof(*c));
  if (!c) return TDB_NOMEM;
  c->pager = p;
  c->root = tdb_pager_catalog_root(p);

  if (c->root == 0) {
    /* bootstrap: create the catalog b-tree and record its root */
    int rc = tdb_pager_begin(p);
    if (rc) { tdb_mfree(c); return rc; }
    rc = tdb_btree_create(p, TDB_BT_TABLE, &c->root);
    if (rc) { tdb_mfree(c); return rc; }
    tdb_pager_set_catalog_root(p, c->root);
    rc = tdb_pager_commit(p);
    if (rc) { tdb_mfree(c); return rc; }
  }

  int rc = catalog_load(c);
  if (rc) { tdb_catalog_close(c); return rc; }
  *out = c;
  return TDB_OK;
}

void tdb_catalog_close(tdb_catalog *c) {
  if (!c) return;
  for (int i = 0; i < c->ntable; i++) tdb_table_free(c->tables[i]);
  for (int i = 0; i < c->nview; i++) tdb_view_free(c->views[i]);
  for (int i = 0; i < c->nroutine; i++) tdb_routine_free(c->routines[i]);
  tdb_mfree(c->tables);
  tdb_mfree(c->views);
  tdb_mfree(c->routines);
  tdb_mfree(c);
}

tdb_table *tdb_catalog_find_table(tdb_catalog *c, const char *name) {
  for (int i = 0; i < c->ntable; i++)
    if (strcasecmp(c->tables[i]->name, name) == 0) return c->tables[i];
  return NULL;
}
tdb_view *tdb_catalog_find_view(tdb_catalog *c, const char *name) {
  for (int i = 0; i < c->nview; i++)
    if (strcasecmp(c->views[i]->name, name) == 0) return c->views[i];
  return NULL;
}
tdb_routine *tdb_catalog_find_routine(tdb_catalog *c, const char *name) {
  for (int i = 0; i < c->nroutine; i++)
    if (strcasecmp(c->routines[i]->name, name) == 0) return c->routines[i];
  return NULL;
}
int tdb_catalog_table_count(tdb_catalog *c) { return c->ntable; }
tdb_table *tdb_catalog_table_at(tdb_catalog *c, int i) { return c->tables[i]; }
int tdb_catalog_routine_count(tdb_catalog *c) { return c->nroutine; }
tdb_routine *tdb_catalog_routine_at(tdb_catalog *c, int i) { return c->routines[i]; }

void tdb_catalog_drop_table(tdb_catalog *c, const char *name) {
  for (int i = 0; i < c->ntable; i++) {
    if (strcasecmp(c->tables[i]->name, name) == 0) {
      tdb_table_free(c->tables[i]);
      for (int j = i; j < c->ntable - 1; j++) c->tables[j] = c->tables[j + 1];
      c->ntable--;
      return;
    }
  }
}
