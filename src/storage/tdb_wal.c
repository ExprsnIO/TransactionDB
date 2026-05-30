/* tdb_wal.c — write-ahead log implementation. */
#include "tdb_wal.h"
#include "tdb_format.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>

struct tdb_wal {
  tdb_file *file;
  uint32_t  page_size;
  uint32_t  frame_stride;   /* frame header + page */
  uint32_t  salt1, salt2;
  uint32_t  ckpt_seq;

  uint64_t  nframe;         /* committed frames in the log */
  uint32_t  db_size;        /* db size recorded by last commit frame */

  /* pgno -> (frame_index + 1); 0 means "not in WAL" */
  uint64_t *map;
  tdb_pgno  map_cap;
};

static uint64_t frame_offset(tdb_wal *w, uint64_t idx) {
  return (uint64_t)TDB_WAL_HDR_SIZE + idx * w->frame_stride;
}

/* Rolling + standalone checksum over the salted frame-prefix and page image. */
static void frame_checksum(uint32_t seed, const uint8_t *hdr16,
                           const uint8_t *page, uint32_t page_size,
                           uint32_t *c1, uint32_t *c2) {
  uint32_t r = tdb_crc32c(seed, hdr16, 16);
  r = tdb_crc32c(r, page, page_size);
  *c1 = r;
  uint32_t s = tdb_crc32c(0, hdr16, 16);
  s = tdb_crc32c(s, page, page_size);
  *c2 = s;
}

static int map_ensure(tdb_wal *w, tdb_pgno pgno) {
  if (pgno < w->map_cap) return TDB_OK;
  tdb_pgno cap = w->map_cap ? w->map_cap : 64;
  while (cap <= pgno) cap *= 2;
  uint64_t *m = (uint64_t *)tdb_realloc(w->map, sizeof(uint64_t) * cap);
  if (!m) return TDB_NOMEM;
  memset(m + w->map_cap, 0, sizeof(uint64_t) * (cap - w->map_cap));
  w->map = m;
  w->map_cap = cap;
  return TDB_OK;
}

static int write_header(tdb_wal *w) {
  uint8_t hdr[TDB_WAL_HDR_SIZE];
  memset(hdr, 0, sizeof(hdr));
  tdb_put_u32(hdr + TDB_WAL_HDR_MAGIC, TDB_WAL_MAGIC);
  tdb_put_u32(hdr + TDB_WAL_HDR_VERSION, TDB_FORMAT_VERSION);
  tdb_put_u32(hdr + TDB_WAL_HDR_PAGE_SIZE, w->page_size);
  tdb_put_u32(hdr + TDB_WAL_HDR_CKPT_SEQ, w->ckpt_seq);
  tdb_put_u32(hdr + TDB_WAL_HDR_SALT1, w->salt1);
  tdb_put_u32(hdr + TDB_WAL_HDR_SALT2, w->salt2);
  uint32_t c1, c2;
  c1 = tdb_crc32c(w->salt1, hdr, 24);
  c2 = tdb_crc32c(0, hdr, 24);
  tdb_put_u32(hdr + TDB_WAL_HDR_CRC1, c1);
  tdb_put_u32(hdr + TDB_WAL_HDR_CRC2, c2);
  return tdb_file_write(w->file, hdr, sizeof(hdr), 0);
}

