/* tdb_btree.c — B-tree + cursor over the pager.
**
** Key design notes:
**   - No b-tree page is page 1 (page 1 holds only the database header), so the
**     b-tree header always begins at byte 0 of a page. Roots are >= page 2.
**   - Cell pointers are absolute byte offsets within the page, kept sorted by
**     key. Cell content grows downward from the end of the page; deleted cells
**     leave holes reclaimed by an on-demand defragment.
**   - Interior cell key i is the maximum key in child i's subtree, so a search
**     descends the first child whose key >= the target (else the right child).
**   - Large leaf values spill into overflow page chains: [u32 next][bytes...].
*/
#include "tdb_btree.h"
#include "tdb_format.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>

#define TDB_MAXDEPTH 40

struct tdb_btree {
  tdb_pager     *p;
  tdb_pgno       root;
  tdb_btree_kind kind;
  int            has_ki;
  tdb_keyinfo    ki;
  uint32_t       ps;
};

struct tdb_cursor {
  tdb_btree *bt;
  tdb_pgno   page[TDB_MAXDEPTH];
  int        cidx[TDB_MAXDEPTH];
  int        depth;          /* leaf level index */
  tdb_page  *leaf;           /* pinned current leaf page (or NULL) */
  int        eof;
  uint8_t   *ovbuf;
  int        ovcap;
};

/* ------------------------------ comparison ---------------------------- */

static int bt_cmp(tdb_btree *bt, const uint8_t *a, int an,
                  const uint8_t *b, int bn) {
  if (bt->kind == TDB_BT_TABLE) {
    return memcmp(a, b, 8); /* 8-byte BE rowids: unsigned order */
  }
  if (bt->has_ki) return tdb_record_compare(a, (size_t)an, b, (size_t)bn, &bt->ki);
  int n = an < bn ? an : bn;
  int c = memcmp(a, b, (size_t)n);
  if (c) return c < 0 ? -1 : 1;
  return an == bn ? 0 : (an < bn ? -1 : 1);
}

static void rowid_to_key(tdb_rowid r, uint8_t out[8]) { tdb_put_u64(out, r); }
static tdb_rowid key_to_rowid(const uint8_t *k) { return tdb_get_u64(k); }

/* --------------------------- page accessors --------------------------- */

static int is_leaf_type(uint8_t t) {
  return t == TDB_PAGE_LEAF_TABLE || t == TDB_PAGE_LEAF_INDEX;
}
static uint8_t pg_type(tdb_page *pg) { return pg->data[TDB_BT_TYPE]; }
static int pg_is_leaf(tdb_page *pg) { return is_leaf_type(pg_type(pg)); }
static int pg_hdrsize(tdb_page *pg) {
  return pg_is_leaf(pg) ? TDB_BT_LEAF_HDR_SIZE : TDB_BT_INTERIOR_HDR_SIZE;
}
static int pg_ncell(tdb_page *pg) { return tdb_get_u16(pg->data + TDB_BT_NCELL); }
static void pg_set_ncell(tdb_page *pg, int n) {
  tdb_put_u16(pg->data + TDB_BT_NCELL, (uint16_t)n);
}
static uint32_t pg_content(tdb_btree *bt, tdb_page *pg) {
  uint16_t c = tdb_get_u16(pg->data + TDB_BT_CELL_CONTENT);
  return c ? c : bt->ps;
}
static void pg_set_content(tdb_btree *bt, tdb_page *pg, uint32_t c) {
  tdb_put_u16(pg->data + TDB_BT_CELL_CONTENT, (uint16_t)(c == bt->ps ? 0 : c));
}
static tdb_pgno pg_right(tdb_page *pg) {
  return tdb_get_u32(pg->data + TDB_BT_RIGHT_CHILD);
}
static void pg_set_right(tdb_page *pg, tdb_pgno c) {
  tdb_put_u32(pg->data + TDB_BT_RIGHT_CHILD, c);
}
static int cellptr_at(tdb_page *pg, int i) {
  return tdb_get_u16(pg->data + pg_hdrsize(pg) + i * 2);
}
static void cellptr_set(tdb_page *pg, int i, int off) {
  tdb_put_u16(pg->data + pg_hdrsize(pg) + i * 2, (uint16_t)off);
}

static void init_page(tdb_btree *bt, tdb_page *pg, uint8_t type) {
  memset(pg->data, 0, bt->ps);
  pg->data[TDB_BT_TYPE] = type;
  pg_set_ncell(pg, 0);
  pg_set_content(bt, pg, bt->ps);
}

/* ------------------------------- cells -------------------------------- */

typedef struct {
  const uint8_t *key; int klen;
  const uint8_t *lval; int local; int vlen;
  tdb_pgno ovfl;
} leafcell;

static int local_val_len(uint32_t ps, int klen, int vlen) {
  int maxpayload = ((int)ps - 12) / 4;
  int hdr = tdb_varint_len((uint64_t)klen) + klen + tdb_varint_len((uint64_t)vlen);
  if (hdr + vlen <= maxpayload) return vlen;
  int budget = maxpayload - hdr - 4;
  if (budget < 0) budget = 0;
  if (budget > vlen) budget = vlen;
  return budget;
}

