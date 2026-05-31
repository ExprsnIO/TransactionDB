/* tdb_pager.c — page cache, free-list, and WAL-backed transactions. */
#include "tdb_pager.h"
#include "tdb_wal.h"
#include "tdb_format.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>
#include <stdlib.h>

#define TDB_CACHE_BUCKETS 1024
#define TDB_CACHE_SOFT_MAX 2048   /* opportunistically evict clean pages above */

/* Parsed copy of the database header (page 1, bytes [0,100)). */
typedef struct {
  uint32_t page_size;
  uint32_t change_counter;
  uint32_t db_size;
  uint32_t freelist_head;
  uint32_t freelist_count;
  uint32_t catalog_root;
  uint32_t schema_cookie;
  uint32_t text_encoding;
  uint64_t max_txnid;
  uint32_t reserved_per_page;
} tdb_dbhdr;

struct tdb_pager {
  tdb_file *file;
  tdb_file *walfile;
  tdb_wal  *wal;
  int       readonly;
  int       in_txn;
  uint32_t  page_size;
  tdb_dbhdr hdr;
  int       hdr_dirty;

  tdb_page *buckets[TDB_CACHE_BUCKETS];
  int       ncached;
  tdb_page *lru_head, *lru_tail; /* head = most recent */

  /* savepoint stack: each level snapshots the header + before-images of the
  ** pages that were dirty when the savepoint was opened */
  struct pager_sp {
    tdb_dbhdr hdr;
    struct { tdb_pgno pgno; uint8_t *data; } *imgs;
    int nimg, capimg;
  } *sps;
  int nsp, capsp;
};

/* ----------------------------- header codec --------------------------- */

static void hdr_parse(const uint8_t *b, tdb_dbhdr *h) {
  h->page_size       = tdb_get_u16(b + TDB_HDR_PAGE_SIZE);
  if (h->page_size == 0) h->page_size = 65536; /* 0 encodes 64KiB */
  h->change_counter  = tdb_get_u32(b + TDB_HDR_CHANGE_COUNTER);
  h->db_size         = tdb_get_u32(b + TDB_HDR_DB_SIZE);
  h->freelist_head   = tdb_get_u32(b + TDB_HDR_FREELIST_HEAD);
  h->freelist_count  = tdb_get_u32(b + TDB_HDR_FREELIST_COUNT);
  h->catalog_root    = tdb_get_u32(b + TDB_HDR_CATALOG_ROOT);
  h->schema_cookie   = tdb_get_u32(b + TDB_HDR_SCHEMA_COOKIE);
  h->text_encoding   = tdb_get_u32(b + TDB_HDR_TEXT_ENCODING);
  h->max_txnid       = tdb_get_u64(b + TDB_HDR_MAX_TXNID);
  h->reserved_per_page = tdb_get_u32(b + TDB_HDR_RESERVED_PER_PG);
}

static void hdr_serialize(const tdb_dbhdr *h, uint8_t *b) {
  memset(b, 0, TDB_HEADER_SIZE);
  memcpy(b + TDB_HDR_MAGIC, TDB_MAGIC, sizeof(TDB_MAGIC));
  uint16_t ps = (h->page_size >= 65536) ? 0 : (uint16_t)h->page_size;
  tdb_put_u16(b + TDB_HDR_PAGE_SIZE, ps);
  b[TDB_HDR_WRITE_VERSION] = 1;
  b[TDB_HDR_READ_VERSION] = 1;
  tdb_put_u32(b + TDB_HDR_FORMAT_VERSION, TDB_FORMAT_VERSION);
  tdb_put_u32(b + TDB_HDR_CHANGE_COUNTER, h->change_counter);
  tdb_put_u32(b + TDB_HDR_DB_SIZE, h->db_size);
  tdb_put_u32(b + TDB_HDR_FREELIST_HEAD, h->freelist_head);
  tdb_put_u32(b + TDB_HDR_FREELIST_COUNT, h->freelist_count);
  tdb_put_u32(b + TDB_HDR_CATALOG_ROOT, h->catalog_root);
  tdb_put_u32(b + TDB_HDR_SCHEMA_COOKIE, h->schema_cookie);
  tdb_put_u32(b + TDB_HDR_TEXT_ENCODING, h->text_encoding);
  tdb_put_u64(b + TDB_HDR_MAX_TXNID, h->max_txnid);
  tdb_put_u32(b + TDB_HDR_RESERVED_PER_PG, h->reserved_per_page);
  uint32_t crc = tdb_crc32c(0, b, TDB_HDR_CRC32C);
  tdb_put_u32(b + TDB_HDR_CRC32C, crc);
}

