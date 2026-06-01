/* test_wal.c — WAL append/read, recovery, and commit-boundary discard. */
#include "tdb_test.h"
#include "../src/storage/tdb_wal.h"
#include "../src/storage/tdb_file.h"

#include <stdio.h>
#include <string.h>

#define PS 256

static tdb_file *open_file(const char *path, int flags) {
  tdb_file *f = NULL;
  const tdb_vfs *vfs = tdb_vfs_default();
  int rc = vfs->open(vfs, path, flags, &f);
  return rc == TDB_OK ? f : NULL;
}

static void fill(uint8_t *p, uint8_t v) { memset(p, v, PS); }

static void test_append_read_recover(void) {
  const char *path = "test_wal_1.wal";
  remove(path);

  /* write a committed batch of 3 pages */
  {
    tdb_file *f = open_file(path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
    TDB_CHECK(f != NULL);
    tdb_wal *w = NULL;
    TDB_CHECK_EQ(tdb_wal_open(f, PS, &w), TDB_OK);

    uint8_t buf[PS];
    fill(buf, 0xA1); tdb_wal_append(w, 2, buf, 0, 0);
    fill(buf, 0xB2); tdb_wal_append(w, 5, buf, 0, 0);
    fill(buf, 0xC3); tdb_wal_append(w, 2, buf, 1, 6); /* commit, db_size=6 */
    TDB_CHECK_EQ(tdb_wal_sync(w), TDB_OK);

    int found = 0;
    TDB_CHECK_EQ(tdb_wal_read_page(w, 2, buf, &found), TDB_OK);
    TDB_CHECK(found && buf[0] == 0xC3);          /* latest wins */
    TDB_CHECK_EQ(tdb_wal_read_page(w, 5, buf, &found), TDB_OK);
    TDB_CHECK(found && buf[0] == 0xB2);
    TDB_CHECK_EQ(tdb_wal_db_size(w), 6u);

    tdb_wal_close(w);
    tdb_file_close(f);
  }

  /* reopen: recovery should restore committed frames */
  {
    tdb_file *f = open_file(path, TDB_OPEN_READWRITE);
    tdb_wal *w = NULL;
    TDB_CHECK_EQ(tdb_wal_open(f, PS, &w), TDB_OK);
    TDB_CHECK_EQ(tdb_wal_db_size(w), 6u);
    uint8_t buf[PS];
    int found = 0;
    TDB_CHECK_EQ(tdb_wal_read_page(w, 2, buf, &found), TDB_OK);
    TDB_CHECK(found && buf[0] == 0xC3);
    tdb_wal_close(w);
    tdb_file_close(f);
  }
  remove(path);
}

static void test_uncommitted_discarded(void) {
  const char *path = "test_wal_2.wal";
  remove(path);
  uint8_t buf[PS];

  {
    tdb_file *f = open_file(path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
    tdb_wal *w = NULL;
    tdb_wal_open(f, PS, &w);
    fill(buf, 0x11); tdb_wal_append(w, 3, buf, 1, 4); /* committed */
    /* a trailing, uncommitted frame (simulated crash mid-next-txn) */
    fill(buf, 0x99); tdb_wal_append(w, 3, buf, 0, 0);
    tdb_wal_sync(w);
    tdb_wal_close(w);
    tdb_file_close(f);
  }
  {
    tdb_file *f = open_file(path, TDB_OPEN_READWRITE);
    tdb_wal *w = NULL;
    tdb_wal_open(f, PS, &w);
    int found = 0;
    tdb_wal_read_page(w, 3, buf, &found);
    TDB_CHECK(found && buf[0] == 0x11);  /* uncommitted 0x99 dropped */
    TDB_CHECK_EQ(tdb_wal_db_size(w), 4u);
    tdb_wal_close(w);
    tdb_file_close(f);
  }
  remove(path);
}

static void test_checkpoint(void) {
  const char *dbpath = "test_wal_ck.db";
  const char *wpath = "test_wal_ck.wal";
  remove(dbpath); remove(wpath);
  uint8_t buf[PS];

  tdb_file *db = open_file(dbpath, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
  tdb_file *wf = open_file(wpath, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
  tdb_wal *w = NULL;
  tdb_wal_open(wf, PS, &w);
  fill(buf, 0x77); tdb_wal_append(w, 2, buf, 1, 2);
  tdb_wal_sync(w);
  TDB_CHECK_EQ(tdb_wal_checkpoint(w, db, NULL), TDB_OK);
  TDB_CHECK_EQ(tdb_wal_frame_count(w), 0u); /* log reset */

  /* page 2 (file offset (2-1)*PS) now holds the checkpointed image */
  uint8_t rd[PS];
  TDB_CHECK_EQ(tdb_file_read(db, rd, PS, (uint64_t)PS), TDB_OK);
  TDB_CHECK(rd[0] == 0x77);

  tdb_wal_close(w);
  tdb_file_close(wf);
  tdb_file_close(db);
  remove(dbpath); remove(wpath);
}

/* v2 frames carry a per-frame physical size and codec id. The build wires
** to zstd via tdb_compress; in passthrough mode (TDB_COMP_NONE) the frame
** payload equals the page size and codec=0. Either way: round-trip + replay
** must reproduce the committed page bytes. */
static void test_compressed_frames(void) {
  const char *path = "test_wal_compressed.wal";
  remove(path);
  tdb_file *f = open_file(path, TDB_OPEN_READWRITE | TDB_OPEN_CREATE);
  TDB_CHECK(f != NULL);
  tdb_wal *w = NULL;
  TDB_CHECK_EQ(tdb_wal_open(f, PS, &w), TDB_OK);

  /* highly compressible page — zeros */
  uint8_t buf[PS]; memset(buf, 0, sizeof(buf));
  TDB_CHECK_EQ(tdb_wal_append(w, 7, buf, 1, 8), TDB_OK);
  TDB_CHECK_EQ(tdb_wal_sync(w), TDB_OK);

  uint8_t out[PS]; memset(out, 0xff, sizeof(out));
  int found = 0;
  TDB_CHECK_EQ(tdb_wal_read_page(w, 7, out, &found), TDB_OK);
  TDB_CHECK(found);
  for (int i = 0; i < PS; i++) TDB_CHECK_EQ(out[i], 0);

  tdb_wal_close(w); tdb_file_close(f);

  /* reopen + replay */
  f = open_file(path, TDB_OPEN_READWRITE);
  TDB_CHECK_EQ(tdb_wal_open(f, PS, &w), TDB_OK);
  memset(out, 0xff, sizeof(out));
  found = 0;
  TDB_CHECK_EQ(tdb_wal_read_page(w, 7, out, &found), TDB_OK);
  TDB_CHECK(found);
  for (int i = 0; i < PS; i++) TDB_CHECK_EQ(out[i], 0);
  TDB_CHECK_EQ(tdb_wal_db_size(w), 8u);
  tdb_wal_close(w); tdb_file_close(f);
  remove(path);
}

static tdb_test_case cases[] = {
  {"append_read_recover", test_append_read_recover},
  {"uncommitted_discarded", test_uncommitted_discarded},
  {"checkpoint", test_checkpoint},
  {"compressed_frames", test_compressed_frames},
};
TDB_MAIN(cases)