static void read_leafcell(tdb_btree *bt, tdb_page *pg, int i, leafcell *lc) {
  const uint8_t *p = pg->data + cellptr_at(pg, i);
  uint64_t kl, vl;
  int a = tdb_get_varint(p, 10, &kl); p += a;
  lc->key = p; lc->klen = (int)kl; p += kl;
  a = tdb_get_varint(p, 10, &vl); p += a;
  lc->vlen = (int)vl;
  lc->local = local_val_len(bt->ps, (int)kl, (int)vl);
  lc->lval = p; p += lc->local;
  lc->ovfl = (lc->local < (int)vl) ? tdb_get_u32(p) : 0;
}

static tdb_pgno intcell_child(tdb_page *pg, int i) {
  return tdb_get_u32(pg->data + cellptr_at(pg, i));
}
static void set_intcell_child(tdb_page *pg, int i, tdb_pgno c) {
  tdb_put_u32(pg->data + cellptr_at(pg, i), c);
}
static void intcell_key(tdb_page *pg, int i, const uint8_t **k, int *kl) {
  const uint8_t *p = pg->data + cellptr_at(pg, i) + 4;
  uint64_t l; int a = tdb_get_varint(p, 10, &l);
  *k = p + a; *kl = (int)l;
}

static int leaf_cell_size(tdb_btree *bt, tdb_page *pg, int i) {
  leafcell lc; read_leafcell(bt, pg, i, &lc);
  int prefix = (int)(lc.lval - (pg->data + cellptr_at(pg, i)));
  return prefix + lc.local + (lc.ovfl ? 4 : 0);
}
static int int_cell_size(tdb_page *pg, int i) {
  const uint8_t *p = pg->data + cellptr_at(pg, i) + 4;
  uint64_t l; int a = tdb_get_varint(p, 10, &l);
  return 4 + a + (int)l;
}
static int cell_size(tdb_btree *bt, tdb_page *pg, int i) {
  return pg_is_leaf(pg) ? leaf_cell_size(bt, pg, i) : int_cell_size(pg, i);
}

/* key of a cell at index i (works for leaf and interior) */
static void cell_key(tdb_btree *bt, tdb_page *pg, int i, const uint8_t **k, int *kl) {
  if (pg_is_leaf(pg)) { leafcell lc; read_leafcell(bt, pg, i, &lc); *k = lc.key; *kl = lc.klen; }
  else intcell_key(pg, i, k, kl);
}

/* ----------------------------- overflow ------------------------------- */

static int overflow_write(tdb_btree *bt, const uint8_t *data, int len,
                          tdb_pgno *first_out) {
  uint32_t per = bt->ps - 4;
  tdb_pgno first = 0, prev = 0;
  int off = 0;
  while (off < len) {
    tdb_page *pg;
    int rc = tdb_pager_alloc(bt->p, &pg);
    if (rc) return rc;
    int chunk = (int)per < (len - off) ? (int)per : (len - off);
    tdb_put_u32(pg->data, 0);
    memcpy(pg->data + 4, data + off, (size_t)chunk);
    tdb_pager_write(bt->p, pg);
    tdb_pgno cur = pg->pgno;
    tdb_pager_unref(bt->p, pg);
    if (!first) first = cur;
    if (prev) {
      tdb_page *pp;
      rc = tdb_pager_get(bt->p, prev, &pp);
      if (rc) return rc;
      tdb_put_u32(pp->data, cur);
      tdb_pager_write(bt->p, pp);
      tdb_pager_unref(bt->p, pp);
    }
    prev = cur;
    off += chunk;
  }
  *first_out = first;
  return TDB_OK;
}

static int overflow_read(tdb_btree *bt, tdb_pgno first, uint8_t *dst, int len) {
  uint32_t per = bt->ps - 4;
  tdb_pgno pgno = first;
  int got = 0;
  while (got < len && pgno) {
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, pgno, &pg);
    if (rc) return rc;
    int chunk = (int)per < (len - got) ? (int)per : (len - got);
    memcpy(dst + got, pg->data + 4, (size_t)chunk);
    pgno = tdb_get_u32(pg->data);
    tdb_pager_unref(bt->p, pg);
    got += chunk;
  }
  return TDB_OK;
}

static void overflow_free(tdb_btree *bt, tdb_pgno first) {
  tdb_pgno pgno = first;
  while (pgno) {
    tdb_page *pg;
    if (tdb_pager_get(bt->p, pgno, &pg)) return;
    tdb_pgno next = tdb_get_u32(pg->data);
    tdb_pager_unref(bt->p, pg);
    tdb_pager_free(bt->p, pgno);
    pgno = next;
  }
}

/* --------------------------- cell builders ---------------------------- */