/* ------------------------------- cache -------------------------------- */

static unsigned bucket_of(tdb_pgno pgno) {
  return (unsigned)(pgno % TDB_CACHE_BUCKETS);
}

static void lru_unlink(tdb_pager *p, tdb_page *pg) {
  if (pg->lru_prev) pg->lru_prev->lru_next = pg->lru_next;
  else p->lru_head = pg->lru_next;
  if (pg->lru_next) pg->lru_next->lru_prev = pg->lru_prev;
  else p->lru_tail = pg->lru_prev;
  pg->lru_prev = pg->lru_next = NULL;
}

static void lru_push_front(tdb_pager *p, tdb_page *pg) {
  pg->lru_prev = NULL;
  pg->lru_next = p->lru_head;
  if (p->lru_head) p->lru_head->lru_prev = pg;
  p->lru_head = pg;
  if (!p->lru_tail) p->lru_tail = pg;
}

static tdb_page *cache_lookup(tdb_pager *p, tdb_pgno pgno) {
  for (tdb_page *pg = p->buckets[bucket_of(pgno)]; pg; pg = pg->hnext)
    if (pg->pgno == pgno) return pg;
  return NULL;
}

static void cache_remove(tdb_pager *p, tdb_page *pg) {
  tdb_page **pp = &p->buckets[bucket_of(pg->pgno)];
  while (*pp && *pp != pg) pp = &(*pp)->hnext;
  if (*pp) *pp = pg->hnext;
  lru_unlink(p, pg);
  p->ncached--;
  tdb_mfree(pg->data);
  tdb_mfree(pg);
}

/* Evict clean, unpinned pages from the LRU tail when over the soft cap. */
static void cache_trim(tdb_pager *p) {
  tdb_page *pg = p->lru_tail;
  while (p->ncached > TDB_CACHE_SOFT_MAX && pg) {
    tdb_page *prev = pg->lru_prev;
    if (pg->refs == 0 && !pg->dirty) cache_remove(p, pg);
    pg = prev;
  }
}

/* Read the committed image of a page into `buf`: WAL first, then file. */
static int read_page_image(tdb_pager *p, tdb_pgno pgno, uint8_t *buf) {
  int found = 0;
  int rc = tdb_wal_read_page(p->wal, pgno, buf, &found);
  if (rc) return rc;
  if (found) return TDB_OK;
  return tdb_file_read(p->file, buf, p->page_size,
                       (uint64_t)(pgno - 1) * p->page_size);
}

static tdb_page *page_new(tdb_pager *p, tdb_pgno pgno) {
  tdb_page *pg = (tdb_page *)tdb_calloc(sizeof(*pg));
  if (!pg) return NULL;
  pg->data = (uint8_t *)tdb_calloc(p->page_size);
  if (!pg->data) { tdb_mfree(pg); return NULL; }
  pg->pgno = pgno;
  pg->hnext = p->buckets[bucket_of(pgno)];
  p->buckets[bucket_of(pgno)] = pg;
  lru_push_front(p, pg);
  p->ncached++;
  return pg;
}

int tdb_pager_get(tdb_pager *p, tdb_pgno pgno, tdb_page **out) {
  if (pgno == 0) return TDB_MISUSE;
  tdb_page *pg = cache_lookup(p, pgno);
  if (pg) {
    lru_unlink(p, pg);
    lru_push_front(p, pg);
    pg->refs++;
    *out = pg;
    return TDB_OK;
  }
  pg = page_new(p, pgno);
  if (!pg) return TDB_NOMEM;
  int rc = read_page_image(p, pgno, pg->data);
  if (rc) { cache_remove(p, pg); return rc; }
  pg->refs = 1;
  *out = pg;
  cache_trim(p);
  return TDB_OK;
}

void tdb_pager_unref(tdb_pager *p, tdb_page *pg) {
  if (!pg) return;
  if (pg->refs > 0) pg->refs--;
  (void)p;
}

int tdb_pager_write(tdb_pager *p, tdb_page *pg) {
  if (p->readonly) return TDB_READONLY;
  pg->dirty = 1;
  return TDB_OK;
}

