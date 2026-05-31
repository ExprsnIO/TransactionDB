/* tdb_geom.c — geospatial geometry encoding, WKT I/O, and predicates. */
#include "tdb_geom.h"
#include "../common/tdb_util.h"
#include "../common/tdb_mem.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* ---- double <-> bytes (LE, IEEE-754 bit pattern) --------------------- */
static void put_dbl(tdb_buf *b, double d) {
  uint64_t u; memcpy(&u, &d, 8);
  uint8_t tmp[8]; tdb_put_u64(tmp, u);
  tdb_buf_append(b, tmp, 8);
}
static double get_dbl(const uint8_t *p) {
  uint64_t u = tdb_get_u64(p); double d; memcpy(&d, &u, 8); return d;
}
static void put_u32b(tdb_buf *b, uint32_t v) {
  uint8_t tmp[4]; tdb_put_u32(tmp, v); tdb_buf_append(b, tmp, 4);
}

/* ------------------------------ WKT parse ----------------------------- */

typedef struct { const char *p; const char *err; } wkt;

static void wskip(wkt *w) { while (*w->p && isspace((unsigned char)*w->p)) w->p++; }
static int wkeyword(wkt *w, const char *kw) {
  wskip(w);
  size_t n = strlen(kw);
  if (strncasecmp(w->p, kw, n) == 0) { w->p += n; return 1; }
  return 0;
}
static int wchar(wkt *w, char c) { wskip(w); if (*w->p == c) { w->p++; return 1; } return 0; }
static int wnum(wkt *w, double *out) {
  wskip(w);
  char *end = NULL;
  double v = strtod(w->p, &end);
  if (end == w->p) { w->err = "expected a number"; return 0; }
  w->p = end; *out = v; return 1;
}

/* parse a "x y, x y, ..." coordinate list into the buffer (count-prefixed) */
static int parse_coords(wkt *w, tdb_buf *out) {
  if (!wchar(w, '(')) { w->err = "expected '('"; return 0; }
  /* reserve count slot */
  size_t count_at = out->len;
  put_u32b(out, 0);
  uint32_t n = 0;
  for (;;) {
    double x, y;
    if (!wnum(w, &x) || !wnum(w, &y)) return 0;
    put_dbl(out, x); put_dbl(out, y); n++;
    if (wchar(w, ',')) continue;
    break;
  }
  if (!wchar(w, ')')) { w->err = "expected ')'"; return 0; }
  tdb_put_u32(out->data + count_at, n);   /* patch count */
  return 1;
}

int tdb_geom_from_wkt(const char *wktstr, tdb_buf *out, const char **why) {
  if (why) *why = NULL;
  tdb_buf_reset(out);
  wkt w = { wktstr, NULL };

  if (wkeyword(&w, "POINT")) {
    if (!wchar(&w, '(')) { if (why) *why = "expected '(' after POINT"; return TDB_MISMATCH; }
    double x, y;
    if (!wnum(&w, &x) || !wnum(&w, &y)) { if (why) *why = w.err ? w.err : "bad POINT"; return TDB_MISMATCH; }
    if (!wchar(&w, ')')) { if (why) *why = "expected ')'"; return TDB_MISMATCH; }
    tdb_buf_putc(out, TDB_GEOM_POINT);
    put_dbl(out, x); put_dbl(out, y);
  } else if (wkeyword(&w, "LINESTRING")) {
    tdb_buf_putc(out, TDB_GEOM_LINESTRING);
    if (!parse_coords(&w, out)) { if (why) *why = w.err ? w.err : "bad LINESTRING"; return TDB_MISMATCH; }
  } else if (wkeyword(&w, "POLYGON")) {
    tdb_buf_putc(out, TDB_GEOM_POLYGON);
    if (!wchar(&w, '(')) { if (why) *why = "expected '(' after POLYGON"; return TDB_MISMATCH; }
    size_t nring_at = out->len;
    put_u32b(out, 0);
    uint32_t nrings = 0;
    for (;;) {
      if (!parse_coords(&w, out)) { if (why) *why = w.err ? w.err : "bad ring"; return TDB_MISMATCH; }
      nrings++;
      if (wchar(&w, ',')) continue;
      break;
    }
    if (!wchar(&w, ')')) { if (why) *why = "expected ')'"; return TDB_MISMATCH; }
    tdb_put_u32(out->data + nring_at, nrings);
  } else {
    if (why) *why = "unrecognized geometry (expected POINT/LINESTRING/POLYGON)";
    return TDB_MISMATCH;
  }
  wskip(&w);
  if (*w.p != '\0') { if (why) *why = "trailing characters after geometry"; return TDB_MISMATCH; }
  return TDB_OK;
}