static int build_leaf_cell(tdb_btree *bt, const uint8_t *key, int klen,
                           const uint8_t *val, int vlen,
                           uint8_t **out, int *outsz) {
  int local = local_val_len(bt->ps, klen, vlen);
  int need = tdb_varint_len((uint64_t)klen) + klen +
             tdb_varint_len((uint64_t)vlen) + local + (local < vlen ? 4 : 0);
  uint8_t *buf = (uint8_t *)tdb_malloc((size_t)need);
  if (!buf) return TDB_NOMEM;
  uint8_t *p = buf;
  p += tdb_put_varint(p, (uint64_t)klen);
  memcpy(p, key, (size_t)klen); p += klen;
  p += tdb_put_varint(p, (uint64_t)vlen);
  if (local) memcpy(p, val, (size_t)local);
  p += local;
  if (local < vlen) {
    tdb_pgno first;
    int rc = overflow_write(bt, val + local, vlen - local, &first);
    if (rc) { tdb_mfree(buf); return rc; }
    tdb_put_u32(p, first);
  }
  *out = buf; *outsz = need;
  return TDB_OK;
}

static int build_int_cell(tdb_pgno child, const uint8_t *key, int klen,
                          uint8_t **out, int *outsz) {
  int need = 4 + tdb_varint_len((uint64_t)klen) + klen;
  uint8_t *buf = (uint8_t *)tdb_malloc((size_t)need);
  if (!buf) return TDB_NOMEM;
  tdb_put_u32(buf, child);
  uint8_t *p = buf + 4;
  p += tdb_put_varint(p, (uint64_t)klen);
  memcpy(p, key, (size_t)klen);
  *out = buf; *outsz = need;
  return TDB_OK;
}

/* --------------------------- page space mgmt -------------------------- */

static int free_contig(tdb_btree *bt, tdb_page *pg) {
  int top = pg_hdrsize(pg) + pg_ncell(pg) * 2;
  return (int)pg_content(bt, pg) - top;
}

static void pg_defrag(tdb_btree *bt, tdb_page *pg) {
  int n = pg_ncell(pg);
  uint8_t *scratch = (uint8_t *)tdb_malloc(bt->ps);
  if (!scratch) return;
  int newoff[2048];
  int use_heap = n > 2048;
  int *offs = use_heap ? (int *)tdb_malloc(sizeof(int) * (size_t)n) : newoff;
  int write = (int)bt->ps;
  for (int i = 0; i < n; i++) {
    int sz = cell_size(bt, pg, i);
    int src = cellptr_at(pg, i);
    write -= sz;
    memcpy(scratch + write, pg->data + src, (size_t)sz);
    offs[i] = write;
  }
  memcpy(pg->data + write, scratch + write, (size_t)((int)bt->ps - write));
  for (int i = 0; i < n; i++) cellptr_set(pg, i, offs[i]);
  pg_set_content(bt, pg, (uint32_t)write);
  if (use_heap) tdb_mfree(offs);
  tdb_mfree(scratch);
}

/* Insert pre-built cell bytes at position `index`. Returns TDB_FULL if there
** is not enough room even after defragmenting. */
static int insert_cell(tdb_btree *bt, tdb_page *pg, int index,
                       const uint8_t *cell, int csize) {
  if (free_contig(bt, pg) < csize + 2) {
    pg_defrag(bt, pg);
    if (free_contig(bt, pg) < csize + 2) return TDB_FULL;
  }
  int off = (int)pg_content(bt, pg) - csize;
  memcpy(pg->data + off, cell, (size_t)csize);
  pg_set_content(bt, pg, (uint32_t)off);
  int n = pg_ncell(pg);
  for (int j = n; j > index; j--) cellptr_set(pg, j, cellptr_at(pg, j - 1));
  cellptr_set(pg, index, off);
  pg_set_ncell(pg, n + 1);
  return TDB_OK;
}

static void delete_cell(tdb_page *pg, int index) {
  int n = pg_ncell(pg);
  for (int j = index; j < n - 1; j++) cellptr_set(pg, j, cellptr_at(pg, j + 1));
  pg_set_ncell(pg, n - 1);
}

/* first index whose cell key >= target; *exact set on equality */
static int pg_find(tdb_btree *bt, tdb_page *pg, const uint8_t *key, int klen,
                   int *exact) {
  int lo = 0, hi = pg_ncell(pg);
  *exact = 0;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    const uint8_t *k; int kl;
    cell_key(bt, pg, mid, &k, &kl);
    int c = bt_cmp(bt, k, kl, key, klen);
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  if (lo < pg_ncell(pg)) {
    const uint8_t *k; int kl;
    cell_key(bt, pg, lo, &k, &kl);
    if (bt_cmp(bt, k, kl, key, klen) == 0) *exact = 1;
  }
  return lo;
}

/* ------------------------------- splits ------------------------------- */

typedef struct { int split; uint8_t *sep; int seplen; tdb_pgno right; } splitinfo;

