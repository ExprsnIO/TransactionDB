/*
 * B+Tree index -- in-memory implementation.
 *
 * Every node is a malloc'd bpnode_t.  The tdb_bptree_t.root_page field
 * stores a pointer to the root node cast to tdb_page_id_t.  The buffer
 * pool pointer (tree->pool) is ignored; that integration comes later.
 *
 * Leaf nodes carry a doubly-linked list (prev / next) so the range
 * iterator can walk forwards efficiently.
 */

#include "tdb/bptree.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal node representation                                      */
/* ------------------------------------------------------------------ */

typedef struct bpnode bpnode_t;

struct bpnode {
    bool        is_leaf;
    uint16_t    num_keys;

    /*
     * keys:  num_keys slots, each of tree->key_size bytes.
     *        Stored as a flat byte array; key i starts at
     *        keys + i * key_size.
     */
    uint8_t    *keys;

    /* Internal node only: children[0..num_keys] */
    bpnode_t  **children;

    /* Leaf node only */
    tdb_rid_t  *rids;        /* rids[0..num_keys-1] */
    bpnode_t   *prev;
    bpnode_t   *next;

    /* Parent -- used for split propagation and rebalancing. */
    bpnode_t   *parent;
};

/* ------------------------------------------------------------------ */
/*  Helpers: pointer <-> page_id casts                                */
/* ------------------------------------------------------------------ */

static inline bpnode_t *page_to_node(tdb_page_id_t p)
{
    return (bpnode_t *)(uintptr_t)p;
}

static inline tdb_page_id_t node_to_page(bpnode_t *n)
{
    return (tdb_page_id_t)(uintptr_t)n;
}

/* ------------------------------------------------------------------ */
/*  Key helpers                                                       */
/* ------------------------------------------------------------------ */

static inline void *key_at(const bpnode_t *node, uint16_t idx,
                           uint16_t key_size)
{
    return node->keys + (size_t)idx * key_size;
}

static inline int key_cmp(const tdb_bptree_t *tree,
                          const void *a, const void *b)
{
    return tree->cmp(a, b, tree->key_size);
}

/* ------------------------------------------------------------------ */
/*  Default comparator                                                */
/* ------------------------------------------------------------------ */

int tdb_key_cmp_default(const void *a, const void *b, size_t key_size)
{
    return memcmp(a, b, key_size);
}

/* ------------------------------------------------------------------ */
/*  Node allocation / deallocation                                    */
/* ------------------------------------------------------------------ */

static bpnode_t *alloc_node(const tdb_bptree_t *tree, bool is_leaf)
{
    bpnode_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;

    n->is_leaf  = is_leaf;
    n->num_keys = 0;

    /* Allocate key array: order slots (max keys that fit before split). */
    n->keys = malloc((size_t)tree->order * tree->key_size);
    if (!n->keys) { free(n); return NULL; }

    if (is_leaf) {
        n->rids = malloc((size_t)tree->order * sizeof(tdb_rid_t));
        if (!n->rids) { free(n->keys); free(n); return NULL; }
        n->children = NULL;
        n->prev     = NULL;
        n->next     = NULL;
    } else {
        /* order keys -> order+1 children */
        n->children = calloc((size_t)tree->order + 1, sizeof(bpnode_t *));
        if (!n->children) { free(n->keys); free(n); return NULL; }
        n->rids = NULL;
    }
    n->parent = NULL;
    return n;
}

static void free_node(bpnode_t *n)
{
    if (!n) return;
    free(n->keys);
    free(n->children);
    free(n->rids);
    free(n);
}

/* ------------------------------------------------------------------ */
/*  Binary search within a node                                       */
/* ------------------------------------------------------------------ */

/*
 * Return the index of the first key >= search_key.
 * If all keys < search_key, returns num_keys.
 */
