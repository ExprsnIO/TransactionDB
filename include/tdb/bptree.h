#ifndef TDB_BPTREE_H
#define TDB_BPTREE_H

#include "tdb/types.h"
#include "tdb/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key comparison function: returns <0, 0, >0 */
typedef int (*tdb_key_cmp_fn)(const void *a, const void *b, size_t key_size);

/* Default memcmp-based comparator */
int tdb_key_cmp_default(const void *a, const void *b, size_t key_size);

/*
 * B+Tree: all data in leaf nodes, internal nodes hold keys + child pointers.
 * Leaf nodes form a doubly-linked list for range scans.
 *
 * Node layout (in a page):
 *   [BPNodeHeader][keys...][children/rids...][prev_leaf|next_leaf (leaf only)]
 */

typedef struct {
    tdb_page_id_t    root_page;
    uint16_t         key_size;
    uint16_t         order;          /* max keys per node (computed from page size) */
    bool             unique;
    tdb_key_cmp_fn   cmp;
    tdb_buffer_pool_t *pool;         /* buffer pool for page access */
} tdb_bptree_t;

/* B+Tree node header (stored at start of each page used by the tree) */
typedef struct {
    uint16_t      num_keys;
    uint16_t      flags;        /* TDB_PAGE_FLAG_LEAF or TDB_PAGE_FLAG_INTERNAL */
    tdb_page_id_t parent;
    tdb_page_id_t prev_leaf;    /* leaf only */
    tdb_page_id_t next_leaf;    /* leaf only */
} tdb_bpnode_header_t;

/* Create a new B+Tree */
tdb_status_t tdb_bptree_create(tdb_bptree_t *tree, tdb_buffer_pool_t *pool,
                                uint16_t key_size, bool unique, tdb_key_cmp_fn cmp);

/* Insert a key-rid pair */
tdb_status_t tdb_bptree_insert(tdb_bptree_t *tree, const void *key, tdb_rid_t rid);

/* Point search: find exact key, return rid */
tdb_status_t tdb_bptree_search(tdb_bptree_t *tree, const void *key, tdb_rid_t *rid_out);

/* Delete a key */
tdb_status_t tdb_bptree_delete(tdb_bptree_t *tree, const void *key);

/* Range scan iterator */
typedef struct {
    tdb_bptree_t  *tree;
    tdb_page_id_t  current_page;
    uint16_t       current_index;
    const void    *high_key;     /* NULL = unbounded */
    bool           inclusive;
    bool           exhausted;
} tdb_bptree_iter_t;

tdb_status_t tdb_bptree_range_start(tdb_bptree_t *tree, const void *low_key,
                                     const void *high_key, bool inclusive,
                                     tdb_bptree_iter_t *iter);
tdb_status_t tdb_bptree_range_next(tdb_bptree_iter_t *iter,
                                    void *key_out, tdb_rid_t *rid_out);
void         tdb_bptree_range_close(tdb_bptree_iter_t *iter);

#ifdef __cplusplus
}
#endif

#endif /* TDB_BPTREE_H */