/* Replay existing frames, validating checksums; rebuild the page map and stop
** at the last valid commit frame. */
static int recover(tdb_wal *w) {
  uint64_t fsize = 0;
  int rc = tdb_file_size(w->file, &fsize);
  if (rc) return rc;
  if (fsize < TDB_WAL_HDR_SIZE) return TDB_OK; /* fresh/empty WAL */

  uint8_t hdr[TDB_WAL_HDR_SIZE];
  rc = tdb_file_read(w->file, hdr, sizeof(hdr), 0);
  if (rc) return rc;
  if (tdb_get_u32(hdr + TDB_WAL_HDR_MAGIC) != TDB_WAL_MAGIC) return TDB_OK;
  uint32_t ps = tdb_get_u32(hdr + TDB_WAL_HDR_PAGE_SIZE);
  if (ps != w->page_size) return TDB_OK; /* mismatched/stale WAL: ignore */
  w->salt1 = tdb_get_u32(hdr + TDB_WAL_HDR_SALT1);
  w->salt2 = tdb_get_u32(hdr + TDB_WAL_HDR_SALT2);
  w->ckpt_seq = tdb_get_u32(hdr + TDB_WAL_HDR_CKPT_SEQ);

  uint8_t *page = (uint8_t *)tdb_malloc(w->page_size);
  uint8_t fhdr[TDB_WAL_FRAME_HDR_SIZE];
  if (!page) return TDB_NOMEM;

  uint64_t idx = 0;
  uint64_t committed = 0;       /* number of frames up to & incl last commit */
  uint32_t committed_dbsize = 0;
  uint32_t rolling = w->salt1;

  /* Track per-pgno frame, but only promote the map to `committed` boundary. */
  while (frame_offset(w, idx) + w->frame_stride <= fsize) {
    uint64_t off = frame_offset(w, idx);
    if (tdb_file_read(w->file, fhdr, sizeof(fhdr), off)) break;
    if (tdb_file_read(w->file, page, w->page_size, off + TDB_WAL_FRAME_HDR_SIZE))
      break;
    uint32_t salt1 = tdb_get_u32(fhdr + TDB_WAL_FR_SALT1);
    uint32_t salt2 = tdb_get_u32(fhdr + TDB_WAL_FR_SALT2);
    if (salt1 != w->salt1 || salt2 != w->salt2) break; /* end of valid log */
    uint32_t c1, c2;
    frame_checksum(rolling, fhdr, page, w->page_size, &c1, &c2);
    if (c1 != tdb_get_u32(fhdr + TDB_WAL_FR_CRC1) ||
        c2 != tdb_get_u32(fhdr + TDB_WAL_FR_CRC2)) {
      break; /* torn frame */
    }
    rolling = c1;
    uint32_t dbsz = tdb_get_u32(fhdr + TDB_WAL_FR_DBSIZE);
    idx++;
    if (dbsz != 0) { /* commit frame */
      committed = idx;
      committed_dbsize = dbsz;
    }
  }
  tdb_mfree(page);

  /* Rebuild the page map from committed frames only (latest wins), so that a
  ** dropped trailing uncommitted frame does not hide an earlier committed
  ** version of the same page. */
  if (w->map) memset(w->map, 0, sizeof(uint64_t) * w->map_cap);
  for (uint64_t i = 0; i < committed; i++) {
    if (tdb_file_read(w->file, fhdr, sizeof(fhdr), frame_offset(w, i))) break;
    tdb_pgno pgno = tdb_get_u32(fhdr + TDB_WAL_FR_PGNO);
    if (map_ensure(w, pgno)) return TDB_NOMEM;
    w->map[pgno] = i + 1;
  }
  w->nframe = committed;
  w->db_size = committed_dbsize;
  return TDB_OK;
}

int tdb_wal_open(tdb_file *wal_file, uint32_t page_size, tdb_wal **out) {
  tdb_wal *w = (tdb_wal *)tdb_calloc(sizeof(*w));
  if (!w) return TDB_NOMEM;
  w->file = wal_file;
  w->page_size = page_size;
  w->frame_stride = TDB_WAL_FRAME_HDR_SIZE + page_size;
  /* Derive initial salts; recovery overrides them if a log already exists. */
  w->salt1 = (uint32_t)tdb_fnv1a(&page_size, sizeof(page_size)) ^ 0x9E3779B9u;
  w->salt2 = 0x1234567u;
  w->ckpt_seq = 0;
  w->nframe = 0;
  w->db_size = 0;

  int rc = recover(w);
  if (rc) { tdb_wal_close(w); return rc; }

  if (w->nframe == 0) {
    rc = write_header(w);
    if (rc) { tdb_wal_close(w); return rc; }
  }
  *out = w;
  return TDB_OK;
}

void tdb_wal_close(tdb_wal *w) {
  if (!w) return;
  tdb_mfree(w->map);
  tdb_mfree(w);
}

int tdb_wal_read_page(tdb_wal *w, tdb_pgno pgno, uint8_t *out, int *found) {
  *found = 0;
  if (pgno >= w->map_cap || w->map[pgno] == 0) return TDB_OK;
  uint64_t idx = w->map[pgno] - 1;
  if (idx >= w->nframe) return TDB_OK; /* uncommitted frame */
  uint64_t off = frame_offset(w, idx) + TDB_WAL_FRAME_HDR_SIZE;
  int rc = tdb_file_read(w->file, out, w->page_size, off);
  if (rc) return rc;
  *found = 1;
  return TDB_OK;
}

