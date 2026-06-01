/* tdb_value.c — runtime SQL value operations. */
#include "tdb_value.h"
#include "tdb_record.h"              /* composite (un)packing reuses the codec */
#include "../common/tdb_mem.h"
#include "../common/tdb_buf.h"

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

int tdb_value_set_composite(tdb_value *v, const void *p, int n, int copy) {
  return value_set_bytes(v, TDB_VAL_COMPOSITE, p, n, copy);
}

int tdb_value_composite_pack(tdb_value *out, const tdb_value *fields, int n) {
  tdb_buf b; tdb_buf_init(&b);
  int rc = tdb_record_encode(fields, n, &b);
  if (rc == TDB_OK) rc = tdb_value_set_composite(out, b.data, (int)b.len, 1);
  tdb_buf_free(&b);
  return rc;
}

int tdb_value_composite_count(const tdb_value *v) {
  if (v->type != TDB_VAL_COMPOSITE || !v->u.s.p) return 0;
  tdb_value dummy; int nc = 0;
  if (tdb_record_decode((const uint8_t *)v->u.s.p, (size_t)v->u.s.n,
                        &dummy, 0, &nc) != TDB_OK)
    return 0;
  return nc;
}

int tdb_value_composite_field(const tdb_value *v, int i, tdb_value *out) {
  tdb_value_clear(out);
  if (v->type != TDB_VAL_COMPOSITE || !v->u.s.p || i < 0) return TDB_RANGE;
  int want = i + 1;
  tdb_value *fs = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)want);
  if (!fs) return TDB_NOMEM;
  int nc = 0;
  int rc = tdb_record_decode((const uint8_t *)v->u.s.p, (size_t)v->u.s.n, fs, want, &nc);
  if (rc == TDB_OK) {
    if (i < nc) rc = tdb_value_copy(out, &fs[i]);
    else rc = TDB_RANGE;
  }
  for (int k = 0; k < want && k < nc; k++) tdb_value_clear(&fs[k]);
  tdb_mfree(fs);
  return rc;
}

void tdb_value_clear(tdb_value *v) {
  if ((v->type == TDB_VAL_TEXT || v->type == TDB_VAL_BLOB ||
       v->type == TDB_VAL_COMPOSITE) && v->u.s.owned) {
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
    case TDB_VAL_COMPOSITE: return tdb_value_set_composite(dst, src->u.s.p, src->u.s.n, 1);
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
    case TDB_VAL_COMPOSITE: {
      /* render as "(field, field, ...)" — materializes into `m`'s storage */
      tdb_buf b; tdb_buf_init(&b);
      tdb_buf_putc(&b, '(');
      int n = tdb_value_composite_count(v);
      for (int i = 0; i < n; i++) {
        tdb_value f; tdb_value_init(&f);
        if (i) tdb_buf_append(&b, ", ", 2);
        if (tdb_value_composite_field(v, i, &f) == TDB_OK) {
          if (f.type == TDB_VAL_NULL) tdb_buf_append(&b, "NULL", 4);
          else {
            const char *t = tdb_value_as_text(&f);
            if (t) tdb_buf_append(&b, t, strlen(t));
          }
        }
        tdb_value_clear(&f);
      }
      tdb_buf_putc(&b, ')');
      tdb_value_set_text(m, (const char *)b.data, (int)b.len, 1);
      tdb_buf_free(&b);
      return m->u.s.p;
    }
  }
  return NULL;
}
