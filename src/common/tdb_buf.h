/* tdb_buf.h — a simple growable byte buffer. */
#ifndef TDB_BUF_H
#define TDB_BUF_H

#include "tdb_internal.h"

typedef struct tdb_buf {
  uint8_t *data;
  size_t   len;
  size_t   cap;
} tdb_buf;

void  tdb_buf_init(tdb_buf *b);
void  tdb_buf_free(tdb_buf *b);
void  tdb_buf_reset(tdb_buf *b);            /* keep capacity, len -> 0 */
int   tdb_buf_reserve(tdb_buf *b, size_t need);
int   tdb_buf_append(tdb_buf *b, const void *p, size_t n);
int   tdb_buf_putc(tdb_buf *b, uint8_t c);
int   tdb_buf_put_varint(tdb_buf *b, uint64_t v);

#endif /* TDB_BUF_H */
