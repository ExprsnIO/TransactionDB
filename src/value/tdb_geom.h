/*
** tdb_geom.h — geospatial geometry values.
**
** Geometries are stored as a compact little-endian binary blob and exchanged
** with SQL as WKT (Well-Known Text), e.g. 'POINT(1 2)',
** 'LINESTRING(0 0, 1 1, 2 0)', 'POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))'.
**
** Binary layout:
**   u8  type           (TDB_GEOM_POINT / LINESTRING / POLYGON)
**   ... type-specific body, all coordinates IEEE-754 double LE:
**     POINT      : x, y
**     LINESTRING : u32 npts, then npts * (x,y)
**     POLYGON    : u32 nrings, then per ring: u32 npts, npts * (x,y)
**
** A cached bounding box (minx,miny,maxx,maxy) is computed on demand from the
** body; it is what the (future) spatial index keys on.
*/
#ifndef TDB_GEOM_H
#define TDB_GEOM_H

#include "../common/tdb_internal.h"
#include "../common/tdb_buf.h"

typedef enum tdb_geom_type {
  TDB_GEOM_POINT = 1,
  TDB_GEOM_LINESTRING = 2,
  TDB_GEOM_POLYGON = 3
} tdb_geom_type;

typedef struct tdb_bbox {
  double minx, miny, maxx, maxy;
} tdb_bbox;

/* Parse WKT into the binary encoding appended to `out` (out is reset first).
** Returns TDB_OK or TDB_MISMATCH with *why set to a static reason string. */
int  tdb_geom_from_wkt(const char *wkt, tdb_buf *out, const char **why);

/* Format a binary geometry as WKT into `out` (reset first, NUL-terminated). */
int  tdb_geom_to_wkt(const uint8_t *blob, int n, tdb_buf *out);

/* Validate a binary geometry blob (structural). */
int  tdb_geom_valid(const uint8_t *blob, int n);

/* Read the geometry type byte (0 if invalid/empty). */
int  tdb_geom_type_of(const uint8_t *blob, int n);

/* Compute the bounding box; returns TDB_OK or an error for malformed input. */
int  tdb_geom_bbox(const uint8_t *blob, int n, tdb_bbox *out);

/* POINT accessors: extract X or Y (TDB_OK only for POINT geometries). */
int  tdb_geom_point_xy(const uint8_t *blob, int n, double *x, double *y);

/* Euclidean distance between two geometries, using their representative points
** (POINT: the point; LINESTRING/POLYGON: bounding-box center). Good enough for
** ST_Distance over planar coordinates; precise edge distance is a later step. */
int  tdb_geom_distance(const uint8_t *a, int an, const uint8_t *b, int bn, double *out);

/* True if bounding boxes intersect (the index-accelerated coarse predicate). */
int  tdb_bbox_intersects(const tdb_bbox *a, const tdb_bbox *b);

/* True if point (px,py) lies inside polygon `blob` (ray casting). For non-
** polygon geometries, falls back to bounding-box containment. */
int  tdb_geom_contains_point(const uint8_t *blob, int n, double px, double py);

#endif /* TDB_GEOM_H */
