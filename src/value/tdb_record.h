/*
** tdb_record.h — row (de)serialization codec.
**
** A "record" is a self-describing serialized array of values: a varint column
** count, a varint "serial type" per column, then the packed body. The serial
** type encoding follows the SQLite scheme so that integers are stored in the
** minimum number of bytes. Records are used for both table rows and index keys.
*/
#ifndef TDB_RECORD_H
#define TDB_RECORD_H

#include "tdb_value.h"
#include "tdb_type.h"
#include "../common/tdb_buf.h"

/* Describes how to compare a multi-column index key record. */
typedef struct tdb_keyinfo {
  int            ncol;
  tdb_collation *coll;  /* per-column collation, or NULL for all-BINARY */
  uint8_t       *desc;  /* per-column descending flag, or NULL for all-ASC */
} tdb_keyinfo;

/* Encode `n` values into `out` (appended). Returns TDB_OK/TDB_NOMEM. */
int tdb_record_encode(const tdb_value *vals, int n, tdb_buf *out);

/* Decode up to `max` values from `p[0..len)`. Borrowed text/blob point into
** `p` (zero-copy, valid while `p` lives). Sets *ncols to columns decoded. */
int tdb_record_decode(const uint8_t *p, size_t len, tdb_value *out, int max,
                      int *ncols);

/* Compare two encoded records as ordered keys. */
int tdb_record_compare(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                       const tdb_keyinfo *ki);

#endif /* TDB_RECORD_H */
