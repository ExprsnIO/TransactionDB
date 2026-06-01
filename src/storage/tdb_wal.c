/* tdb_wal.c — write-ahead log implementation. */
#include "tdb_wal.h"
#include "tdb_format.h"
#include "tdb_compress.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_util.h"

#include <string.h>

struct tdb_wal {
  tdb_file *file;
  uint32_t  page_size;
  uint32_t  frame_stride;   /* v1: fixed (header + page). v2: only the v1 stride
                            ** used by readers as a recovery hint upper bound. */
  uint32_t  version;        /* TDB_WAL_VERSION_V1 or V2 */
  uint32_t  salt1, salt2;
  uint32_t  ckpt_seq;

  uint64_t  nframe;         /* committed frames in the log */
  uint32_t  db_size;        /* db size recorded by last commit frame */

  /* pgno -> (frame_index + 1); 0 means "not in WAL" */
  uint64_t *map;
  tdb_pgno  map_cap;

  /* v2: byte-offset of each frame's header within the WAL file. v1 leaves
  ** this NULL — frame_offset() falls back to the fixed stride. */
  uint64_t *offsets;        /* size = nframe (room for nframe+1 sentinel) */
  uint64_t  offsets_cap;
  uint64_t  end_off;        /* byte offset for the next frame to be appended */
};

static int offsets_ensure(tdb_wal *w, uint64_t want) {
  if (want <= w->offsets_cap) return TDB_OK;
  uint64_t cap = w->offsets_cap ? w->offsets_cap : 64;
  while (cap < want) cap *= 2;
  uint64_t *o = (uint64_t *)tdb_realloc(w->offsets, sizeof(uint64_t) * cap);
  if (!o) return TDB_NOMEM;
  w->offsets = o;
  w->offsets_cap = cap;
  return TDB_OK;
}