static uint16_t lower_bound(const tdb_bptree_t *tree,
                            const bpnode_t *node, const void *search_key)
{
    uint16_t lo = 0, hi = node->num_keys;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (key_cmp(tree, key_at(node, mid, tree->key_size), search_key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/*
 * Return the child index to follow when descending for 'key'.
 * child[i] covers keys[i-1] <= k < keys[i].
 */
static uint16_t find_child_index(const tdb_bptree_t *tree,
                                 const bpnode_t *node, const void *key)
{
    uint16_t lo = 0, hi = node->num_keys;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (key_cmp(tree, key_at(node, mid, tree->key_size), key) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ------------------------------------------------------------------ */
/*  Root-to-leaf traversal                                            */
/* ------------------------------------------------------------------ */

static bpnode_t *find_leaf(const tdb_bptree_t *tree, const void *key)
{
    bpnode_t *cur = page_to_node(tree->root_page);
    while (cur && !cur->is_leaf) {
        uint16_t idx = find_child_index(tree, cur, key);
        cur = cur->children[idx];
    }
    return cur;
}

/* ------------------------------------------------------------------ */
/*  tdb_bptree_create                                                 */
/* ------------------------------------------------------------------ */

tdb_status_t tdb_bptree_create(tdb_bptree_t *tree, tdb_buffer_pool_t *pool,
                                uint16_t key_size, bool unique,
                                tdb_key_cmp_fn cmp)
{
    if (!tree || key_size == 0)
        return TDB_ERR_INVALID_ARG;

    tree->pool     = pool;   /* stored but unused for now */
    tree->key_size = key_size;
    tree->unique   = unique;
    tree->cmp      = cmp ? cmp : tdb_key_cmp_default;

    /*
     * Compute order: maximum number of keys per node.
     * Use a fixed cap of 128 for the in-memory version.
     */
    tree->order = 128;

    /* Allocate empty root (a leaf). */
    bpnode_t *root = alloc_node(tree, true);
    if (!root) return TDB_ERR_NOMEM;

    tree->root_page = node_to_page(root);
    return TDB_OK;
}

/* ------------------------------------------------------------------ */
/*  Insertion helpers                                                  */
/* ------------------------------------------------------------------ */

/* Insert key+rid into leaf at position pos, shifting right. */
static void leaf_insert_at(bpnode_t *leaf, uint16_t pos,
                           const void *key, tdb_rid_t rid,
                           uint16_t key_size)
{
    uint16_t n = leaf->num_keys;
    /* Shift keys and rids right by one from pos..n-1 */
    if (pos < n) {
        memmove(key_at(leaf, pos + 1, key_size),
                key_at(leaf, pos, key_size),
                (size_t)(n - pos) * key_size);
        memmove(&leaf->rids[pos + 1], &leaf->rids[pos],
                (size_t)(n - pos) * sizeof(tdb_rid_t));
    }
    memcpy(key_at(leaf, pos, key_size), key, key_size);
    leaf->rids[pos] = rid;
    leaf->num_keys++;
}

/* Insert key + right child into internal node at position pos. */
static void internal_insert_at(bpnode_t *node, uint16_t pos,
                               const void *key, bpnode_t *right_child,
                               uint16_t key_size)
{
    uint16_t n = node->num_keys;
    if (pos < n) {
        memmove(key_at(node, pos + 1, key_size),
                key_at(node, pos, key_size),
                (size_t)(n - pos) * key_size);
        memmove(&node->children[pos + 2],
                &node->children[pos + 1],
                (size_t)(n - pos) * sizeof(bpnode_t *));
    }
    memcpy(key_at(node, pos, key_size), key, key_size);
    node->children[pos + 1] = right_child;
    node->num_keys++;
}

/* Forward declaration. */
static tdb_status_t insert_into_parent(tdb_bptree_t *tree,
                                       bpnode_t *left, const void *key,
                                       bpnode_t *right);

/*
 * Split a leaf that is full (num_keys == order).
 * The key to insert has already been placed in the leaf (which has
 * order slots allocated, so it can hold exactly order keys at most).
 *
 * We create a new right sibling, move the upper half of the keys
 * there, and push the first key of the new sibling up to the parent.
 */
static tdb_status_t split_leaf(tdb_bptree_t *tree, bpnode_t *leaf)
{
    bpnode_t *new_leaf = alloc_node(tree, true);
    if (!new_leaf) return TDB_ERR_NOMEM;

    uint16_t total   = leaf->num_keys;
    uint16_t split   = total / 2;          /* keys 0..split-1 stay */
    uint16_t move    = total - split;       /* keys split..total-1 move */
    uint16_t ks      = tree->key_size;

    memcpy(new_leaf->keys, key_at(leaf, split, ks), (size_t)move * ks);
    memcpy(new_leaf->rids, &leaf->rids[split], (size_t)move * sizeof(tdb_rid_t));
    new_leaf->num_keys = move;
    leaf->num_keys     = split;

    /* Maintain leaf linked list. */
    new_leaf->next = leaf->next;
    new_leaf->prev = leaf;
    if (leaf->next) leaf->next->prev = new_leaf;
    leaf->next = new_leaf;

    /* Parent linkage. */
    new_leaf->parent = leaf->parent;

    /* Push first key of new_leaf up to parent. */
    return insert_into_parent(tree, leaf, new_leaf->keys, new_leaf);
}

/*
 * Split an internal node.
 * The node already has order keys (was just over-stuffed by one key
 * insertion).  We split around the middle key: left keeps keys
 * 0..mid-1, right gets keys mid+1..n-1, and keys[mid] is pushed up.
 */
static tdb_status_t split_internal(tdb_bptree_t *tree, bpnode_t *node)
{
    uint16_t total = node->num_keys;
    uint16_t mid   = total / 2;
    uint16_t ks    = tree->key_size;

    bpnode_t *new_node = alloc_node(tree, false);
    if (!new_node) return TDB_ERR_NOMEM;

    /* Number of keys going to the new (right) node. */
    uint16_t right_keys = total - mid - 1;

    /* Copy keys[mid+1..total-1] and children[mid+1..total] to new_node. */
    memcpy(new_node->keys,
           key_at(node, mid + 1, ks),
           (size_t)right_keys * ks);
    memcpy(new_node->children,
           &node->children[mid + 1],
           (size_t)(right_keys + 1) * sizeof(bpnode_t *));
    new_node->num_keys = right_keys;

    /* Update parent pointers for moved children. */
    for (uint16_t i = 0; i <= right_keys; i++)
        if (new_node->children[i])
            new_node->children[i]->parent = new_node;

    /* Save the middle key before truncating. */
    uint8_t *mid_key = malloc(ks);
    if (!mid_key) { free_node(new_node); return TDB_ERR_NOMEM; }
    memcpy(mid_key, key_at(node, mid, ks), ks);

    node->num_keys = mid;   /* left keeps keys 0..mid-1 */

    new_node->parent = node->parent;

    tdb_status_t rc = insert_into_parent(tree, node, mid_key, new_node);
    free(mid_key);
    return rc;
}

/*
 * Insert a key that separates 'left' and 'right' into their parent.
 * If left has no parent (it is the root), create a new root.
 */
static tdb_status_t insert_into_parent(tdb_bptree_t *tree,
                                       bpnode_t *left, const void *key,
                                       bpnode_t *right)
{
    bpnode_t *parent = left->parent;

    /* Case 1: left is the root -- grow the tree upward. */
    if (!parent) {
        bpnode_t *new_root = alloc_node(tree, false);
        if (!new_root) return TDB_ERR_NOMEM;

        memcpy(key_at(new_root, 0, tree->key_size), key, tree->key_size);
        new_root->children[0] = left;
        new_root->children[1] = right;
        new_root->num_keys    = 1;

        left->parent  = new_root;
        right->parent = new_root;

        tree->root_page = node_to_page(new_root);
        return TDB_OK;
    }

    /* Find position of 'left' in parent->children. */
    uint16_t idx = 0;
    while (idx <= parent->num_keys && parent->children[idx] != left)
        idx++;

    /* Insert key at idx, right child at idx+1. */
    internal_insert_at(parent, idx, key, right, tree->key_size);
    right->parent = parent;

    /* If parent overflowed, split it. */
    if (parent->num_keys >= tree->order)
        return split_internal(tree, parent);

    return TDB_OK;
}

/* ------------------------------------------------------------------ */
/*  tdb_bptree_insert                                                 */
/* ------------------------------------------------------------------ */

tdb_status_t tdb_bptree_insert(tdb_bptree_t *tree, const void *key,
                                tdb_rid_t rid)
{
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    bpnode_t *leaf = find_leaf(tree, key);
    if (!leaf) return TDB_ERR_INTERNAL;

    /* Binary search for insertion point. */
    uint16_t pos = lower_bound(tree, leaf, key);

    /* Duplicate check for unique indexes. */
    if (tree->unique && pos < leaf->num_keys &&
        key_cmp(tree, key_at(leaf, pos, tree->key_size), key) == 0) {
        return TDB_ERR_EXISTS;
    }

    /* Insert into leaf. */
    leaf_insert_at(leaf, pos, key, rid, tree->key_size);

    /* Split if the leaf is over capacity. */
    if (leaf->num_keys >= tree->order)
        return split_leaf(tree, leaf);

    return TDB_OK;
}

/* ------------------------------------------------------------------ */
/*  tdb_bptree_search                                                 */
/* ------------------------------------------------------------------ */

tdb_status_t tdb_bptree_search(tdb_bptree_t *tree, const void *key,
                                tdb_rid_t *rid_out)
{
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    bpnode_t *leaf = find_leaf(tree, key);
    if (!leaf) return TDB_ERR_NOT_FOUND;

    uint16_t pos = lower_bound(tree, leaf, key);
    if (pos < leaf->num_keys &&
        key_cmp(tree, key_at(leaf, pos, tree->key_size), key) == 0) {
        if (rid_out)
            *rid_out = leaf->rids[pos];
        return TDB_OK;
    }
    return TDB_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Deletion helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Remove key (and rid) at position pos from a leaf.
 */
static void leaf_remove_at(bpnode_t *leaf, uint16_t pos, uint16_t key_size)
{
    uint16_t n = leaf->num_keys;
    if (pos + 1 < n) {
        memmove(key_at(leaf, pos, key_size),
                key_at(leaf, pos + 1, key_size),
                (size_t)(n - pos - 1) * key_size);
        memmove(&leaf->rids[pos], &leaf->rids[pos + 1],
                (size_t)(n - pos - 1) * sizeof(tdb_rid_t));
    }
    leaf->num_keys--;
}

/*
 * Remove key at position pos and the child at pos+1 from an internal node.
 */
static void internal_remove_at(bpnode_t *node, uint16_t pos,
                               uint16_t key_size)
{
    uint16_t n = node->num_keys;
    if (pos + 1 < n) {
        memmove(key_at(node, pos, key_size),
                key_at(node, pos + 1, key_size),
                (size_t)(n - pos - 1) * key_size);
    }
    if (pos + 2 <= n) {
        memmove(&node->children[pos + 1],
                &node->children[pos + 2],
                (size_t)(n - pos - 1) * sizeof(bpnode_t *));
    }
    node->num_keys--;
}

/* Find the index of 'child' in parent->children[]. Returns num_keys+1 on fail. */
static uint16_t child_index(const bpnode_t *parent, const bpnode_t *child)
{
    for (uint16_t i = 0; i <= parent->num_keys; i++)
        if (parent->children[i] == child)
            return i;
    return (uint16_t)(parent->num_keys + 1);
}

/* Forward declaration. */
static tdb_status_t fix_internal(tdb_bptree_t *tree, bpnode_t *node);

/*
 * After removing a key from a leaf, fix underflow if needed.
 * Minimum keys in a leaf = ceil(order/2) - 1, but for the root leaf
 * there is no minimum (it can even be empty).
 */
static tdb_status_t fix_leaf(tdb_bptree_t *tree, bpnode_t *leaf)
{
    /* Root leaf has no minimum. */
    if (!leaf->parent) return TDB_OK;

    uint16_t min_keys = (tree->order - 1) / 2;
    if (leaf->num_keys >= min_keys) return TDB_OK;

    bpnode_t *parent = leaf->parent;
    uint16_t ci      = child_index(parent, leaf);
    uint16_t ks      = tree->key_size;

    /* Try borrowing from left sibling. */
    if (ci > 0) {
        bpnode_t *left_sib = parent->children[ci - 1];
        if (left_sib->num_keys > min_keys) {
            /* Move the last key/rid of left_sib to front of leaf. */
            leaf_insert_at(leaf, 0,
                           key_at(left_sib, left_sib->num_keys - 1, ks),
                           left_sib->rids[left_sib->num_keys - 1], ks);
            left_sib->num_keys--;
            /* Update separator in parent: parent key[ci-1] = leaf key[0]. */
            memcpy(key_at(parent, ci - 1, ks), key_at(leaf, 0, ks), ks);
            return TDB_OK;
        }
    }

    /* Try borrowing from right sibling. */
    if (ci < parent->num_keys) {
        bpnode_t *right_sib = parent->children[ci + 1];
        if (right_sib->num_keys > min_keys) {
            /* Move the first key/rid of right_sib to end of leaf. */
            leaf_insert_at(leaf, leaf->num_keys,
                           key_at(right_sib, 0, ks),
                           right_sib->rids[0], ks);
            leaf_remove_at(right_sib, 0, ks);
            /* Update separator: parent key[ci] = right_sib key[0]. */
            memcpy(key_at(parent, ci, ks), key_at(right_sib, 0, ks), ks);
            return TDB_OK;
        }
    }

    /*
     * Merge.  Prefer merging with the left sibling so we always merge
     * a right node into a left node.
     */
    bpnode_t *merge_into;
    bpnode_t *merge_from;
    uint16_t  sep_idx;    /* index of separator key in parent */

    if (ci > 0) {
        merge_into = parent->children[ci - 1];
        merge_from = leaf;
        sep_idx    = ci - 1;
    } else {
        merge_into = leaf;
        merge_from = parent->children[ci + 1];
        sep_idx    = ci;
    }

    /* Append all of merge_from to merge_into. */
    memcpy(key_at(merge_into, merge_into->num_keys, ks),
           merge_from->keys,
           (size_t)merge_from->num_keys * ks);
    memcpy(&merge_into->rids[merge_into->num_keys],
           merge_from->rids,
           (size_t)merge_from->num_keys * sizeof(tdb_rid_t));
    merge_into->num_keys = (uint16_t)(merge_into->num_keys + merge_from->num_keys);

    /* Fix linked list. */
    merge_into->next = merge_from->next;
    if (merge_from->next)
        merge_from->next->prev = merge_into;

    /* Remove separator and merge_from child from parent. */
    internal_remove_at(parent, sep_idx, ks);
    free_node(merge_from);

    /* If parent is now the root and empty, shrink the tree. */
    if (!parent->parent && parent->num_keys == 0) {
        tree->root_page       = node_to_page(merge_into);
        merge_into->parent    = NULL;
        free_node(parent);
        return TDB_OK;
    }

    return fix_internal(tree, parent);
}

/*
 * Fix an internal node after a child merge reduced its key count.
 */
static tdb_status_t fix_internal(tdb_bptree_t *tree, bpnode_t *node)
{
    if (!node->parent) return TDB_OK;  /* root has no minimum */

    uint16_t min_keys = (tree->order - 1) / 2;
    if (node->num_keys >= min_keys) return TDB_OK;

    bpnode_t *parent = node->parent;
    uint16_t ci      = child_index(parent, node);
    uint16_t ks      = tree->key_size;

    /* Try borrowing from left sibling. */
    if (ci > 0) {
        bpnode_t *left_sib = parent->children[ci - 1];
        if (left_sib->num_keys > min_keys) {
            /*
             * Rotate right:
             *   - parent key[ci-1] becomes new first key in node
             *   - left_sib's last key goes up to parent[ci-1]
             *   - left_sib's last child becomes node's first child
             */
            /* Shift node keys/children right by 1. */
            memmove(key_at(node, 1, ks), key_at(node, 0, ks),
                    (size_t)node->num_keys * ks);
            memmove(&node->children[1], &node->children[0],
                    (size_t)(node->num_keys + 1) * sizeof(bpnode_t *));

            memcpy(key_at(node, 0, ks), key_at(parent, ci - 1, ks), ks);
            node->children[0] = left_sib->children[left_sib->num_keys];
            if (node->children[0])
                node->children[0]->parent = node;
            node->num_keys++;

            memcpy(key_at(parent, ci - 1, ks),
                   key_at(left_sib, left_sib->num_keys - 1, ks), ks);
            left_sib->num_keys--;
            return TDB_OK;
        }
    }

    /* Try borrowing from right sibling. */
    if (ci < parent->num_keys) {
        bpnode_t *right_sib = parent->children[ci + 1];
        if (right_sib->num_keys > min_keys) {
            /*
             * Rotate left:
             *   - parent key[ci] becomes last key in node
             *   - right_sib's first key goes up to parent[ci]
             *   - right_sib's first child becomes node's last child
             */
            memcpy(key_at(node, node->num_keys, ks),
                   key_at(parent, ci, ks), ks);
            node->children[node->num_keys + 1] = right_sib->children[0];
            if (node->children[node->num_keys + 1])
                node->children[node->num_keys + 1]->parent = node;
            node->num_keys++;

            memcpy(key_at(parent, ci, ks),
                   key_at(right_sib, 0, ks), ks);

            /* Remove first key+child[0] from right_sib. */
            memmove(right_sib->keys, key_at(right_sib, 1, ks),
                    (size_t)(right_sib->num_keys - 1) * ks);
            memmove(&right_sib->children[0], &right_sib->children[1],
                    (size_t)right_sib->num_keys * sizeof(bpnode_t *));
            right_sib->num_keys--;
            return TDB_OK;
        }
    }

    /*
     * Merge with a sibling.  Pull the separator down from the parent
     * to join the two internal nodes.
     */
    bpnode_t *merge_into;
    bpnode_t *merge_from;
    uint16_t  sep_idx;

    if (ci > 0) {
        merge_into = parent->children[ci - 1];
        merge_from = node;
        sep_idx    = ci - 1;
    } else {
        merge_into = node;
        merge_from = parent->children[ci + 1];
        sep_idx    = ci;
    }

    uint16_t left_n = merge_into->num_keys;

    /* Pull separator down. */
    memcpy(key_at(merge_into, left_n, ks),
           key_at(parent, sep_idx, ks), ks);
    left_n++;

    /* Copy merge_from keys and children. */
    memcpy(key_at(merge_into, left_n, ks),
           merge_from->keys,
           (size_t)merge_from->num_keys * ks);
    memcpy(&merge_into->children[left_n],
           merge_from->children,
           (size_t)(merge_from->num_keys + 1) * sizeof(bpnode_t *));

    /* Reparent moved children. */
    for (uint16_t i = 0; i <= merge_from->num_keys; i++)
        if (merge_from->children[i])
            merge_from->children[i]->parent = merge_into;

    merge_into->num_keys = (uint16_t)(left_n + merge_from->num_keys);

    /* Remove separator and merge_from from parent. */
    internal_remove_at(parent, sep_idx, ks);
    free_node(merge_from);

    /* If parent was the root and is now empty, shrink. */
    if (!parent->parent && parent->num_keys == 0) {
        tree->root_page    = node_to_page(merge_into);
        merge_into->parent = NULL;
        free_node(parent);
        return TDB_OK;
    }

    return fix_internal(tree, parent);
}

/* ------------------------------------------------------------------ */
/*  tdb_bptree_delete                                                 */
/* ------------------------------------------------------------------ */

tdb_status_t tdb_bptree_delete(tdb_bptree_t *tree, const void *key)
{
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    bpnode_t *leaf = find_leaf(tree, key);
    if (!leaf) return TDB_ERR_NOT_FOUND;

    uint16_t pos = lower_bound(tree, leaf, key);
    if (pos >= leaf->num_keys ||
        key_cmp(tree, key_at(leaf, pos, tree->key_size), key) != 0)
        return TDB_ERR_NOT_FOUND;

    leaf_remove_at(leaf, pos, tree->key_size);
    return fix_leaf(tree, leaf);
}

/* ------------------------------------------------------------------ */
/*  Range iteration                                                   */
/* ------------------------------------------------------------------ */

tdb_status_t tdb_bptree_range_start(tdb_bptree_t *tree,
                                     const void *low_key,
                                     const void *high_key,
                                     bool inclusive,
                                     tdb_bptree_iter_t *iter)
{
    if (!tree || !iter) return TDB_ERR_INVALID_ARG;

    iter->tree      = tree;
    iter->high_key  = high_key;
    iter->inclusive  = inclusive;
    iter->exhausted = false;

    if (!low_key) {
        /* Unbounded low: start at the leftmost leaf. */
        bpnode_t *cur = page_to_node(tree->root_page);
        while (cur && !cur->is_leaf)
            cur = cur->children[0];
        iter->current_page  = node_to_page(cur);
        iter->current_index = 0;
        if (!cur || cur->num_keys == 0)
            iter->exhausted = true;
    } else {
        bpnode_t *leaf = find_leaf(tree, low_key);
        if (!leaf) { iter->exhausted = true; return TDB_OK; }

        uint16_t pos = lower_bound(tree, leaf, low_key);

        /* Advance past current leaf if pos is at end. */
        while (leaf && pos >= leaf->num_keys) {
            leaf = leaf->next;
            pos  = 0;
        }
        if (!leaf) {
            iter->exhausted = true;
        } else {
            iter->current_page  = node_to_page(leaf);
            iter->current_index = pos;
        }
    }

    return TDB_OK;
}

tdb_status_t tdb_bptree_range_next(tdb_bptree_iter_t *iter,
                                    void *key_out, tdb_rid_t *rid_out)
{
    if (!iter) return TDB_ERR_INVALID_ARG;
    if (iter->exhausted) return TDB_ERR_NOT_FOUND;

    bpnode_t *leaf = page_to_node(iter->current_page);
    if (!leaf || iter->current_index >= leaf->num_keys) {
        iter->exhausted = true;
        return TDB_ERR_NOT_FOUND;
    }

    const tdb_bptree_t *tree = iter->tree;
    uint16_t ks  = tree->key_size;
    void    *cur = key_at(leaf, iter->current_index, ks);

    /* Check high bound. */
    if (iter->high_key) {
        int c = key_cmp(tree, cur, iter->high_key);
        if (iter->inclusive ? (c > 0) : (c >= 0)) {
            iter->exhausted = true;
            return TDB_ERR_NOT_FOUND;
        }
    }

    /* Yield current entry. */
    if (key_out)
        memcpy(key_out, cur, ks);
    if (rid_out)
        *rid_out = leaf->rids[iter->current_index];

    /* Advance. */
    iter->current_index++;
    if (iter->current_index >= leaf->num_keys) {
        leaf = leaf->next;
        if (leaf) {
            iter->current_page  = node_to_page(leaf);
            iter->current_index = 0;
            if (leaf->num_keys == 0) iter->exhausted = true;
        } else {
            iter->exhausted = true;
        }
    }

    return TDB_OK;
}

void tdb_bptree_range_close(tdb_bptree_iter_t *iter)
{
    if (!iter) return;
    iter->exhausted = true;
    iter->current_page = 0;
}