static int split_leaf(tdb_btree *bt, tdb_page *pg, int ins,
                      const uint8_t *cell, int csize, splitinfo *si) {
  int n = pg_ncell(pg);
  int total = n + 1;
  uint8_t *scratch = (uint8_t *)tdb_malloc((size_t)bt->ps * 2);
  int *off = (int *)tdb_malloc(sizeof(int) * (size_t)total);
  int *sz = (int *)tdb_malloc(sizeof(int) * (size_t)total);
  if (!scratch || !off || !sz) { tdb_mfree(scratch); tdb_mfree(off); tdb_mfree(sz); return TDB_NOMEM; }

  int w = 0;
  for (int li = 0; li < total; li++) {
    const uint8_t *cb; int cs;
    if (li < ins)      { cb = pg->data + cellptr_at(pg, li);     cs = cell_size(bt, pg, li); }
    else if (li == ins){ cb = cell;                              cs = csize; }
    else               { cb = pg->data + cellptr_at(pg, li - 1); cs = cell_size(bt, pg, li - 1); }
    memcpy(scratch + w, cb, (size_t)cs);
    off[li] = w; sz[li] = cs; w += cs;
  }

  int left = total / 2;
  if (left < 1) left = 1;
  if (left >= total) left = total - 1;

  uint8_t type = pg_type(pg);
  tdb_page *rp;
  int rc = tdb_pager_alloc(bt->p, &rp);
  if (rc) goto fail;
  init_page(bt, rp, type);
  init_page(bt, pg, type);

  for (int i = 0; i < left; i++)
    insert_cell(bt, pg, i, scratch + off[i], sz[i]);
  for (int i = left; i < total; i++)
    insert_cell(bt, rp, i - left, scratch + off[i], sz[i]);

  /* separator = max key of left page = key of cell (left-1) */
  const uint8_t *sk; uint64_t skl;
  { const uint8_t *c = scratch + off[left - 1];
    int a = tdb_get_varint(c, 10, &skl); sk = c + a; }
  si->sep = (uint8_t *)tdb_malloc((size_t)skl);
  if (!si->sep) { rc = TDB_NOMEM; goto fail; }
  memcpy(si->sep, sk, (size_t)skl);
  si->seplen = (int)skl;
  si->split = 1;
  si->right = rp->pgno;

  tdb_pager_write(bt->p, pg);
  tdb_pager_write(bt->p, rp);
  tdb_pager_unref(bt->p, rp);
  rc = TDB_OK;
fail:
  tdb_mfree(scratch); tdb_mfree(off); tdb_mfree(sz);
  return rc;
}

static int split_interior(tdb_btree *bt, tdb_page *pg, int ins,
                          const uint8_t *newcell, tdb_pgno new_right,
                          splitinfo *si) {
  int n = pg_ncell(pg);
  int nk = n + 1;        /* keys after insertion */
  int nc = nk + 1;       /* children after insertion */

  uint8_t *scratch = (uint8_t *)tdb_malloc((size_t)bt->ps * 2);
  int *koff = (int *)tdb_malloc(sizeof(int) * (size_t)nk);
  int *klen = (int *)tdb_malloc(sizeof(int) * (size_t)nk);
  tdb_pgno *child = (tdb_pgno *)tdb_malloc(sizeof(tdb_pgno) * (size_t)nc);
  if (!scratch || !koff || !klen || !child) {
    tdb_mfree(scratch); tdb_mfree(koff); tdb_mfree(klen); tdb_mfree(child);
    return TDB_NOMEM;
  }

  /* extract separator + left child from the new interior cell */
  tdb_pgno new_left = tdb_get_u32(newcell);
  const uint8_t *nsk; uint64_t nskl;
  { const uint8_t *p = newcell + 4; int a = tdb_get_varint(p, 10, &nskl); nsk = p + a; }

  int w = 0;
  /* build key array with sep inserted at `ins` */
  for (int i = 0; i < nk; i++) {
    const uint8_t *k; int kl;
    if (i < ins)      intcell_key(pg, i, &k, &kl);
    else if (i == ins){ k = nsk; kl = (int)nskl; }
    else              intcell_key(pg, i - 1, &k, &kl);
    memcpy(scratch + w, k, (size_t)kl);
    koff[i] = w; klen[i] = kl; w += kl;
  }
  /* build child array with new_left at ins and new_right at ins+1 */
  for (int i = 0; i < nc; i++) {
    tdb_pgno c;
    if (i < ins)        c = (i < n) ? intcell_child(pg, i) : pg_right(pg);
    else if (i == ins)  c = new_left;
    else if (i == ins + 1) c = new_right;
    else { int oi = i - 1; c = (oi < n) ? intcell_child(pg, oi) : pg_right(pg); }
    child[i] = c;
  }

  int mid = nk / 2;
  uint8_t type = pg_type(pg);

  tdb_page *rp;
  int rc = tdb_pager_alloc(bt->p, &rp);
  if (rc) goto fail;
  init_page(bt, rp, type);
  init_page(bt, pg, type);

  for (int j = 0; j < mid; j++) {
    uint8_t *ic; int isz;
    build_int_cell(child[j], scratch + koff[j], klen[j], &ic, &isz);
    insert_cell(bt, pg, j, ic, isz);
    tdb_mfree(ic);
  }
  pg_set_right(pg, child[mid]);

  int rj = 0;
  for (int j = mid + 1; j < nk; j++) {
    uint8_t *ic; int isz;
    build_int_cell(child[j], scratch + koff[j], klen[j], &ic, &isz);
    insert_cell(bt, rp, rj++, ic, isz);
    tdb_mfree(ic);
  }
  pg_set_right(rp, child[nc - 1]);

  si->seplen = klen[mid];
  si->sep = (uint8_t *)tdb_malloc((size_t)klen[mid]);
  if (!si->sep) { rc = TDB_NOMEM; goto fail; }
  memcpy(si->sep, scratch + koff[mid], (size_t)klen[mid]);
  si->split = 1;
  si->right = rp->pgno;

  tdb_pager_write(bt->p, pg);
  tdb_pager_write(bt->p, rp);
  tdb_pager_unref(bt->p, rp);
  rc = TDB_OK;
fail:
  tdb_mfree(scratch); tdb_mfree(koff); tdb_mfree(klen); tdb_mfree(child);
  return rc;
}

