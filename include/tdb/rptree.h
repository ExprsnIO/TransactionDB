#ifndef TDB_RPTREE_H
#define TDB_RPTREE_H

#include "tdb/types.h"
#include "tdb/buffer.h"
#include "tdb/rtree.h"  /* for tdb_mbr_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * R+Tree: non-overlapping partitions at internal nodes.
 * Better for point queries; entries may be duplicated across leaves.
 */

typedef struct {
    tdb_page_id_t    root_page;
    uint16_t         max_entries;
    uint16_t         min_entries;
    uint8_t          dimensions;
    tdb_buffer_pool_t *pool;
} tdb_rptree_t;

tdb_status_t tdb_rptree_create(tdb_rptree_t *tree, tdb_buffer_pool_t *pool, uint8_t dimensions);
tdb_status_t tdb_rptree_insert(tdb_rptree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid);
tdb_status_t tdb_rptree_delete(tdb_rptree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid);
tdb_status_t tdb_rptree_search(tdb_rptree_t *tree, const tdb_mbr_t *query,
                                tdb_rid_t *results, size_t max_results, size_t *found);

#ifdef __cplusplus
}
#endif

#endif /* TDB_RPTREE_H */