static uint64_t frame_offset(tdb_wal *w, uint64_t idx) {
  if (w->version == TDB_WAL_VERSION_V2 && w->offsets) return w->offsets[idx];
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
  tdb_put_u32(hdr + TDB_WAL_HDR_VERSION, w->version);
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
** at the last valid commit frame. Supports both v1 (fixed-stride) and v2
** (variable physical size + codec id prefixed payload) layouts. */
static int recover(tdb_wal *w) {
  uint64_t fsize = 0;
  int rc = tdb_file_size(w->file, &fsize);
  if (rc) return rc;
  if (fsize < TDB_WAL_HDR_SIZE) return TDB_OK; /* fresh/empty WAL */

  uint8_t hdr[TDB_WAL_HDR_SIZE];
  rc = tdb_file_read(w->file, hdr, sizeof(hdr), 0);
  if (rc) return rc;
  if (tdb_get_u32(hdr + TDB_WAL_HDR_MAGIC) != TDB_WAL_MAGIC) return TDB_OK;
  uint32_t ver = tdb_get_u32(hdr + TDB_WAL_HDR_VERSION);
  if (ver != TDB_WAL_VERSION_V1 && ver != TDB_WAL_VERSION_V2) return TDB_OK;
  uint32_t ps = tdb_get_u32(hdr + TDB_WAL_HDR_PAGE_SIZE);
  if (ps != w->page_size) return TDB_OK; /* mismatched/stale WAL: ignore */
  w->version = ver;
  w->salt1 = tdb_get_u32(hdr + TDB_WAL_HDR_SALT1);
  w->salt2 = tdb_get_u32(hdr + TDB_WAL_HDR_SALT2);
  w->ckpt_seq = tdb_get_u32(hdr + TDB_WAL_HDR_CKPT_SEQ);

  uint8_t *page = (uint8_t *)tdb_malloc(w->page_size);
  uint8_t *scratch = (uint8_t *)tdb_malloc(w->page_size + 64);
  uint8_t fhdr[TDB_WAL_FRAME_HDR_SIZE];
  if (!page || !scratch) { tdb_mfree(page); tdb_mfree(scratch); return TDB_NOMEM; }

  uint64_t idx = 0;
  uint64_t off = TDB_WAL_HDR_SIZE;
  uint64_t committed = 0;       /* number of frames up to & incl last commit */
  uint32_t committed_dbsize = 0;
  uint64_t committed_end = TDB_WAL_HDR_SIZE;
  uint32_t rolling = w->salt1;

  while (off + TDB_WAL_FRAME_HDR_SIZE <= fsize) {
    if (tdb_file_read(w->file, fhdr, sizeof(fhdr), off)) break;
    uint32_t phys = w->page_size;
    uint8_t  codec = TDB_COMP_NONE;
    uint64_t payload_off = off + TDB_WAL_FRAME_HDR_SIZE;
    if (ver == TDB_WAL_VERSION_V2) {
      uint8_t pfx[TDB_WAL_V2_PAYLOAD_HDR];
      if (off + TDB_WAL_FRAME_HDR_SIZE + sizeof(pfx) > fsize) break;
      if (tdb_file_read(w->file, pfx, sizeof(pfx), payload_off)) break;
      phys  = tdb_get_u32(pfx);
      codec = pfx[4];
      payload_off += sizeof(pfx);
      if (phys > w->page_size || payload_off + phys > fsize) break;
    } else if (off + TDB_WAL_FRAME_HDR_SIZE + w->page_size > fsize) {
      break;
    }
    if (tdb_file_read(w->file, scratch, phys, payload_off)) break;
    /* materialize the logical page bytes (for checksum + map_read). */
    if (ver == TDB_WAL_VERSION_V2 && codec != TDB_COMP_NONE) {
      size_t out_n = w->page_size;
      if (tdb_decompress(scratch, phys, page, &out_n) != TDB_OK ||
          out_n != w->page_size) break;
    } else {
      memcpy(page, scratch, phys);
      if (phys < w->page_size) memset(page + phys, 0, w->page_size - phys);
    }

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
    /* record this frame's start offset for v2 index */
    if (ver == TDB_WAL_VERSION_V2) {
      if (offsets_ensure(w, idx + 1)) { rc = TDB_NOMEM; break; }
      w->offsets[idx] = off;
    }
    idx++;
    off = payload_off + phys;
    if (dbsz != 0) { /* commit frame */
      committed = idx;
      committed_dbsize = dbsz;
      committed_end = off;
    }
  }
  tdb_mfree(page); tdb_mfree(scratch);
  if (rc) return rc;

  /* Rebuild the page map from committed frames only (latest wins). */
  if (w->map) memset(w->map, 0, sizeof(uint64_t) * w->map_cap);
  for (uint64_t i = 0; i < committed; i++) {
    if (tdb_file_read(w->file, fhdr, sizeof(fhdr), frame_offset(w, i))) break;
    tdb_pgno pgno = tdb_get_u32(fhdr + TDB_WAL_FR_PGNO);
    if (map_ensure(w, pgno)) return TDB_NOMEM;
    w->map[pgno] = i + 1;
  }
  w->nframe = committed;
  w->db_size = committed_dbsize;
  w->end_off = committed_end;
  return TDB_OK;
}

int tdb_wal_open(tdb_file *wal_file, uint32_t page_size, tdb_wal **out) {
  tdb_wal *w = (tdb_wal *)tdb_calloc(sizeof(*w));
  if (!w) return TDB_NOMEM;
  w->file = wal_file;
  w->page_size = page_size;
  w->frame_stride = TDB_WAL_FRAME_HDR_SIZE + page_size;
  /* Default to v2 in builds where Zstandard is linked in, v1 otherwise — v1
  ** writes the smallest frame on disk when no compression is available. */
  w->version = (tdb_compress_codec() != TDB_COMP_NONE)
                  ? TDB_WAL_VERSION_V2 : TDB_WAL_VERSION_V1;
  /* Derive initial salts; recovery overrides them if a log already exists. */
  w->salt1 = (uint32_t)tdb_fnv1a(&page_size, sizeof(page_size)) ^ 0x9E3779B9u;
  w->salt2 = 0x1234567u;
  w->ckpt_seq = 0;
  w->nframe = 0;
  w->db_size = 0;
  w->end_off = TDB_WAL_HDR_SIZE;

  int rc = recover(w);
  if (rc) { tdb_wal_close(w); return rc; }

  if (w->nframe == 0) {
    rc = write_header(w);
    if (rc) { tdb_wal_close(w); return rc; }
    w->end_off = TDB_WAL_HDR_SIZE;
  }
  *out = w;
  return TDB_OK;
}

void tdb_wal_close(tdb_wal *w) {
  if (!w) return;
  tdb_mfree(w->map);
  tdb_mfree(w->offsets);
  tdb_mfree(w);
}

int tdb_wal_read_page(tdb_wal *w, tdb_pgno pgno, uint8_t *out, int *found) {
  *found = 0;
  if (pgno >= w->map_cap || w->map[pgno] == 0) return TDB_OK;
  uint64_t idx = w->map[pgno] - 1;
  if (idx >= w->nframe) return TDB_OK; /* uncommitted frame */
  uint64_t off = frame_offset(w, idx) + TDB_WAL_FRAME_HDR_SIZE;
  if (w->version == TDB_WAL_VERSION_V2) {
    uint8_t pfx[TDB_WAL_V2_PAYLOAD_HDR];
    int rc = tdb_file_read(w->file, pfx, sizeof(pfx), off);
    if (rc) return rc;
    uint32_t phys = tdb_get_u32(pfx);
    uint8_t  codec = pfx[4];
    off += sizeof(pfx);
    if (codec == TDB_COMP_NONE) {
      rc = tdb_file_read(w->file, out, phys, off);
      if (rc) return rc;
      if (phys < w->page_size) memset(out + phys, 0, w->page_size - phys);
    } else {
      uint8_t *buf = (uint8_t *)tdb_malloc(phys);
      if (!buf) return TDB_NOMEM;
      rc = tdb_file_read(w->file, buf, phys, off);
      if (rc) { tdb_mfree(buf); return rc; }
      size_t out_n = w->page_size;
      rc = tdb_decompress(buf, phys, out, &out_n);
      tdb_mfree(buf);
      if (rc) return rc;
      if (out_n != w->page_size) return TDB_CORRUPT;
    }
  } else {
    int rc = tdb_file_read(w->file, out, w->page_size, off);
    if (rc) return rc;
  }
  *found = 1;
  return TDB_OK;
}

int tdb_wal_append(tdb_wal *w, tdb_pgno pgno, const uint8_t *page,
                   int is_commit, uint32_t db_size) {
  uint64_t idx = w->nframe;
  uint8_t fhdr[TDB_WAL_FRAME_HDR_SIZE];
  tdb_put_u32(fhdr + TDB_WAL_FR_PGNO, pgno);
  tdb_put_u32(fhdr + TDB_WAL_FR_DBSIZE, is_commit ? db_size : 0);
  tdb_put_u32(fhdr + TDB_WAL_FR_SALT1, w->salt1);
  tdb_put_u32(fhdr + TDB_WAL_FR_SALT2, w->salt2);

  uint32_t seed = (idx == 0) ? w->salt1 : 0;
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

  uint64_t off = (w->version == TDB_WAL_VERSION_V2) ? w->end_off : frame_offset(w, idx);
  int rc = tdb_file_write(w->file, fhdr, sizeof(fhdr), off);
  if (rc) return rc;

  uint32_t phys = w->page_size;
  uint8_t  codec = TDB_COMP_NONE;
  uint8_t *cbuf = NULL;
  if (w->version == TDB_WAL_VERSION_V2 && tdb_compress_codec() != TDB_COMP_NONE) {
    size_t bound = tdb_compress_bound(w->page_size);
    cbuf = (uint8_t *)tdb_malloc(bound);
    if (!cbuf) return TDB_NOMEM;
    size_t cn = bound;
    if (tdb_compress(page, w->page_size, cbuf, &cn) == TDB_OK && cn < w->page_size) {
      phys = (uint32_t)cn;
      codec = (uint8_t)tdb_compress_codec();
    } else {
      tdb_mfree(cbuf); cbuf = NULL;
    }
  }

  uint64_t payload_off = off + TDB_WAL_FRAME_HDR_SIZE;
  if (w->version == TDB_WAL_VERSION_V2) {
    uint8_t pfx[TDB_WAL_V2_PAYLOAD_HDR];
    tdb_put_u32(pfx, phys);
    pfx[4] = codec;
    rc = tdb_file_write(w->file, pfx, sizeof(pfx), payload_off);
    if (rc) { tdb_mfree(cbuf); return rc; }
    payload_off += sizeof(pfx);
  }
  rc = tdb_file_write(w->file, cbuf ? cbuf : page, phys, payload_off);
  tdb_mfree(cbuf);
  if (rc) return rc;

  if (w->version == TDB_WAL_VERSION_V2) {
    if (offsets_ensure(w, idx + 1)) return TDB_NOMEM;
    w->offsets[idx] = off;
    w->end_off = payload_off + phys;
  } else {
    w->end_off = payload_off + phys;          /* keeps a usable tail offset */
  }
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
    int found = 0;
    rc = tdb_wal_read_page(w, p, page, &found);
    if (rc || !found) { if (rc) goto done; else continue; }
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
  w->end_off = TDB_WAL_HDR_SIZE;
  rc = tdb_file_sync(w->file);

done:
  tdb_mfree(page);
  return rc;
}