/* ----------------------------- recursive insert ----------------------- */

static int insert_rec(tdb_btree *bt, tdb_pgno pgno, const uint8_t *key, int klen,
                      const uint8_t *cell, int csize, splitinfo *si) {
  tdb_page *pg;
  int rc = tdb_pager_get(bt->p, pgno, &pg);
  if (rc) return rc;

  if (pg_is_leaf(pg)) {
    int exact, idx = pg_find(bt, pg, key, klen, &exact);
    if (exact) {
      leafcell lc; read_leafcell(bt, pg, idx, &lc);
      if (lc.ovfl) overflow_free(bt, lc.ovfl);
      delete_cell(pg, idx);
    }
    rc = insert_cell(bt, pg, idx, cell, csize);
    if (rc == TDB_FULL) rc = split_leaf(bt, pg, idx, cell, csize, si);
    if (rc == TDB_OK) tdb_pager_write(bt->p, pg);
    tdb_pager_unref(bt->p, pg);
    return rc;
  }

  int exact, ci = pg_find(bt, pg, key, klen, &exact);
  int n = pg_ncell(pg);
  tdb_pgno childpg = (ci < n) ? intcell_child(pg, ci) : pg_right(pg);

  splitinfo csi; memset(&csi, 0, sizeof(csi));
  rc = insert_rec(bt, childpg, key, klen, cell, csize, &csi);
  if (rc) { tdb_pager_unref(bt->p, pg); return rc; }

  if (csi.split) {
    uint8_t *icell; int isz;
    rc = build_int_cell(childpg, csi.sep, csi.seplen, &icell, &isz);
    if (rc) { tdb_mfree(csi.sep); tdb_pager_unref(bt->p, pg); return rc; }
    int oldn = pg_ncell(pg);
    rc = insert_cell(bt, pg, ci, icell, isz);
    if (rc == TDB_FULL) {
      rc = split_interior(bt, pg, ci, icell, csi.right, si);
    } else if (rc == TDB_OK) {
      if (ci < oldn) set_intcell_child(pg, ci + 1, csi.right);
      else pg_set_right(pg, csi.right);
    }
    if (rc == TDB_OK) tdb_pager_write(bt->p, pg);
    tdb_mfree(icell);
    tdb_mfree(csi.sep);
  }
  tdb_pager_unref(bt->p, pg);
  return rc;
}

static int grow_root(tdb_btree *bt, uint8_t *sep, int seplen, tdb_pgno right) {
  tdb_page *r;
  int rc = tdb_pager_get(bt->p, bt->root, &r);
  if (rc) return rc;
  tdb_page *nl;
  rc = tdb_pager_alloc(bt->p, &nl);
  if (rc) { tdb_pager_unref(bt->p, r); return rc; }
  memcpy(nl->data, r->data, bt->ps);   /* move old root content down */
  tdb_pager_write(bt->p, nl);
  tdb_pgno nlpg = nl->pgno;
  tdb_pager_unref(bt->p, nl);

  uint8_t itype = (bt->kind == TDB_BT_TABLE) ? TDB_PAGE_INTERIOR_TABLE
                                             : TDB_PAGE_INTERIOR_INDEX;
  init_page(bt, r, itype);
  uint8_t *ic; int isz;
  rc = build_int_cell(nlpg, sep, seplen, &ic, &isz);
  if (rc) { tdb_pager_unref(bt->p, r); return rc; }
  insert_cell(bt, r, 0, ic, isz);
  pg_set_right(r, right);
  tdb_mfree(ic);
  tdb_pager_write(bt->p, r);
  tdb_pager_unref(bt->p, r);
  return TDB_OK;
}

int tdb_btree_insert(tdb_btree *bt, const uint8_t *key, int klen,
                     const uint8_t *val, int vlen) {
  if (klen + 16 > (int)bt->ps / 2) return TDB_FULL; /* key too large */
  uint8_t *cell; int csize;
  int rc = build_leaf_cell(bt, key, klen, val, vlen, &cell, &csize);
  if (rc) return rc;
  splitinfo si; memset(&si, 0, sizeof(si));
  rc = insert_rec(bt, bt->root, key, klen, cell, csize, &si);
  tdb_mfree(cell);
  if (rc == TDB_OK && si.split) {
    rc = grow_root(bt, si.sep, si.seplen, si.right);
    tdb_mfree(si.sep);
  }
  return rc;
}

