/* tdb_buf.c — growable byte buffer. */
#include "tdb_buf.h"
#include "tdb_mem.h"
#include "tdb_util.h"

#include <string.h>

void tdb_buf_init(tdb_buf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void tdb_buf_free(tdb_buf *b) {
  tdb_mfree(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

void tdb_buf_reset(tdb_buf *b) { b->len = 0; }

int tdb_buf_reserve(tdb_buf *b, size_t need) {
  if (b->len + need <= b->cap) return TDB_OK;
  size_t cap = b->cap ? b->cap : 64;
  while (cap < b->len + need) cap *= 2;
  uint8_t *p = (uint8_t *)tdb_realloc(b->data, cap);
  if (!p) return TDB_NOMEM;
  b->data = p;
  b->cap = cap;
  return TDB_OK;
}

int tdb_buf_append(tdb_buf *b, const void *p, size_t n) {
  int rc = tdb_buf_reserve(b, n);
  if (rc) return rc;
  if (n) memcpy(b->data + b->len, p, n);
  b->len += n;
  return TDB_OK;
}

int tdb_buf_putc(tdb_buf *b, uint8_t c) {
  int rc = tdb_buf_reserve(b, 1);
  if (rc) return rc;
  b->data[b->len++] = c;
  return TDB_OK;
}

int tdb_buf_put_varint(tdb_buf *b, uint64_t v) {
  int rc = tdb_buf_reserve(b, 10);
  if (rc) return rc;
  b->len += (size_t)tdb_put_varint(b->data + b->len, v);
  return TDB_OK;
}