uint32_t tdb_pager_page_size(tdb_pager *p) { return p->page_size; }
tdb_pgno tdb_pager_db_size(tdb_pager *p) { return p->hdr.db_size; }
tdb_pgno tdb_pager_freelist_count(tdb_pager *p) { return p->hdr.freelist_count; }
int tdb_pager_is_readonly(tdb_pager *p) { return p->readonly; }

uint64_t tdb_pager_max_txnid(tdb_pager *p) { return p->hdr.max_txnid; }
void tdb_pager_set_max_txnid(tdb_pager *p, uint64_t v) {
  p->hdr.max_txnid = v; p->hdr_dirty = 1;
}
uint32_t tdb_pager_schema_cookie(tdb_pager *p) { return p->hdr.schema_cookie; }
void tdb_pager_set_schema_cookie(tdb_pager *p, uint32_t v) {
  p->hdr.schema_cookie = v; p->hdr_dirty = 1;
}
uint32_t tdb_pager_catalog_root(tdb_pager *p) { return p->hdr.catalog_root; }
void tdb_pager_set_catalog_root(tdb_pager *p, uint32_t pgno) {
  p->hdr.catalog_root = pgno; p->hdr_dirty = 1;
}

/* ----------------------------- free-list ------------------------------ */

int tdb_pager_alloc(tdb_pager *p, tdb_page **out) {
  if (p->readonly) return TDB_READONLY;
  tdb_pgno pgno = 0;

  if (p->hdr.freelist_head != 0) {
    tdb_page *trunk;
    int rc = tdb_pager_get(p, p->hdr.freelist_head, &trunk);
    if (rc) return rc;
    uint32_t leaf_count = tdb_get_u32(trunk->data + TDB_FL_LEAF_COUNT);
    if (leaf_count > 0) {
      uint32_t idx = leaf_count - 1;
      pgno = tdb_get_u32(trunk->data + TDB_FL_LEAVES + idx * 4u);
      tdb_put_u32(trunk->data + TDB_FL_LEAF_COUNT, leaf_count - 1);
      tdb_pager_write(p, trunk);
      tdb_pager_unref(p, trunk);
    } else {
      /* reuse the (now empty) trunk page itself */
      pgno = p->hdr.freelist_head;
      p->hdr.freelist_head = tdb_get_u32(trunk->data + TDB_FL_NEXT_TRUNK);
      tdb_pager_unref(p, trunk);
    }
    p->hdr.freelist_count--;
    p->hdr_dirty = 1;
  }

  if (pgno == 0) {
    pgno = ++p->hdr.db_size; /* grow */
    p->hdr_dirty = 1;
  }

  /* drop any stale cache entry, then create a fresh zeroed page */
  tdb_page *old = cache_lookup(p, pgno);
  if (old) {
    old->refs = 0;
    cache_remove(p, old);
  }
  tdb_page *pg = page_new(p, pgno);
  if (!pg) return TDB_NOMEM;
  pg->refs = 1;
  pg->dirty = 1;
  *out = pg;
  return TDB_OK;
}

int tdb_pager_free(tdb_pager *p, tdb_pgno pgno) {
  if (p->readonly) return TDB_READONLY;
  if (pgno == 0 || pgno == TDB_CATALOG_PGNO) return TDB_MISUSE;

  uint32_t per_trunk = (p->page_size - TDB_FL_LEAVES) / 4u;
  if (p->hdr.freelist_head != 0) {
    tdb_page *trunk;
    int rc = tdb_pager_get(p, p->hdr.freelist_head, &trunk);
    if (rc) return rc;
    uint32_t leaf_count = tdb_get_u32(trunk->data + TDB_FL_LEAF_COUNT);
    if (leaf_count < per_trunk) {
      tdb_put_u32(trunk->data + TDB_FL_LEAVES + leaf_count * 4u, pgno);
      tdb_put_u32(trunk->data + TDB_FL_LEAF_COUNT, leaf_count + 1);
      tdb_pager_write(p, trunk);
      tdb_pager_unref(p, trunk);
      p->hdr.freelist_count++;
      p->hdr_dirty = 1;
      return TDB_OK;
    }
    tdb_pager_unref(p, trunk);
  }

  /* make `pgno` a new trunk page */
  tdb_page *pg;
  int rc = tdb_pager_get(p, pgno, &pg);
  if (rc) return rc;
  memset(pg->data, 0, p->page_size);
  pg->data[TDB_FL_TYPE] = TDB_PAGE_FREELIST_TRUNK;
  tdb_put_u32(pg->data + TDB_FL_NEXT_TRUNK, p->hdr.freelist_head);
  tdb_put_u32(pg->data + TDB_FL_LEAF_COUNT, 0);
  tdb_pager_write(p, pg);
  tdb_pager_unref(p, pg);
  p->hdr.freelist_head = pgno;
  p->hdr.freelist_count++;
  p->hdr_dirty = 1;
  return TDB_OK;
}

