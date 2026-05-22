#ifndef TDB_BTREE_H
#define TDB_BTREE_H

#include "tdb/types.h"
#include "tdb/buffer.h"
#include "tdb/bptree.h"  /* for tdb_key_cmp_fn */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * B-Tree: data in both internal and leaf nodes.
 * More space-efficient for unique key lookups, less efficient for range scans.
 */

typedef struct {
    tdb_page_id_t    root_page;
    uint16_t         key_size;
    uint16_t         order;
    bool             unique;
    tdb_key_cmp_fn   cmp;
    tdb_buffer_pool_t *pool;
} tdb_btree_t;

/* B-Tree node header */
typedef struct {
    uint16_t      num_keys;
    uint16_t      flags;
    tdb_page_id_t parent;
} tdb_btnode_header_t;

tdb_status_t tdb_btree_create(tdb_btree_t *tree, tdb_buffer_pool_t *pool,
                               uint16_t key_size, bool unique, tdb_key_cmp_fn cmp);
tdb_status_t tdb_btree_insert(tdb_btree_t *tree, const void *key, tdb_rid_t rid);
tdb_status_t tdb_btree_search(tdb_btree_t *tree, const void *key, tdb_rid_t *rid_out);
tdb_status_t tdb_btree_delete(tdb_btree_t *tree, const void *key);

#ifdef __cplusplus
}
#endif

#endif /* TDB_BTREE_H */