int tdb_btree_remove(tdb_btree *bt, const uint8_t *key, int klen, int *found) {
  if (found) *found = 0;
  tdb_pgno pgno = bt->root;
  for (;;) {
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, pgno, &pg);
    if (rc) return rc;
    int exact, idx = pg_find(bt, pg, key, klen, &exact);
    if (pg_is_leaf(pg)) {
      if (exact) {
        leafcell lc; read_leafcell(bt, pg, idx, &lc);
        if (lc.ovfl) overflow_free(bt, lc.ovfl);
        delete_cell(pg, idx);
        tdb_pager_write(bt->p, pg);
        if (found) *found = 1;
      }
      tdb_pager_unref(bt->p, pg);
      return TDB_OK;
    }
    int n = pg_ncell(pg);
    tdb_pgno child = (idx < n) ? intcell_child(pg, idx) : pg_right(pg);
    tdb_pager_unref(bt->p, pg);
    pgno = child;
  }
}

/* ------------------------------ open/close ---------------------------- */

int tdb_btree_create(tdb_pager *p, tdb_btree_kind kind, tdb_pgno *root_out) {
  tdb_page *pg;
  int rc = tdb_pager_alloc(p, &pg);
  if (rc) return rc;
  uint8_t type = (kind == TDB_BT_TABLE) ? TDB_PAGE_LEAF_TABLE : TDB_PAGE_LEAF_INDEX;
  uint32_t ps = tdb_pager_page_size(p);
  memset(pg->data, 0, ps);
  pg->data[TDB_BT_TYPE] = type;
  tdb_put_u16(pg->data + TDB_BT_NCELL, 0);
  tdb_put_u16(pg->data + TDB_BT_CELL_CONTENT, 0); /* 0 == page_size (empty) */
  tdb_pager_write(p, pg);
  *root_out = pg->pgno;
  tdb_pager_unref(p, pg);
  return TDB_OK;
}

int tdb_btree_open(tdb_pager *p, tdb_pgno root, tdb_btree_kind kind,
                   const tdb_keyinfo *ki, tdb_btree **out) {
  tdb_btree *bt = (tdb_btree *)tdb_calloc(sizeof(*bt));
  if (!bt) return TDB_NOMEM;
  bt->p = p;
  bt->root = root;
  bt->kind = kind;
  bt->ps = tdb_pager_page_size(p);
  if (kind == TDB_BT_INDEX && ki) { bt->has_ki = 1; bt->ki = *ki; }
  *out = bt;
  return TDB_OK;
}

void tdb_btree_close(tdb_btree *bt) { tdb_mfree(bt); }
tdb_pgno tdb_btree_root(tdb_btree *bt) { return bt->root; }

/* Recursively free a subtree: leaf overflow chains + interior children, then
** the page itself. Children pointers are read into a local array before the
** page is released so the recursion holds at most one page pinned per level. */
static int free_subtree(tdb_btree *bt, tdb_pgno pgno) {
  tdb_page *pg;
  int rc = tdb_pager_get(bt->p, pgno, &pg);
  if (rc) return rc;
  int n = pg_ncell(pg);
  if (pg_is_leaf(pg)) {
    for (int i = 0; i < n; i++) {
      leafcell lc; read_leafcell(bt, pg, i, &lc);
      if (lc.ovfl) overflow_free(bt, lc.ovfl);
    }
    tdb_pager_unref(bt->p, pg);
    return tdb_pager_free(bt->p, pgno);
  }
  tdb_pgno *kids = (tdb_pgno *)tdb_malloc(sizeof(tdb_pgno) * (size_t)(n + 1));
  if (!kids) { tdb_pager_unref(bt->p, pg); return TDB_NOMEM; }
  for (int i = 0; i < n; i++) kids[i] = intcell_child(pg, i);
  kids[n] = pg_right(pg);
  tdb_pager_unref(bt->p, pg);
  for (int i = 0; i <= n && !rc; i++)
    if (kids[i]) rc = free_subtree(bt, kids[i]);
  tdb_mfree(kids);
  if (!rc) rc = tdb_pager_free(bt->p, pgno);
  return rc;
}

int tdb_btree_destroy(tdb_pager *p, tdb_pgno root, tdb_btree_kind kind) {
  if (root == 0) return TDB_OK;
  tdb_btree *bt; int rc = tdb_btree_open(p, root, kind, NULL, &bt);
  if (rc) return rc;
  rc = free_subtree(bt, root);
  tdb_btree_close(bt);
  return rc;
}

/* ----------------------------- table API ------------------------------ */

int tdb_btree_put(tdb_btree *bt, tdb_rowid rowid, const void *val, int n) {
  uint8_t key[8]; rowid_to_key(rowid, key);
  return tdb_btree_insert(bt, key, 8, (const uint8_t *)val, n);
}
int tdb_btree_del(tdb_btree *bt, tdb_rowid rowid, int *found) {
  uint8_t key[8]; rowid_to_key(rowid, key);
  return tdb_btree_remove(bt, key, 8, found);
}
int tdb_index_put(tdb_btree *bt, const void *key, int klen) {
  return tdb_btree_insert(bt, (const uint8_t *)key, klen, NULL, 0);
}
int tdb_index_del(tdb_btree *bt, const void *key, int klen, int *found) {
  return tdb_btree_remove(bt, (const uint8_t *)key, klen, found);
}