/* ------------------------------ WKT format ---------------------------- */

static void fmt_num(tdb_buf *out, double v) {
  char buf[40];
  int n = snprintf(buf, sizeof(buf), "%g", v);
  if (n > 0) tdb_buf_append(out, buf, (size_t)n);
}
/* format a count-prefixed coord list starting at p; returns bytes consumed */
static int fmt_coords(tdb_buf *out, const uint8_t *p, const uint8_t *end) {
  if (p + 4 > end) return -1;
  uint32_t npts = tdb_get_u32(p); const uint8_t *q = p + 4;
  tdb_buf_putc(out, '(');
  for (uint32_t i = 0; i < npts; i++) {
    if (q + 16 > end) return -1;
    if (i) tdb_buf_append(out, ", ", 2);
    fmt_num(out, get_dbl(q)); tdb_buf_putc(out, ' '); fmt_num(out, get_dbl(q + 8));
    q += 16;
  }
  tdb_buf_putc(out, ')');
  return (int)(q - p);
}

int tdb_geom_to_wkt(const uint8_t *blob, int n, tdb_buf *out) {
  tdb_buf_reset(out);
  if (n < 1) return TDB_MISMATCH;
  const uint8_t *end = blob + n;
  int t = blob[0];
  const uint8_t *p = blob + 1;
  if (t == TDB_GEOM_POINT) {
    if (p + 16 > end) return TDB_MISMATCH;
    tdb_buf_append(out, "POINT(", 6);
    fmt_num(out, get_dbl(p)); tdb_buf_putc(out, ' '); fmt_num(out, get_dbl(p + 8));
    tdb_buf_putc(out, ')');
  } else if (t == TDB_GEOM_LINESTRING) {
    tdb_buf_append(out, "LINESTRING", 10);
    if (fmt_coords(out, p, end) < 0) return TDB_MISMATCH;
  } else if (t == TDB_GEOM_POLYGON) {
    if (p + 4 > end) return TDB_MISMATCH;
    uint32_t nrings = tdb_get_u32(p); p += 4;
    tdb_buf_append(out, "POLYGON(", 8);
    for (uint32_t i = 0; i < nrings; i++) {
      if (i) tdb_buf_append(out, ", ", 2);
      int used = fmt_coords(out, p, end);
      if (used < 0) return TDB_MISMATCH;
      p += used;
    }
    tdb_buf_putc(out, ')');
  } else {
    return TDB_MISMATCH;
  }
  tdb_buf_putc(out, '\0');
  out->len--;   /* keep NUL out of the logical length */
  return TDB_OK;
}

/* ------------------------------ validation ---------------------------- */

int tdb_geom_type_of(const uint8_t *blob, int n) {
  return (n >= 1) ? blob[0] : 0;
}

/* walk a count-prefixed coord list; returns bytes consumed or -1 */
static int skip_coords(const uint8_t *p, const uint8_t *end) {
  if (p + 4 > end) return -1;
  uint32_t npts = tdb_get_u32(p);
  const uint8_t *q = p + 4 + (size_t)npts * 16;
  if (q > end) return -1;
  return (int)(q - p);
}

int tdb_geom_valid(const uint8_t *blob, int n) {
  if (n < 1) return 0;
  const uint8_t *end = blob + n;
  const uint8_t *p = blob + 1;
  switch (blob[0]) {
    case TDB_GEOM_POINT: return (p + 16 == end);
    case TDB_GEOM_LINESTRING: {
      int u = skip_coords(p, end); return u >= 0 && p + u == end;
    }
    case TDB_GEOM_POLYGON: {
      if (p + 4 > end) return 0;
      uint32_t nr = tdb_get_u32(p); p += 4;
      for (uint32_t i = 0; i < nr; i++) {
        int u = skip_coords(p, end); if (u < 0) return 0; p += u;
      }
      return p == end;
    }
  }
  return 0;
}

/* ------------------------------ bbox ---------------------------------- */

static void bbox_pts(const uint8_t *p, uint32_t npts, tdb_bbox *bb) {
  for (uint32_t i = 0; i < npts; i++) {
    double x = get_dbl(p), y = get_dbl(p + 8); p += 16;
    if (x < bb->minx) bb->minx = x;
    if (x > bb->maxx) bb->maxx = x;
    if (y < bb->miny) bb->miny = y;
    if (y > bb->maxy) bb->maxy = y;
  }
}