/* --------------------------- transactions ----------------------------- */

int tdb_pager_begin(tdb_pager *p) {
  if (p->readonly) return TDB_READONLY;
  p->in_txn = 1;
  return TDB_OK;
}

static void sp_clear_all(tdb_pager *p);

/* Write the header into page 1's data buffer and mark it dirty. */
static int flush_header(tdb_pager *p) {
  tdb_page *pg;
  int rc = tdb_pager_get(p, TDB_CATALOG_PGNO, &pg);
  if (rc) return rc;
  p->hdr.change_counter++;
  hdr_serialize(&p->hdr, pg->data);
  pg->dirty = 1;
  tdb_pager_unref(p, pg);
  p->hdr_dirty = 0;
  return TDB_OK;
}

int tdb_pager_commit(tdb_pager *p) {
  if (p->readonly) return TDB_READONLY;

  int rc = flush_header(p);
  if (rc) return rc;

  /* collect dirty pages */
  tdb_page *dirty[TDB_CACHE_SOFT_MAX + 64];
  int k = 0;
  for (int b = 0; b < TDB_CACHE_BUCKETS; b++) {
    for (tdb_page *pg = p->buckets[b]; pg; pg = pg->hnext) {
      if (pg->dirty) {
        if (k < (int)(sizeof(dirty) / sizeof(dirty[0]))) dirty[k++] = pg;
      }
    }
  }

  for (int i = 0; i < k; i++) {
    int is_commit = (i == k - 1);
    rc = tdb_wal_append(p->wal, dirty[i]->pgno, dirty[i]->data, is_commit,
                        is_commit ? p->hdr.db_size : 0);
    if (rc) return rc;
  }
  if (k > 0) {
    rc = tdb_wal_sync(p->wal);
    if (rc) return rc;
    for (int i = 0; i < k; i++) dirty[i]->dirty = 0;
  }

  p->in_txn = 0;
  sp_clear_all(p);

  /* Auto-checkpoint when the log grows large to bound its size. */
  if (tdb_wal_frame_count(p->wal) > 1000) {
    tdb_pager_checkpoint(p);
  }
  return TDB_OK;
}

int tdb_pager_rollback(tdb_pager *p) {
  /* Evict all dirty pages so subsequent reads see committed data. */
  for (int b = 0; b < TDB_CACHE_BUCKETS; b++) {
    tdb_page *pg = p->buckets[b];
    while (pg) {
      tdb_page *next = pg->hnext;
      if (pg->dirty) { pg->refs = 0; cache_remove(p, pg); }
      pg = next;
    }
  }
  /* Reload header from durable storage. */
  uint8_t *full = (uint8_t *)tdb_malloc(p->page_size);
  if (full) {
    if (read_page_image(p, TDB_CATALOG_PGNO, full) == TDB_OK &&
        memcmp(full, TDB_MAGIC, sizeof(TDB_MAGIC)) == 0) {
      hdr_parse(full, &p->hdr);
    }
    tdb_mfree(full);
  }
  p->hdr_dirty = 0;
  p->in_txn = 0;
  sp_clear_all(p);
  return TDB_OK;
}

/* ----------------------------- savepoints ----------------------------- */

static void sp_free(struct pager_sp *sp) {
  for (int i = 0; i < sp->nimg; i++) tdb_mfree(sp->imgs[i].data);
  tdb_mfree(sp->imgs);
  sp->imgs = NULL; sp->nimg = sp->capimg = 0;
}
static void sp_clear_all(tdb_pager *p) {
  for (int i = 0; i < p->nsp; i++) sp_free(&p->sps[i]);
  p->nsp = 0;
}

