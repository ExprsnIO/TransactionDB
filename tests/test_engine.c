/* test_engine.c — row engine: insert/scan, MVCC snapshot isolation, delete. */
#include "tdb_test.h"
#include "../src/storage/tdb_storage.h"
#include "../src/storage/tdb_pager.h"
#include "../src/catalog/tdb_catalog.h"
#include "../src/txn/tdb_txn.h"
#include "../src/txn/tdb_lock.h"
#include "../src/value/tdb_record.h"

#include <string.h>

/* encode a (id INTEGER, name TEXT) row record */
static void enc_row(tdb_buf *b, int64_t id, const char *name) {
  tdb_value v[2];
  tdb_value_init(&v[0]); tdb_value_init(&v[1]);
  tdb_value_set_int(&v[0], id);
  tdb_value_set_text(&v[1], name, -1, 1);
  tdb_buf_reset(b);
  tdb_record_encode(v, 2, b);
  tdb_value_clear(&v[0]); tdb_value_clear(&v[1]);
}

static void dec_name(const uint8_t *rec, int n, char *out, int cap) {
  tdb_value v[2]; int nc;
  tdb_record_decode(rec, (size_t)n, v, 2, &nc);
  int len = v[1].u.s.n < cap - 1 ? v[1].u.s.n : cap - 1;
  memcpy(out, v[1].u.s.p, (size_t)len);
  out[len] = '\0';
  tdb_value_clear(&v[0]); tdb_value_clear(&v[1]);
}

static tdb_table *make_table(void) {
  tdb_table *t = tdb_table_new("people");
  tdb_column c;
  tdb_column_init(&c, "id", tdb_typespec_parse("INTEGER")); c.pk = 1;
  tdb_table_add_column(t, &c);
  tdb_column_init(&c, "name", tdb_typespec_parse("TEXT"));
  tdb_table_add_column(t, &c);
  return t;
}

static int count_visible(tdb_storage *s, tdb_txn *txn, tdb_table *t) {
  tdb_scan *sc; s->vtab->scan_open(s, txn, t, NULL, NULL, 0, &sc);
  tdb_rowid rid; const uint8_t *rec; int n; int count = 0;
  while (s->vtab->scan_next(sc, &rid, &rec, &n) == TDB_ROW) count++;
  s->vtab->scan_close(sc);
  return count;
}

static void test_engine_mvcc(void) {
  tdb_pager *p = NULL;
  TDB_CHECK_EQ(tdb_pager_open(NULL, ":memory:", TDB_OPEN_MEMORY, &p), TDB_OK);
  tdb_catalog *cat; TDB_CHECK_EQ(tdb_catalog_open(p, &cat), TDB_OK);
  tdb_lockmgr *lm = tdb_lockmgr_new();
  tdb_txnmgr *tm; TDB_CHECK_EQ(tdb_txnmgr_open(p, lm, &tm), TDB_OK);
  tdb_storage *s; TDB_CHECK_EQ(tdb_engine_row_open(p, &s), TDB_OK);

  /* DDL: create the table (engine btrees + catalog row) atomically */
  tdb_table *t = make_table();
  tdb_txn *d;
  tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 1, &d);
  TDB_CHECK_EQ(s->vtab->create_table(s, d, t), TDB_OK);
  TDB_CHECK_EQ(tdb_catalog_add_table(cat, t), TDB_OK);
  tdb_txn_commit(d);

  /* insert two rows */
  tdb_txn *w; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 1, &w);
  tdb_buf b; tdb_buf_init(&b);
  enc_row(&b, 1, "alice"); s->vtab->insert(s, w, t, 1, b.data, (int)b.len);
  enc_row(&b, 2, "bob");   s->vtab->insert(s, w, t, 2, b.data, (int)b.len);
  tdb_txn_commit(w);

  /* a reader sees both */
  tdb_txn *r1; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 0, &r1);
  TDB_CHECK_EQ(count_visible(s, r1, t), 2);

  /* R holds an old snapshot; W2 updates alice -> ALICE and commits */
  tdb_txn *rold; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 0, &rold);
  tdb_txn *w2; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 1, &w2);
  enc_row(&b, 1, "ALICE"); s->vtab->update(s, w2, t, 1, b.data, (int)b.len);
  tdb_txn_commit(w2);

  /* rold (snapshot before the update) still sees "alice" */
  const uint8_t *rec; int n; int found;
  s->vtab->seek_rowid(s, rold, t, 1, &rec, &n, &found);
  TDB_CHECK(found);
  char name[32]; dec_name(rec, n, name, sizeof(name));
  TDB_CHECK_STR(name, "alice");

  /* a fresh reader sees the new value */
  tdb_txn *rnew; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 0, &rnew);
  s->vtab->seek_rowid(s, rnew, t, 1, &rec, &n, &found);
  TDB_CHECK(found);
  dec_name(rec, n, name, sizeof(name));
  TDB_CHECK_STR(name, "ALICE");

  tdb_txn_rollback(r1); tdb_txn_rollback(rold); tdb_txn_rollback(rnew);

  /* delete bob */
  tdb_txn *wd; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 1, &wd);
  TDB_CHECK_EQ(s->vtab->remove(s, wd, t, 2), TDB_OK);
  tdb_txn_commit(wd);

  tdb_txn *r3; tdb_txn_begin(tm, TDB_ISO_SNAPSHOT, 0, &r3);
  TDB_CHECK_EQ(count_visible(s, r3, t), 1); /* only alice remains */
  tdb_txn_rollback(r3);

  tdb_buf_free(&b);
  tdb_storage_close(s);
  tdb_txnmgr_close(tm);
  tdb_lockmgr_free(lm);
  tdb_catalog_close(cat);
  tdb_pager_close(p);
}

static tdb_test_case cases[] = {
  {"engine_mvcc", test_engine_mvcc},
};
TDB_MAIN(cases)
