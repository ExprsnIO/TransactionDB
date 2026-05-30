/* tdb_value.c — runtime SQL value operations. */
#include "tdb_value.h"
#include "../common/tdb_mem.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void tdb_value_init(tdb_value *v) {
  v->type = TDB_VAL_NULL;
  v->u.i = 0;
}

void tdb_value_set_null(tdb_value *v) {
  tdb_value_clear(v);
  v->type = TDB_VAL_NULL;
}

void tdb_value_set_int(tdb_value *v, int64_t i) {
  tdb_value_clear(v);
  v->type = TDB_VAL_INT;
  v->u.i = i;
}

void tdb_value_set_real(tdb_value *v, double r) {
  tdb_value_clear(v);
  v->type = TDB_VAL_REAL;
  v->u.r = r;
}

static int value_set_bytes(tdb_value *v, tdb_valtype t, const void *p, int n,
                           int copy) {
  tdb_value_clear(v);
  v->type = t;
  if (n < 0) n = 0;
  if (copy) {
    char *buf = (char *)tdb_malloc((size_t)n + 1);
    if (!buf) {
      v->type = TDB_VAL_NULL;
      return TDB_NOMEM;
    }
    if (n) memcpy(buf, p, (size_t)n);
    buf[n] = '\0';
    v->u.s.p = buf;
    v->u.s.owned = 1;
  } else {
    v->u.s.p = (char *)p;
    v->u.s.owned = 0;
  }
  v->u.s.n = n;
  return TDB_OK;
}

int tdb_value_set_text(tdb_value *v, const char *p, int n, int copy) {
  if (n < 0 && p) n = (int)strlen(p);
  return value_set_bytes(v, TDB_VAL_TEXT, p, n, copy);
}

int tdb_value_set_blob(tdb_value *v, const void *p, int n, int copy) {
  return value_set_bytes(v, TDB_VAL_BLOB, p, n, copy);
}

void tdb_value_clear(tdb_value *v) {
  if ((v->type == TDB_VAL_TEXT || v->type == TDB_VAL_BLOB) && v->u.s.owned) {
    tdb_mfree(v->u.s.p);
  }
  v->type = TDB_VAL_NULL;
  v->u.i = 0;
}

int tdb_value_copy(tdb_value *dst, const tdb_value *src) {
  if (dst == src) return TDB_OK;
  tdb_value_clear(dst);
  switch (src->type) {
    case TDB_VAL_NULL: dst->type = TDB_VAL_NULL; break;
    case TDB_VAL_INT:  tdb_value_set_int(dst, src->u.i); break;
    case TDB_VAL_REAL: tdb_value_set_real(dst, src->u.r); break;
    case TDB_VAL_TEXT: return tdb_value_set_text(dst, src->u.s.p, src->u.s.n, 1);
    case TDB_VAL_BLOB: return tdb_value_set_blob(dst, src->u.s.p, src->u.s.n, 1);
  }
  return TDB_OK;
}

int64_t tdb_value_as_int(const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_INT:  return v->u.i;
    case TDB_VAL_REAL: return (int64_t)v->u.r;
    case TDB_VAL_TEXT: return v->u.s.p ? strtoll(v->u.s.p, NULL, 10) : 0;
    default:           return 0;
  }
}

double tdb_value_as_real(const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_INT:  return (double)v->u.i;
    case TDB_VAL_REAL: return v->u.r;
    case TDB_VAL_TEXT: return v->u.s.p ? strtod(v->u.s.p, NULL) : 0.0;
    default:           return 0.0;
  }
}

/* Returns a pointer that is valid until the value changes. For INT/REAL the
** formatted text is cached in the value's own byte storage. */
const char *tdb_value_as_text(const tdb_value *v) {
  tdb_value *m = (tdb_value *)v; /* lazily materialize into a mutable copy */
  switch (v->type) {
    case TDB_VAL_TEXT:
    case TDB_VAL_BLOB:
      return v->u.s.p ? v->u.s.p : "";
    case TDB_VAL_NULL:
      return NULL;
    case TDB_VAL_INT: {
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.i);
      tdb_value_set_text(m, tmp, -1, 1);
      return m->u.s.p;
    }
    case TDB_VAL_REAL: {
      char tmp[40];
      snprintf(tmp, sizeof(tmp), "%.17g", v->u.r);
      tdb_value_set_text(m, tmp, -1, 1);
      return m->u.s.p;
    }
  }
  return NULL;
}
