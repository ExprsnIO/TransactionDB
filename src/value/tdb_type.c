/* tdb_type.c — affinity derivation, coercion, and value comparison. */
#include "tdb_type.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static int ci_contains(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++) {
    if (strncasecmp(p, needle, nl) == 0) return 1;
  }
  return 0;
}

tdb_affinity tdb_affinity_from_decltype(const char *decltype) {
  if (!decltype || !*decltype) return TDB_AFF_BLOB;
  if (ci_contains(decltype, "INT")) return TDB_AFF_INTEGER;
  if (ci_contains(decltype, "CHAR") || ci_contains(decltype, "CLOB") ||
      ci_contains(decltype, "TEXT")) return TDB_AFF_TEXT;
  if (ci_contains(decltype, "BLOB")) return TDB_AFF_BLOB;
  if (ci_contains(decltype, "REAL") || ci_contains(decltype, "FLOA") ||
      ci_contains(decltype, "DOUB")) return TDB_AFF_REAL;
  return TDB_AFF_NUMERIC;
}

/* Can the text be losslessly parsed as an integer or real? */
static int looks_numeric(const char *s, int *is_int, int64_t *iv, double *rv) {
  if (!s || !*s) return 0;
  char *end = NULL;
  long long ll = strtoll(s, &end, 10);
  if (end && *end == '\0') {
    *is_int = 1;
    *iv = (int64_t)ll;
    return 1;
  }
  end = NULL;
  double d = strtod(s, &end);
  if (end && *end == '\0') {
    *is_int = 0;
    *rv = d;
    return 1;
  }
  return 0;
}

void tdb_apply_affinity(tdb_value *v, tdb_affinity aff) {
  if (v->type == TDB_VAL_NULL || v->type == TDB_VAL_BLOB) return;
  switch (aff) {
    case TDB_AFF_INTEGER:
    case TDB_AFF_NUMERIC:
      if (v->type == TDB_VAL_TEXT) {
        int is_int = 0; int64_t iv = 0; double rv = 0;
        if (looks_numeric(v->u.s.p, &is_int, &iv, &rv)) {
          if (is_int) tdb_value_set_int(v, iv);
          else if (aff == TDB_AFF_INTEGER && (double)(int64_t)rv == rv)
            tdb_value_set_int(v, (int64_t)rv);
          else tdb_value_set_real(v, rv);
        }
      } else if (aff == TDB_AFF_INTEGER && v->type == TDB_VAL_REAL) {
        if ((double)(int64_t)v->u.r == v->u.r) tdb_value_set_int(v, (int64_t)v->u.r);
      }
      break;
    case TDB_AFF_REAL:
      if (v->type == TDB_VAL_INT) tdb_value_set_real(v, (double)v->u.i);
      else if (v->type == TDB_VAL_TEXT) {
        int is_int = 0; int64_t iv = 0; double rv = 0;
        if (looks_numeric(v->u.s.p, &is_int, &iv, &rv))
          tdb_value_set_real(v, is_int ? (double)iv : rv);
      }
      break;
    case TDB_AFF_TEXT:
      if (v->type == TDB_VAL_INT || v->type == TDB_VAL_REAL) {
        const char *t = tdb_value_as_text(v); /* materializes in place */
        (void)t;
      }
      break;
    case TDB_AFF_BLOB:
      break;
  }
}

/* Type class ordering for cross-type comparison: NULL < numbers < text < blob */
static int type_class(tdb_valtype t) {
  switch (t) {
    case TDB_VAL_NULL: return 0;
    case TDB_VAL_INT:
    case TDB_VAL_REAL: return 1;
    case TDB_VAL_TEXT: return 2;
    case TDB_VAL_BLOB: return 3;
    case TDB_VAL_COMPOSITE: return 4;
  }
  return 5;
}

static int cmp_text(const char *a, int na, const char *b, int nb,
                    tdb_collation coll) {
  int n = na < nb ? na : nb;
  int c;
  if (coll == TDB_COLL_NOCASE) {
    c = strncasecmp(a, b, (size_t)n);
  } else {
    c = memcmp(a, b, (size_t)n);
  }
  if (c) return c < 0 ? -1 : 1;
  if (na == nb) return 0;
  return na < nb ? -1 : 1;
}

int tdb_value_compare(const tdb_value *a, const tdb_value *b,
                      tdb_collation coll) {
  int ca = type_class(a->type), cb = type_class(b->type);
  if (ca != cb) return ca < cb ? -1 : 1;

  switch (a->type) {
    case TDB_VAL_NULL:
      return 0;
    case TDB_VAL_INT:
    case TDB_VAL_REAL: {
      double da = tdb_value_as_real(a), db = tdb_value_as_real(b);
      /* prefer exact integer compare when both are ints */
      if (a->type == TDB_VAL_INT && b->type == TDB_VAL_INT) {
        if (a->u.i < b->u.i) return -1;
        return a->u.i > b->u.i ? 1 : 0;
      }
      if (da < db) return -1;
      return da > db ? 1 : 0;
    }
    case TDB_VAL_TEXT:
      return cmp_text(a->u.s.p, a->u.s.n, b->u.s.p, b->u.s.n, coll);
    case TDB_VAL_BLOB:
    case TDB_VAL_COMPOSITE: {
      int n = a->u.s.n < b->u.s.n ? a->u.s.n : b->u.s.n;
      int c = memcmp(a->u.s.p, b->u.s.p, (size_t)n);
      if (c) return c < 0 ? -1 : 1;
      if (a->u.s.n == b->u.s.n) return 0;
      return a->u.s.n < b->u.s.n ? -1 : 1;
    }
  }
  return 0;
}