int tdb_wal_append(tdb_wal *w, tdb_pgno pgno, const uint8_t *page,
                   int is_commit, uint32_t db_size) {
  uint64_t idx = w->nframe;  /* append at the committed high-water mark */
  uint8_t fhdr[TDB_WAL_FRAME_HDR_SIZE];
  tdb_put_u32(fhdr + TDB_WAL_FR_PGNO, pgno);
  tdb_put_u32(fhdr + TDB_WAL_FR_DBSIZE, is_commit ? db_size : 0);
  tdb_put_u32(fhdr + TDB_WAL_FR_SALT1, w->salt1);
  tdb_put_u32(fhdr + TDB_WAL_FR_SALT2, w->salt2);

  uint32_t seed = (idx == 0) ? w->salt1 : 0;
  /* For a correct rolling chain we need the previous frame's c1. Recompute the
  ** seed by reading the previous frame's stored checksum. */
  if (idx > 0) {
    uint8_t prev[TDB_WAL_FRAME_HDR_SIZE];
    int rc = tdb_file_read(w->file, prev, sizeof(prev), frame_offset(w, idx - 1));
    if (rc) return rc;
    seed = tdb_get_u32(prev + TDB_WAL_FR_CRC1);
  }
  uint32_t c1, c2;
  frame_checksum(seed, fhdr, page, w->page_size, &c1, &c2);
  tdb_put_u32(fhdr + TDB_WAL_FR_CRC1, c1);
  tdb_put_u32(fhdr + TDB_WAL_FR_CRC2, c2);

  uint64_t off = frame_offset(w, idx);
  int rc = tdb_file_write(w->file, fhdr, sizeof(fhdr), off);
  if (rc) return rc;
  rc = tdb_file_write(w->file, page, w->page_size, off + TDB_WAL_FRAME_HDR_SIZE);
  if (rc) return rc;

  if (map_ensure(w, pgno)) return TDB_NOMEM;
  w->map[pgno] = idx + 1;
  w->nframe = idx + 1;
  if (is_commit) w->db_size = db_size;
  return TDB_OK;
}

int tdb_wal_sync(tdb_wal *w) { return tdb_file_sync(w->file); }

uint32_t tdb_wal_db_size(tdb_wal *w) { return w->db_size; }
uint64_t tdb_wal_frame_count(tdb_wal *w) { return w->nframe; }

int tdb_wal_checkpoint(tdb_wal *w, tdb_file *db, tdb_pgno *new_db_size) {
  uint8_t *page = (uint8_t *)tdb_malloc(w->page_size);
  if (!page) return TDB_NOMEM;
  int rc = TDB_OK;

  /* Write the latest committed image of each page back into the db file. */
  for (tdb_pgno p = 0; p < w->map_cap; p++) {
    uint64_t slot = w->map[p];
    if (slot == 0 || slot > w->nframe) continue;
    uint64_t off = frame_offset(w, slot - 1) + TDB_WAL_FRAME_HDR_SIZE;
    rc = tdb_file_read(w->file, page, w->page_size, off);
    if (rc) goto done;
    rc = tdb_file_write(db, page, w->page_size, (uint64_t)(p - 1) * w->page_size);
    if (rc) goto done;
  }
  rc = tdb_file_sync(db);
  if (rc) goto done;

  if (new_db_size) *new_db_size = w->db_size;

  /* Reset the log: new salts so stale frames fail recovery, truncate, rewrite
  ** the header. */
  w->ckpt_seq++;
  w->salt1 += 0x9E3779B9u;
  w->salt2 ^= 0xA5A5A5A5u;
  w->nframe = 0;
  if (w->map) memset(w->map, 0, sizeof(uint64_t) * w->map_cap);
  rc = tdb_file_truncate(w->file, 0);
  if (rc) goto done;
  rc = write_header(w);
  if (rc) goto done;
  rc = tdb_file_sync(w->file);

done:
  tdb_mfree(page);
  return rc;
}
