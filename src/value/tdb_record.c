/* tdb_record.c — record codec with SQLite-style serial types. */
#include "tdb_record.h"
#include "../common/tdb_util.h"
#include "../common/tdb_mem.h"

#include <string.h>

/* serial type -> body byte length */
static int serial_body_len(uint64_t st) {
  switch (st) {
    case 0: return 0;  /* NULL */
    case 1: return 1;  /* int8  */
    case 2: return 2;  /* int16 */
    case 3: return 3;  /* int24 */
    case 4: return 4;  /* int32 */
    case 5: return 6;  /* int48 */
    case 6: return 8;  /* int64 */
    case 7: return 8;  /* float64 */
    case 8: return 0;  /* literal 0 */
    case 9: return 0;  /* literal 1 */
    default:
      if (st >= 12) return (int)((st - 12) / 2);
      return -1; /* reserved 10,11 */
  }
}

static uint64_t int_serial_type(int64_t v, int *body_len) {
  uint64_t u = (uint64_t)v;
  if (v == 0) { *body_len = 0; return 8; }
  if (v == 1) { *body_len = 0; return 9; }
  if (v >= -128 && v <= 127)            { *body_len = 1; return 1; }
  if (v >= -32768 && v <= 32767)        { *body_len = 2; return 2; }
  if (v >= -8388608 && v <= 8388607)    { *body_len = 3; return 3; }
  if (v >= -2147483648LL && v <= 2147483647LL) { *body_len = 4; return 4; }
  if (v >= -140737488355328LL && v <= 140737488355327LL) { *body_len = 6; return 5; }
  (void)u;
  *body_len = 8; return 6;
}

static void put_be_signed(uint8_t *p, int64_t v, int n) {
  for (int i = n - 1; i >= 0; i--) {
    p[i] = (uint8_t)(v & 0xff);
    v >>= 8;
  }
}