/* -------------------------------- cursor ------------------------------ */

int tdb_cursor_open(tdb_btree *bt, tdb_cursor **out) {
  tdb_cursor *c = (tdb_cursor *)tdb_calloc(sizeof(*c));
  if (!c) return TDB_NOMEM;
  c->bt = bt;
  c->eof = 1;
  *out = c;
  return TDB_OK;
}

void tdb_cursor_close(tdb_cursor *c) {
  if (!c) return;
  if (c->leaf) tdb_pager_unref(c->bt->p, c->leaf);
  tdb_mfree(c->ovbuf);
  tdb_mfree(c);
}

static int descend_leftmost(tdb_cursor *c, tdb_pgno pgno, int level) {
  tdb_btree *bt = c->bt;
  for (;;) {
    c->page[level] = pgno;
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, pgno, &pg);
    if (rc) return rc;
    if (pg_is_leaf(pg)) {
      c->depth = level;
      c->cidx[level] = 0;
      c->leaf = pg;          /* keep pinned */
      c->eof = 0;
      return TDB_OK;
    }
    c->cidx[level] = 0;
    int n = pg_ncell(pg);
    tdb_pgno child = (n > 0) ? intcell_child(pg, 0) : pg_right(pg);
    tdb_pager_unref(bt->p, pg);
    pgno = child;
    level++;
  }
}

/* Advance to the next leaf (used when the current leaf is exhausted). */
static int next_leaf(tdb_cursor *c) {
  tdb_btree *bt = c->bt;
  if (c->leaf) { tdb_pager_unref(bt->p, c->leaf); c->leaf = NULL; }
  for (int level = c->depth - 1; level >= 0; level--) {
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, c->page[level], &pg);
    if (rc) return rc;
    int n = pg_ncell(pg);
    int nextci = c->cidx[level] + 1;
    if (nextci <= n) {
      c->cidx[level] = nextci;
      tdb_pgno child = (nextci < n) ? intcell_child(pg, nextci) : pg_right(pg);
      tdb_pager_unref(bt->p, pg);
      return descend_leftmost(c, child, level + 1);
    }
    tdb_pager_unref(bt->p, pg);
  }
  c->eof = 1;
  return TDB_OK;
}

int tdb_cursor_first(tdb_cursor *c) {
  if (c->leaf) { tdb_pager_unref(c->bt->p, c->leaf); c->leaf = NULL; }
  int rc = descend_leftmost(c, c->bt->root, 0);
  if (rc) return rc;
  while (!c->eof && pg_ncell(c->leaf) == 0) {
    rc = next_leaf(c);
    if (rc) return rc;
  }
  if (!c->eof && c->cidx[c->depth] >= pg_ncell(c->leaf)) c->eof = 1;
  return TDB_OK;
}

int tdb_cursor_next(tdb_cursor *c) {
  if (c->eof || !c->leaf) return TDB_OK;
  c->cidx[c->depth]++;
  if (c->cidx[c->depth] < pg_ncell(c->leaf)) return TDB_OK;
  int rc = next_leaf(c);
  if (rc) return rc;
  while (!c->eof && pg_ncell(c->leaf) == 0) {
    rc = next_leaf(c);
    if (rc) return rc;
  }
  return TDB_OK;
}

int tdb_cursor_eof(tdb_cursor *c) { return c->eof; }

int tdb_cursor_seek(tdb_cursor *c, const void *key, int klen, int *cmp) {
  tdb_btree *bt = c->bt;
  if (c->leaf) { tdb_pager_unref(bt->p, c->leaf); c->leaf = NULL; }
  tdb_pgno pgno = bt->root;
  int level = 0;
  for (;;) {
    c->page[level] = pgno;
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, pgno, &pg);
    if (rc) return rc;
    int exact, idx = pg_find(bt, pg, (const uint8_t *)key, klen, &exact);
    if (pg_is_leaf(pg)) {
      c->depth = level;
      c->cidx[level] = idx;
      c->leaf = pg;
      c->eof = 0;
      if (idx >= pg_ncell(pg)) {
        rc = next_leaf(c);
        if (rc) return rc;
        while (!c->eof && pg_ncell(c->leaf) == 0) { rc = next_leaf(c); if (rc) return rc; }
        if (cmp) *cmp = c->eof ? 1 : 1; /* landed key > target */
      } else if (cmp) {
        *cmp = exact ? 0 : 1;
      }
      return TDB_OK;
    }
    c->cidx[level] = idx;
    int n = pg_ncell(pg);
    tdb_pgno child = (idx < n) ? intcell_child(pg, idx) : pg_right(pg);
    tdb_pager_unref(bt->p, pg);
    pgno = child;
    level++;
  }
}

int tdb_cursor_seek_rowid(tdb_cursor *c, tdb_rowid rowid, int *found) {
  uint8_t key[8]; rowid_to_key(rowid, key);
  int cmp = 1;
  int rc = tdb_cursor_seek(c, key, 8, &cmp);
  if (found) *found = (!c->eof && cmp == 0) ? 1 : 0;
  return rc;
}