int tdb_pager_savepoint(tdb_pager *p) {
  if (p->readonly) return -1;
  if (p->nsp == p->capsp) {
    int cap = p->capsp ? p->capsp * 2 : 4;
    p->sps = (struct pager_sp *)tdb_realloc(p->sps, sizeof(*p->sps) * (size_t)cap);
    p->capsp = cap;
  }
  struct pager_sp *sp = &p->sps[p->nsp];
  sp->hdr = p->hdr;
  sp->imgs = NULL; sp->nimg = sp->capimg = 0;
  /* snapshot before-images of all currently-dirty pages */
  for (int b = 0; b < TDB_CACHE_BUCKETS; b++) {
    for (tdb_page *pg = p->buckets[b]; pg; pg = pg->hnext) {
      if (!pg->dirty) continue;
      if (sp->nimg == sp->capimg) {
        int cap = sp->capimg ? sp->capimg * 2 : 16;
        sp->imgs = tdb_realloc(sp->imgs, sizeof(sp->imgs[0]) * (size_t)cap);
        sp->capimg = cap;
      }
      uint8_t *copy = (uint8_t *)tdb_malloc(p->page_size);
      memcpy(copy, pg->data, p->page_size);
      sp->imgs[sp->nimg].pgno = pg->pgno;
      sp->imgs[sp->nimg].data = copy;
      sp->nimg++;
    }
  }
  return p->nsp++;
}

/* RELEASE: discard savepoint `level` and any nested inside it (changes kept). */
int tdb_pager_savepoint_release(tdb_pager *p, int level) {
  if (level < 0 || level >= p->nsp) return TDB_MISUSE;
  for (int i = p->nsp - 1; i >= level; i--) sp_free(&p->sps[i]);
  p->nsp = level;
  return TDB_OK;
}

/* ROLLBACK TO: revert to savepoint `level` (kept); discard nested savepoints. */
int tdb_pager_savepoint_rollback(tdb_pager *p, int level) {
  if (level < 0 || level >= p->nsp) return TDB_MISUSE;
  for (int i = p->nsp - 1; i > level; i--) sp_free(&p->sps[i]);
  p->nsp = level + 1;
  struct pager_sp *sp = &p->sps[level];

  /* collect currently-dirty pages */
  tdb_page *dirty[TDB_CACHE_SOFT_MAX + 64]; int k = 0;
  for (int b = 0; b < TDB_CACHE_BUCKETS; b++)
    for (tdb_page *pg = p->buckets[b]; pg; pg = pg->hnext)
      if (pg->dirty && k < (int)(sizeof(dirty) / sizeof(dirty[0]))) dirty[k++] = pg;

  for (int i = 0; i < k; i++) {
    tdb_page *pg = dirty[i];
    int restored = 0;
    for (int j = 0; j < sp->nimg; j++) {
      if (sp->imgs[j].pgno == pg->pgno) {
        memcpy(pg->data, sp->imgs[j].data, p->page_size); /* dirty at savepoint */
        restored = 1; break;
      }
    }
    if (!restored) { pg->refs = 0; cache_remove(p, pg); } /* clean at savepoint */
  }
  p->hdr = sp->hdr;
  p->hdr_dirty = 1;
  return TDB_OK;
}

int tdb_pager_checkpoint(tdb_pager *p) {
  if (p->readonly) return TDB_OK;
  if (tdb_wal_frame_count(p->wal) == 0) return TDB_OK;
  return tdb_wal_checkpoint(p->wal, p->file, NULL);
}

/* ------------------------------ open/close ---------------------------- */

static char *wal_path(const char *path) {
  size_t n = strlen(path);
  char *w = (char *)tdb_malloc(n + 5);
  if (!w) return NULL;
  memcpy(w, path, n);
  memcpy(w + n, "-wal", 5);
  return w;
}

