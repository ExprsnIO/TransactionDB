/* test_geom.c — geometry encoding, WKT I/O, predicates, malformed input. */
#include "tdb_test.h"
#include "../src/value/tdb_geom.h"
#include "../src/common/tdb_buf.h"

#include <string.h>

static void roundtrip(const char *wkt) {
  tdb_buf b; tdb_buf_init(&b);
  const char *why = NULL;
  TDB_CHECK_EQ(tdb_geom_from_wkt(wkt, &b, &why), TDB_OK);
  TDB_CHECK(tdb_geom_valid(b.data, (int)b.len));
  tdb_buf t; tdb_buf_init(&t);
  TDB_CHECK_EQ(tdb_geom_to_wkt(b.data, (int)b.len, &t), TDB_OK);
  TDB_CHECK_STR((const char *)t.data, wkt);
  tdb_buf_free(&t); tdb_buf_free(&b);
}

static void test_wkt_roundtrip(void) {
  roundtrip("POINT(1 2)");
  roundtrip("POINT(-3.5 4.25)");
  roundtrip("LINESTRING(0 0, 1 1, 2 0)");
  roundtrip("POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))");
  roundtrip("POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))"); /* with hole */
}

static void test_bad_wkt(void) {
  tdb_buf b; tdb_buf_init(&b);
  const char *why = NULL;
  TDB_CHECK(tdb_geom_from_wkt("POINT(1)", &b, &why) != TDB_OK);          /* missing y */
  TDB_CHECK(tdb_geom_from_wkt("POINT(a b)", &b, &why) != TDB_OK);        /* non-numeric */
  TDB_CHECK(tdb_geom_from_wkt("CIRCLE(0 0 1)", &b, &why) != TDB_OK);     /* unknown type */
  TDB_CHECK(tdb_geom_from_wkt("POINT(1 2) junk", &b, &why) != TDB_OK);   /* trailing */
  TDB_CHECK(tdb_geom_from_wkt("LINESTRING(0 0", &b, &why) != TDB_OK);    /* unterminated */
  tdb_buf_free(&b);
}

static void test_malformed_blob(void) {
  /* truncated / nonsense blobs must be rejected, never crash */
  TDB_CHECK(!tdb_geom_valid((const uint8_t *)"", 0));
  uint8_t pt_short[5] = { TDB_GEOM_POINT, 1, 2, 3, 4 }; /* needs 17 bytes */
  TDB_CHECK(!tdb_geom_valid(pt_short, sizeof(pt_short)));
  uint8_t bogus_type[20]; memset(bogus_type, 0, sizeof(bogus_type)); bogus_type[0] = 99;
  TDB_CHECK(!tdb_geom_valid(bogus_type, sizeof(bogus_type)));
  /* a LINESTRING claiming a huge point count but short body */
  uint8_t ls[5] = { TDB_GEOM_LINESTRING, 0xFF, 0xFF, 0xFF, 0x7F };
  TDB_CHECK(!tdb_geom_valid(ls, sizeof(ls)));
}

static void mkgeom(tdb_buf *b, const char *wkt) {
  const char *why = NULL;
  tdb_buf_init(b);
  TDB_CHECK_EQ(tdb_geom_from_wkt(wkt, b, &why), TDB_OK);
}

static void test_predicates(void) {
  tdb_buf a, c, poly;
  mkgeom(&a, "POINT(0 0)");
  mkgeom(&c, "POINT(3 4)");
  mkgeom(&poly, "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))");

  double d = -1;
  TDB_CHECK_EQ(tdb_geom_distance(a.data, (int)a.len, c.data, (int)c.len, &d), TDB_OK);
  TDB_CHECK((int)(d + 0.5) == 5);     /* 3-4-5 */

  double x, y;
  TDB_CHECK_EQ(tdb_geom_point_xy(c.data, (int)c.len, &x, &y), TDB_OK);
  TDB_CHECK((int)(x + 0.5) == 3 && (int)(y + 0.5) == 4);
  TDB_CHECK(tdb_geom_point_xy(poly.data, (int)poly.len, &x, &y) != TDB_OK); /* not a point */

  TDB_CHECK(tdb_geom_contains_point(poly.data, (int)poly.len, 5, 5));
  TDB_CHECK(!tdb_geom_contains_point(poly.data, (int)poly.len, 50, 50));
  TDB_CHECK(!tdb_geom_contains_point(poly.data, (int)poly.len, -1, 5));

  tdb_bbox ba, bp;
  TDB_CHECK_EQ(tdb_geom_bbox(a.data, (int)a.len, &ba), TDB_OK);
  TDB_CHECK_EQ(tdb_geom_bbox(poly.data, (int)poly.len, &bp), TDB_OK);
  TDB_CHECK(bp.minx == 0 && bp.maxx == 10 && bp.miny == 0 && bp.maxy == 10);
  TDB_CHECK(tdb_bbox_intersects(&ba, &bp));   /* (0,0) touches the polygon bbox */

  /* hole containment: a point in the hole is NOT contained */
  tdb_buf donut; mkgeom(&donut, "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (3 3, 3 7, 7 7, 7 3, 3 3))");
  TDB_CHECK(tdb_geom_contains_point(donut.data, (int)donut.len, 1, 1));   /* in outer, outside hole */
  TDB_CHECK(!tdb_geom_contains_point(donut.data, (int)donut.len, 5, 5));  /* inside the hole */
  tdb_buf_free(&donut);

  tdb_buf_free(&a); tdb_buf_free(&c); tdb_buf_free(&poly);
}

static tdb_test_case cases[] = {
  {"wkt_roundtrip", test_wkt_roundtrip},
  {"bad_wkt", test_bad_wkt},
  {"malformed_blob", test_malformed_blob},
  {"predicates", test_predicates},
};
TDB_MAIN(cases)