int tdb_cursor_key(tdb_cursor *c, const uint8_t **key, int *klen) {
  if (c->eof || !c->leaf) return TDB_NOTFOUND;
  leafcell lc; read_leafcell(c->bt, c->leaf, c->cidx[c->depth], &lc);
  *key = lc.key; *klen = lc.klen;
  return TDB_OK;
}

int tdb_cursor_rowid(tdb_cursor *c, tdb_rowid *rowid) {
  const uint8_t *k; int kl;
  int rc = tdb_cursor_key(c, &k, &kl);
  if (rc) return rc;
  *rowid = key_to_rowid(k);
  return TDB_OK;
}

int tdb_cursor_data(tdb_cursor *c, const uint8_t **val, int *vlen) {
  if (c->eof || !c->leaf) return TDB_NOTFOUND;
  leafcell lc; read_leafcell(c->bt, c->leaf, c->cidx[c->depth], &lc);
  if (lc.ovfl == 0) {
    *val = lc.lval; *vlen = lc.vlen;
    return TDB_OK;
  }
  if (c->ovcap < lc.vlen) {
    uint8_t *nb = (uint8_t *)tdb_realloc(c->ovbuf, (size_t)lc.vlen);
    if (!nb) return TDB_NOMEM;
    c->ovbuf = nb; c->ovcap = lc.vlen;
  }
  memcpy(c->ovbuf, lc.lval, (size_t)lc.local);
  int rc = overflow_read(c->bt, lc.ovfl, c->ovbuf + lc.local, lc.vlen - lc.local);
  if (rc) return rc;
  *val = c->ovbuf; *vlen = lc.vlen;
  return TDB_OK;
}

static int descend_rightmost(tdb_cursor *c, tdb_pgno pgno, int level) {
  tdb_btree *bt = c->bt;
  for (;;) {
    c->page[level] = pgno;
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, pgno, &pg);
    if (rc) return rc;
    int n = pg_ncell(pg);
    if (pg_is_leaf(pg)) {
      c->depth = level;
      c->cidx[level] = (n > 0) ? n - 1 : 0;
      c->leaf = pg;          /* keep pinned */
      c->eof = (n == 0);
      return TDB_OK;
    }
    c->cidx[level] = n; /* right pointer */
    tdb_pgno child = pg_right(pg);
    tdb_pager_unref(bt->p, pg);
    pgno = child;
    level++;
  }
}

int tdb_cursor_last(tdb_cursor *c) {
  if (c->leaf) { tdb_pager_unref(c->bt->p, c->leaf); c->leaf = NULL; }
  return descend_rightmost(c, c->bt->root, 0);
}

/* Step back to the predecessor leaf (used when the current leaf is exhausted
** to the left). Mirror of next_leaf: walk up the ancestor stack until we find
** an internal page whose previous child branch hasn't been visited, then
** descend rightmost into it. */
static int prev_leaf(tdb_cursor *c) {
  tdb_btree *bt = c->bt;
  if (c->leaf) { tdb_pager_unref(bt->p, c->leaf); c->leaf = NULL; }
  for (int level = c->depth - 1; level >= 0; level--) {
    tdb_page *pg;
    int rc = tdb_pager_get(bt->p, c->page[level], &pg);
    if (rc) return rc;
    int prevci = c->cidx[level] - 1;
    if (prevci >= 0) {
      int n = pg_ncell(pg);
      c->cidx[level] = prevci;
      tdb_pgno child = (prevci < n) ? intcell_child(pg, prevci) : pg_right(pg);
      tdb_pager_unref(bt->p, pg);
      return descend_rightmost(c, child, level + 1);
    }
    tdb_pager_unref(bt->p, pg);
  }
  c->eof = 1;
  return TDB_OK;
}

int tdb_cursor_prev(tdb_cursor *c) {
  if (c->eof || !c->leaf) return TDB_OK;
  if (c->cidx[c->depth] > 0) { c->cidx[c->depth]--; return TDB_OK; }
  int rc = prev_leaf(c);
  if (rc) return rc;
  while (!c->eof && pg_ncell(c->leaf) == 0) {
    rc = prev_leaf(c);
    if (rc) return rc;
  }
  return TDB_OK;
}

int tdb_btree_max_rowid(tdb_btree *bt, tdb_rowid *out) {
  tdb_cursor *c;
  int rc = tdb_cursor_open(bt, &c);
  if (rc) return rc;
  rc = tdb_cursor_last(c);
  if (rc) { tdb_cursor_close(c); return rc; }
  if (c->eof) *out = 0;
  else rc = tdb_cursor_rowid(c, out);
  tdb_cursor_close(c);
  return rc;
}

int tdb_btree_get(tdb_btree *bt, tdb_rowid rowid, const uint8_t **val, int *n,
                  tdb_cursor *cur) {
  int found = 0;
  int rc = tdb_cursor_seek_rowid(cur, rowid, &found);
  if (rc) return rc;
  if (!found) { *val = NULL; *n = 0; return TDB_NOTFOUND; }
  return tdb_cursor_data(cur, val, n);
}
