/*
** tdb_value.h — the runtime, dynamically-typed SQL value.
**
** `struct tdb_value` is the concrete definition behind the opaque `tdb_value`
** typedef in the public API, so user functions receive pointers to these.
*/
#ifndef TDB_VALUE_H
#define TDB_VALUE_H

#include "tdb_internal.h"

typedef enum tdb_valtype {
  TDB_VAL_NULL = 0,
  TDB_VAL_INT,
  TDB_VAL_REAL,
  TDB_VAL_TEXT,
  TDB_VAL_BLOB,
  /* A composite (ROW/STRUCT) value: an ordered tuple of fields, stored as the
  ** record encoding of those fields (see tdb_record). The bytes live in the
  ** same `u.s` storage as TEXT/BLOB; field access goes through the helpers
  ** below. Composites may nest (a field can itself be composite). */
  TDB_VAL_COMPOSITE
} tdb_valtype;

struct tdb_value {
  tdb_valtype type;
  union {
    int64_t i;
    double  r;
    struct {
      char    *p;     /* text/blob bytes (text is NUL-terminated)   */
      int      n;     /* byte length (excluding the NUL for text)   */
      uint8_t  owned; /* 1 if `p` was malloc'd and must be freed    */
    } s;
  } u;
};

void tdb_value_init(tdb_value *v);                 /* -> NULL value */
void tdb_value_set_null(tdb_value *v);
void tdb_value_set_int(tdb_value *v, int64_t i);
void tdb_value_set_real(tdb_value *v, double r);
/* copy != 0 duplicates the bytes; otherwise borrows `p` (caller keeps alive) */
int  tdb_value_set_text(tdb_value *v, const char *p, int n, int copy);
int  tdb_value_set_blob(tdb_value *v, const void *p, int n, int copy);
/* Wrap already-encoded composite bytes (a record encoding) as a composite. */
int  tdb_value_set_composite(tdb_value *v, const void *p, int n, int copy);

/* Pack `n` field values into one composite value (deep copy of the fields). */
int  tdb_value_composite_pack(tdb_value *out, const tdb_value *fields, int n);
/* Number of fields in a composite (0 if `v` is not a composite). */
int  tdb_value_composite_count(const tdb_value *v);
/* Copy field `i` (0-based) of a composite into `out`. TDB_OK / TDB_RANGE. */
int  tdb_value_composite_field(const tdb_value *v, int i, tdb_value *out);

void tdb_value_clear(tdb_value *v);                /* free owned bytes */
int  tdb_value_copy(tdb_value *dst, const tdb_value *src); /* deep copy */

int64_t     tdb_value_as_int(const tdb_value *v);
double      tdb_value_as_real(const tdb_value *v);
const char *tdb_value_as_text(const tdb_value *v); /* may coerce/format */

#endif /* TDB_VALUE_H */
