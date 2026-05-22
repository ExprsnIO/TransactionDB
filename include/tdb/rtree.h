#ifndef TDB_RTREE_H
#define TDB_RTREE_H

#include "tdb/types.h"
#include "tdb/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TDB_RTREE_MAX_DIMS 4

/* Minimum bounding rectangle */
typedef struct {
    double min[TDB_RTREE_MAX_DIMS];
    double max[TDB_RTREE_MAX_DIMS];
} tdb_mbr_t;

/* R-Tree descriptor */
typedef struct {
    tdb_page_id_t    root_page;
    uint16_t         max_entries;  /* M: max entries per node */
    uint16_t         min_entries;  /* m: min entries per node (typically M/2) */
    uint8_t          dimensions;
    tdb_buffer_pool_t *pool;
} tdb_rtree_t;

/* R-Tree node header */
typedef struct {
    uint16_t      num_entries;
    uint16_t      flags;   /* leaf or internal */
    tdb_page_id_t parent;
} tdb_rtnode_header_t;

/* MBR utility functions */
double tdb_mbr_area(const tdb_mbr_t *mbr, uint8_t dims);
double tdb_mbr_overlap_area(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims);
double tdb_mbr_enlargement(const tdb_mbr_t *mbr, const tdb_mbr_t *add, uint8_t dims);
void   tdb_mbr_combine(tdb_mbr_t *out, const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims);
bool   tdb_mbr_contains(const tdb_mbr_t *outer, const tdb_mbr_t *inner, uint8_t dims);
bool   tdb_mbr_overlaps(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims);
double tdb_mbr_distance(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims);
void   tdb_mbr_from_point(tdb_mbr_t *mbr, const double *coords, uint8_t dims);

/* R-Tree operations */
tdb_status_t tdb_rtree_create(tdb_rtree_t *tree, tdb_buffer_pool_t *pool, uint8_t dimensions);
tdb_status_t tdb_rtree_insert(tdb_rtree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid);
tdb_status_t tdb_rtree_delete(tdb_rtree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid);

/* Search: find all entries whose MBR overlaps the query */
tdb_status_t tdb_rtree_search(tdb_rtree_t *tree, const tdb_mbr_t *query,
                               tdb_rid_t *results, size_t max_results, size_t *found);

/* Nearest-neighbor search */
tdb_status_t tdb_rtree_nearest(tdb_rtree_t *tree, const tdb_mbr_t *point,
                                size_t k, tdb_rid_t *results, double *distances,
                                size_t *found);

#ifdef __cplusplus
}
#endif

#endif /* TDB_RTREE_H */