static int64_t get_be_signed(const uint8_t *p, int n) {
  if (n == 0) return 0;
  int64_t v = (p[0] & 0x80) ? -1 : 0; /* sign extend */
  for (int i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

int tdb_record_encode(const tdb_value *vals, int n, tdb_buf *out) {
  /* compute serial types + body lengths */
  uint64_t st_stack[16];
  int      bl_stack[16];
  uint64_t *st = st_stack;
  int      *bl = bl_stack;
  if (n > 16) {
    st = (uint64_t *)tdb_malloc(sizeof(uint64_t) * (size_t)n);
    bl = (int *)tdb_malloc(sizeof(int) * (size_t)n);
    if (!st || !bl) { tdb_mfree(st); tdb_mfree(bl); return TDB_NOMEM; }
  }
  int rc = TDB_OK;

  for (int i = 0; i < n; i++) {
    const tdb_value *v = &vals[i];
    switch (v->type) {
      case TDB_VAL_NULL: st[i] = 0; bl[i] = 0; break;
      case TDB_VAL_INT:  st[i] = int_serial_type(v->u.i, &bl[i]); break;
      case TDB_VAL_REAL: st[i] = 7; bl[i] = 8; break;
      case TDB_VAL_TEXT: bl[i] = v->u.s.n; st[i] = (uint64_t)13 + (uint64_t)2 * (uint64_t)(unsigned)v->u.s.n; break;
      case TDB_VAL_BLOB:
      case TDB_VAL_COMPOSITE: bl[i] = v->u.s.n; st[i] = (uint64_t)12 + (uint64_t)2 * (uint64_t)(unsigned)v->u.s.n; break;
      default: st[i] = 0; bl[i] = 0; break;
    }
  }

  if ((rc = tdb_buf_put_varint(out, (uint64_t)n))) goto done;
  for (int i = 0; i < n; i++)
    if ((rc = tdb_buf_put_varint(out, st[i]))) goto done;

  for (int i = 0; i < n; i++) {
    const tdb_value *v = &vals[i];
    if (bl[i] == 0 && v->type != TDB_VAL_TEXT && v->type != TDB_VAL_BLOB &&
        v->type != TDB_VAL_COMPOSITE) continue;
    if ((rc = tdb_buf_reserve(out, (size_t)bl[i] + 8))) goto done;
    uint8_t *dst = out->data + out->len;
    switch (v->type) {
      case TDB_VAL_INT:  put_be_signed(dst, v->u.i, bl[i]); break;
      case TDB_VAL_REAL: {
        uint64_t bits; memcpy(&bits, &v->u.r, 8);
        tdb_put_u64(dst, bits);
        break;
      }
      case TDB_VAL_TEXT:
      case TDB_VAL_BLOB:
      case TDB_VAL_COMPOSITE:
        if (bl[i]) memcpy(dst, v->u.s.p, (size_t)bl[i]);
        break;
      default: break;
    }
    out->len += (size_t)bl[i];
  }

done:
  if (st != st_stack) tdb_mfree(st);
  if (bl != bl_stack) tdb_mfree(bl);
  return rc;
}

int tdb_record_decode(const uint8_t *p, size_t len, tdb_value *out, int max,
                      int *ncols) {
  size_t off = 0;
  uint64_t n = 0;
  int adv = tdb_get_varint(p, len, &n);
  if (adv <= 0) return TDB_CORRUPT;
  off += (size_t)adv;

  /* read serial types */
  uint64_t st_stack[16];
  uint64_t *st = st_stack;
  if (n > 16) {
    st = (uint64_t *)tdb_malloc(sizeof(uint64_t) * (size_t)n);
    if (!st) return TDB_NOMEM;
  }
  int rc = TDB_OK;
  for (uint64_t i = 0; i < n; i++) {
    adv = tdb_get_varint(p + off, len - off, &st[i]);
    if (adv <= 0) { rc = TDB_CORRUPT; goto done; }
    off += (size_t)adv;
  }

  for (uint64_t i = 0; i < n; i++) {
    int bl = serial_body_len(st[i]);
    if (bl < 0 || off + (size_t)bl > len) { rc = TDB_CORRUPT; goto done; }
    if ((int)i < max) {
      tdb_value *v = &out[i];
      tdb_value_init(v);
      const uint8_t *body = p + off;
      if (st[i] == 0) {
        tdb_value_set_null(v);
      } else if (st[i] == 8) {
        tdb_value_set_int(v, 0);
      } else if (st[i] == 9) {
        tdb_value_set_int(v, 1);
      } else if (st[i] >= 1 && st[i] <= 6) {
        tdb_value_set_int(v, get_be_signed(body, bl));
      } else if (st[i] == 7) {
        uint64_t bits = tdb_get_u64(body);
        double d; memcpy(&d, &bits, 8);
        tdb_value_set_real(v, d);
      } else if (st[i] >= 12 && (st[i] % 2) == 0) {
        tdb_value_set_blob(v, body, bl, 0); /* borrow */
      } else {
        tdb_value_set_text(v, (const char *)body, bl, 0); /* borrow */
      }
    }
    off += (size_t)bl;
  }
  if (ncols) *ncols = (int)n;

done:
  if (st != st_stack) tdb_mfree(st);
  return rc;
}

int tdb_record_compare(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                       const tdb_keyinfo *ki) {
  tdb_value va[16], vb[16];
  int ca = 0, cb = 0;
  /* For comparison we only need up to ki->ncol columns. */
  int maxa = ki && ki->ncol > 0 && ki->ncol <= 16 ? ki->ncol : 16;
  if (tdb_record_decode(a, na, va, maxa, &ca) != TDB_OK) return 0;
  if (tdb_record_decode(b, nb, vb, maxa, &cb) != TDB_OK) return 0;
  int n = ca < cb ? ca : cb;
  int result = 0;
  for (int i = 0; i < n; i++) {
    tdb_collation coll = (ki && ki->coll) ? ki->coll[i] : TDB_COLL_BINARY;
    int c = tdb_value_compare(&va[i], &vb[i], coll);
    if (ki && ki->desc && ki->desc[i]) c = -c;
    if (c) { result = c; break; }
  }
  if (result == 0 && ca != cb) result = ca < cb ? -1 : 1;
  for (int i = 0; i < ca; i++) tdb_value_clear(&va[i]);
  for (int i = 0; i < cb; i++) tdb_value_clear(&vb[i]);
  return result;
}
