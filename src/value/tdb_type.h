/*
** tdb_type.h — type affinity, collation, and value comparison.
**
** Column affinities follow the SQLite model (a declared type maps to one of a
** small set of affinities used to coerce values on insert/compare).
*/
#ifndef TDB_TYPE_H
#define TDB_TYPE_H

#include "tdb_value.h"

typedef enum tdb_affinity {
  TDB_AFF_BLOB = 0,   /* no affinity (BLOB)        */
  TDB_AFF_TEXT,
  TDB_AFF_NUMERIC,
  TDB_AFF_INTEGER,
  TDB_AFF_REAL
} tdb_affinity;

typedef enum tdb_collation {
  TDB_COLL_BINARY = 0,
  TDB_COLL_NOCASE
} tdb_collation;

/* Derive affinity from a declared SQL type name (case-insensitive substrings). */
tdb_affinity tdb_affinity_from_decltype(const char *decltype);

/* Apply affinity coercion in place (e.g. text "12" -> int under INTEGER). */
void tdb_apply_affinity(tdb_value *v, tdb_affinity aff);

/* Three-way comparison honoring SQL NULL/type ordering and collation.
** Returns <0, 0, >0. NULLs sort first. */
int tdb_value_compare(const tdb_value *a, const tdb_value *b,
                      tdb_collation coll);

#endif /* TDB_TYPE_H */