int tdb_geom_bbox(const uint8_t *blob, int n, tdb_bbox *out) {
  if (!tdb_geom_valid(blob, n)) return TDB_MISMATCH;
  out->minx = out->miny = 1e308; out->maxx = out->maxy = -1e308;
  const uint8_t *p = blob + 1;
  switch (blob[0]) {
    case TDB_GEOM_POINT: {
      double x = get_dbl(p), y = get_dbl(p + 8);
      out->minx = out->maxx = x; out->miny = out->maxy = y;
      break;
    }
    case TDB_GEOM_LINESTRING: {
      uint32_t np = tdb_get_u32(p); bbox_pts(p + 4, np, out);
      break;
    }
    case TDB_GEOM_POLYGON: {
      uint32_t nr = tdb_get_u32(p); p += 4;
      for (uint32_t i = 0; i < nr; i++) {
        uint32_t np = tdb_get_u32(p); bbox_pts(p + 4, np, out); p += 4 + (size_t)np * 16;
      }
      break;
    }
  }
  return TDB_OK;
}

int tdb_geom_point_xy(const uint8_t *blob, int n, double *x, double *y) {
  if (n < 17 || blob[0] != TDB_GEOM_POINT) return TDB_MISMATCH;
  *x = get_dbl(blob + 1); *y = get_dbl(blob + 9);
  return TDB_OK;
}

/* representative point: POINT -> itself; else bbox center */
static int rep_point(const uint8_t *b, int n, double *x, double *y) {
  if (tdb_geom_point_xy(b, n, x, y) == TDB_OK) return TDB_OK;
  tdb_bbox bb;
  if (tdb_geom_bbox(b, n, &bb) != TDB_OK) return TDB_MISMATCH;
  *x = (bb.minx + bb.maxx) * 0.5; *y = (bb.miny + bb.maxy) * 0.5;
  return TDB_OK;
}

int tdb_geom_distance(const uint8_t *a, int an, const uint8_t *b, int bn, double *out) {
  double ax, ay, bx, by;
  if (rep_point(a, an, &ax, &ay) != TDB_OK) return TDB_MISMATCH;
  if (rep_point(b, bn, &bx, &by) != TDB_OK) return TDB_MISMATCH;
  double dx = ax - bx, dy = ay - by;
  *out = sqrt(dx * dx + dy * dy);
  return TDB_OK;
}

int tdb_bbox_intersects(const tdb_bbox *a, const tdb_bbox *b) {
  return !(a->maxx < b->minx || a->minx > b->maxx ||
           a->maxy < b->miny || a->miny > b->maxy);
}

/* ray-casting point-in-polygon over the outer ring (ring 0) */
static int point_in_ring(const uint8_t *p, uint32_t npts, double px, double py) {
  int inside = 0;
  for (uint32_t i = 0, j = npts - 1; i < npts; j = i++) {
    double xi = get_dbl(p + (size_t)i * 16),     yi = get_dbl(p + (size_t)i * 16 + 8);
    double xj = get_dbl(p + (size_t)j * 16),     yj = get_dbl(p + (size_t)j * 16 + 8);
    int cross = ((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
    if (cross) inside = !inside;
  }
  return inside;
}

int tdb_geom_contains_point(const uint8_t *blob, int n, double px, double py) {
  if (!tdb_geom_valid(blob, n)) return 0;
  if (blob[0] == TDB_GEOM_POLYGON) {
    const uint8_t *p = blob + 1;
    uint32_t nr = tdb_get_u32(p); p += 4;
    if (nr == 0) return 0;
    uint32_t np = tdb_get_u32(p);
    int in_outer = point_in_ring(p + 4, np, px, py);
    if (!in_outer) return 0;
    p += 4 + (size_t)np * 16;
    /* holes: inside a hole -> not contained */
    for (uint32_t i = 1; i < nr; i++) {
      uint32_t hn = tdb_get_u32(p);
      if (point_in_ring(p + 4, hn, px, py)) return 0;
      p += 4 + (size_t)hn * 16;
    }
    return 1;
  }
  /* fallback: bbox containment */
  tdb_bbox bb;
  if (tdb_geom_bbox(blob, n, &bb) != TDB_OK) return 0;
  return px >= bb.minx && px <= bb.maxx && py >= bb.miny && py <= bb.maxy;
}