int tdb_pager_open(const tdb_vfs *vfs, const char *path, int flags,
                   tdb_pager **out) {
  int memory = (flags & TDB_OPEN_MEMORY) || !path || strcmp(path, ":memory:") == 0;
  if (memory) vfs = tdb_vfs_memory();
  else if (!vfs) vfs = tdb_vfs_default();

  tdb_pager *p = (tdb_pager *)tdb_calloc(sizeof(*p));
  if (!p) return TDB_NOMEM;
  p->readonly = (flags & TDB_OPEN_READONLY) ? 1 : 0;

  int rc = vfs->open(vfs, memory ? "" : path, flags, &p->file);
  if (rc) { tdb_mfree(p); return rc; }

  /* Determine page size: file header, else WAL header, else default. */
  uint8_t probe[TDB_HEADER_SIZE];
  uint64_t fsize = 0;
  tdb_file_size(p->file, &fsize);
  int have_hdr = 0;
  uint32_t page_size = TDB_DEFAULT_PAGE_SIZE;
  if (fsize >= TDB_HEADER_SIZE) {
    if (tdb_file_read(p->file, probe, sizeof(probe), 0) == TDB_OK &&
        memcmp(probe, TDB_MAGIC, sizeof(TDB_MAGIC)) == 0) {
      have_hdr = 1;
      hdr_parse(probe, &p->hdr);
      page_size = p->hdr.page_size;
    }
  }

  /* open WAL file */
  char *wp = memory ? NULL : wal_path(path);
  if (!memory) {
    rc = vfs->open(vfs, wp, flags | TDB_OPEN_CREATE, &p->walfile);
    tdb_mfree(wp);
    if (rc) { tdb_file_close(p->file); tdb_mfree(p); return rc; }
  } else {
    rc = vfs->open(vfs, "", TDB_OPEN_CREATE, &p->walfile);
    if (rc) { tdb_file_close(p->file); tdb_mfree(p); return rc; }
  }

  if (!have_hdr) {
    /* Peek at WAL header for page size (crash before first checkpoint). */
    uint8_t wh[TDB_WAL_HDR_SIZE];
    uint64_t wsize = 0;
    tdb_file_size(p->walfile, &wsize);
    if (wsize >= TDB_WAL_HDR_SIZE &&
        tdb_file_read(p->walfile, wh, sizeof(wh), 0) == TDB_OK &&
        tdb_get_u32(wh + TDB_WAL_HDR_MAGIC) == TDB_WAL_MAGIC) {
      page_size = tdb_get_u32(wh + TDB_WAL_HDR_PAGE_SIZE);
    }
  }
  p->page_size = page_size;

  rc = tdb_wal_open(p->walfile, p->page_size, &p->wal);
  if (rc) { tdb_file_close(p->walfile); tdb_file_close(p->file); tdb_mfree(p); return rc; }

  if (!have_hdr) {
    /* Maybe a header lives only in the WAL (recovered). */
    uint8_t *full = (uint8_t *)tdb_malloc(p->page_size);
    if (!full) { tdb_pager_close(p); return TDB_NOMEM; }
    int found = 0;
    if (tdb_wal_read_page(p->wal, TDB_CATALOG_PGNO, full, &found) == TDB_OK &&
        found && memcmp(full, TDB_MAGIC, sizeof(TDB_MAGIC)) == 0) {
      hdr_parse(full, &p->hdr);
      have_hdr = 1;
    }
    tdb_mfree(full);
  }

  if (!have_hdr) {
    /* brand-new database */
    memset(&p->hdr, 0, sizeof(p->hdr));
    p->hdr.page_size = p->page_size;
    p->hdr.db_size = 1;            /* page 1 = header only */
    p->hdr.catalog_root = 0;       /* catalog b-tree created on first open */
    p->hdr.schema_cookie = 0;
    p->hdr.text_encoding = TDB_TEXT_ENC_UTF8;
    p->hdr.max_txnid = 0;
    if (!p->readonly) {
      rc = tdb_pager_begin(p);
      if (!rc) rc = tdb_pager_commit(p); /* persist initial header */
      if (rc) { tdb_pager_close(p); return rc; }
    }
  } else if (tdb_wal_db_size(p->wal) != 0) {
    p->hdr.db_size = tdb_wal_db_size(p->wal);
  }

  *out = p;
  return TDB_OK;
}

int tdb_pager_close(tdb_pager *p) {
  if (!p) return TDB_OK;
  if (!p->readonly && p->wal) tdb_pager_checkpoint(p);
  for (int b = 0; b < TDB_CACHE_BUCKETS; b++) {
    tdb_page *pg = p->buckets[b];
    while (pg) { tdb_page *next = pg->hnext; tdb_mfree(pg->data); tdb_mfree(pg); pg = next; }
  }
  sp_clear_all(p);
  tdb_mfree(p->sps);
  if (p->wal) tdb_wal_close(p->wal);
  if (p->walfile) tdb_file_close(p->walfile);
  if (p->file) tdb_file_close(p->file);
  tdb_mfree(p);
  return TDB_OK;
}
