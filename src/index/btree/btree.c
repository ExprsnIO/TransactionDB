#include "tdb/btree.h"
#include <stdlib.h>
#include <string.h>

/*
 * B-Tree: data (key + rid) in both internal and leaf nodes.
 * Each node stores keys and associated rids at every level.
 * Internal nodes also have child pointers between keys.
 *
 * Node layout (in-memory):
 *   num_keys keys, num_keys rids, (num_keys+1) children (internal only)
 */

#define BTREE_MAX_ORDER 128

typedef struct tdb_bt_node {
    bool     is_leaf;
    uint16_t num_keys;
    uint8_t  keys[BTREE_MAX_ORDER * 256]; /* key_size * MAX_ORDER */
    tdb_rid_t rids[BTREE_MAX_ORDER];
    struct tdb_bt_node *children[BTREE_MAX_ORDER + 1];
    struct tdb_bt_node *parent;
} tdb_bt_node_t;

static tdb_bt_node_t *bt_new_node(bool is_leaf) {
    tdb_bt_node_t *n = (tdb_bt_node_t *)calloc(1, sizeof(tdb_bt_node_t));
    if (n) {
        n->is_leaf = is_leaf;
        n->num_keys = 0;
        n->parent = NULL;
    }
    return n;
}

void tdb_btree_destroy(tdb_btree_t *tree) {
    if (!tree) return;
    /* Free all nodes recursively */
    tdb_bt_node_t *root = (tdb_bt_node_t *)(uintptr_t)tree->root_page;
    if (!root) return;

    /* Simple iterative free using a stack */
    tdb_bt_node_t *stack[4096];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        tdb_bt_node_t *n = stack[--top];
        if (!n->is_leaf) {
            for (int i = 0; i <= n->num_keys && top < 4096; i++) {
                if (n->children[i]) stack[top++] = n->children[i];
            }
        }
        free(n);
    }
    tree->root_page = 0;
}

static uint8_t *bt_key_at(tdb_bt_node_t *node, int idx, uint16_t key_size) {
    return node->keys + (idx * key_size);
}

static const uint8_t *bt_key_at_const(const tdb_bt_node_t *node, int idx, uint16_t key_size) {
    return node->keys + (idx * key_size);
}

/* Binary search: find index of first key >= search key */
static int bt_find_key(const tdb_bt_node_t *node, const void *key,
                       uint16_t key_size, tdb_key_cmp_fn cmp) {
    int lo = 0, hi = node->num_keys - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = cmp(bt_key_at_const(node, mid, key_size), key, key_size);
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return lo;
}

/* Split a full child[idx] of parent */
static tdb_status_t bt_split_child(tdb_btree_t *tree, tdb_bt_node_t *parent,
                                    int idx, tdb_bt_node_t *child) {
    uint16_t ks = tree->key_size;
    uint16_t order = tree->order;
    uint16_t mid = order / 2;

    tdb_bt_node_t *sibling = bt_new_node(child->is_leaf);
    if (!sibling) return TDB_ERR_NOMEM;

    /* Copy upper half of keys/rids to sibling */
    sibling->num_keys = child->num_keys - mid - 1;
    memcpy(sibling->keys, bt_key_at(child, mid + 1, ks), sibling->num_keys * ks);
    memcpy(sibling->rids, &child->rids[mid + 1], sibling->num_keys * sizeof(tdb_rid_t));

    if (!child->is_leaf) {
        /* Copy upper children */
        for (int i = 0; i <= sibling->num_keys; i++) {
            sibling->children[i] = child->children[mid + 1 + i];
            if (sibling->children[i]) sibling->children[i]->parent = sibling;
        }
    }

    /* The median key+rid will be pushed up to parent */
    uint8_t median_key[256];
    memcpy(median_key, bt_key_at(child, mid, ks), ks);
    tdb_rid_t median_rid = child->rids[mid];

    child->num_keys = mid;

    /* Shift parent's children and keys to make room */
    for (int i = parent->num_keys; i > idx; i--) {
        parent->children[i + 1] = parent->children[i];
    }
    parent->children[idx + 1] = sibling;
    sibling->parent = parent;

    for (int i = parent->num_keys - 1; i >= idx; i--) {
        memcpy(bt_key_at(parent, i + 1, ks), bt_key_at(parent, i, ks), ks);
        parent->rids[i + 1] = parent->rids[i];
    }
    memcpy(bt_key_at(parent, idx, ks), median_key, ks);
    parent->rids[idx] = median_rid;
    parent->num_keys++;

    return TDB_OK;
}

/* Insert into a non-full node */
static tdb_status_t bt_insert_nonfull(tdb_btree_t *tree, tdb_bt_node_t *node,
                                       const void *key, tdb_rid_t rid) {
    uint16_t ks = tree->key_size;
    int i = node->num_keys - 1;

    if (node->is_leaf) {
        /* Shift keys right to find insertion point */
        while (i >= 0 && tree->cmp(bt_key_at(node, i, ks), key, ks) > 0) {
            memcpy(bt_key_at(node, i + 1, ks), bt_key_at(node, i, ks), ks);
            node->rids[i + 1] = node->rids[i];
            i--;
        }
        memcpy(bt_key_at(node, i + 1, ks), key, ks);
        node->rids[i + 1] = rid;
        node->num_keys++;
        return TDB_OK;
    }

    /* Internal node: find child to descend into */
    while (i >= 0 && tree->cmp(bt_key_at(node, i, ks), key, ks) > 0) {
        i--;
    }

    /* Check if key matches an internal key (B-Tree stores data at all levels) */
    if (i >= 0 && tree->cmp(bt_key_at(node, i, ks), key, ks) == 0) {
        if (tree->unique) return TDB_ERR_EXISTS;
        /* For non-unique, we can update the rid or just go to child */
    }

    i++;
    tdb_bt_node_t *child = node->children[i];

    if (child->num_keys == tree->order - 1) {
        tdb_status_t rc = bt_split_child(tree, node, i, child);
        if (rc != TDB_OK) return rc;
        /* After split, decide which child to descend into */
        if (tree->cmp(bt_key_at(node, i, ks), key, ks) < 0) {
            i++;
        }
    }

    return bt_insert_nonfull(tree, node->children[i], key, rid);
}

tdb_status_t tdb_btree_create(tdb_btree_t *tree, tdb_buffer_pool_t *pool,
                               uint16_t key_size, bool unique, tdb_key_cmp_fn cmp) {
    if (!tree || key_size == 0) return TDB_ERR_INVALID_ARG;

    tree->key_size = key_size;
    tree->unique = unique;
    tree->cmp = cmp ? cmp : tdb_key_cmp_default;
    tree->pool = pool;

    /* Compute order: fit keys + rids + child pointers in reasonable memory */
    tree->order = BTREE_MAX_ORDER;
    if (key_size > 64) tree->order = 64;
    if (key_size > 128) tree->order = 32;

    /* Create root as empty leaf */
    tdb_bt_node_t *root = bt_new_node(true);
    if (!root) return TDB_ERR_NOMEM;

    /* Store root pointer as page_id (in-memory hack) */
    tree->root_page = (tdb_page_id_t)(uintptr_t)root;

    return TDB_OK;
}

tdb_status_t tdb_btree_insert(tdb_btree_t *tree, const void *key, tdb_rid_t rid) {
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    tdb_bt_node_t *root = (tdb_bt_node_t *)(uintptr_t)tree->root_page;

    /* Check for duplicate in unique mode */
    if (tree->unique) {
        tdb_rid_t tmp;
        if (tdb_btree_search(tree, key, &tmp) == TDB_OK) {
            return TDB_ERR_EXISTS;
        }
    }

    /* If root is full, grow the tree */
    if (root->num_keys == tree->order - 1) {
        tdb_bt_node_t *new_root = bt_new_node(false);
        if (!new_root) return TDB_ERR_NOMEM;
        new_root->children[0] = root;
        root->parent = new_root;

        tdb_status_t rc = bt_split_child(tree, new_root, 0, root);
        if (rc != TDB_OK) { free(new_root); return rc; }

        tree->root_page = (tdb_page_id_t)(uintptr_t)new_root;
        return bt_insert_nonfull(tree, new_root, key, rid);
    }

    return bt_insert_nonfull(tree, root, key, rid);
}

tdb_status_t tdb_btree_search(tdb_btree_t *tree, const void *key, tdb_rid_t *rid_out) {
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    tdb_bt_node_t *node = (tdb_bt_node_t *)(uintptr_t)tree->root_page;
    uint16_t ks = tree->key_size;

    while (node) {
        int idx = bt_find_key(node, key, ks, tree->cmp);

        /* Check if we found exact match at this node */
        if (idx < node->num_keys &&
            tree->cmp(bt_key_at_const(node, idx, ks), key, ks) == 0) {
            if (rid_out) *rid_out = node->rids[idx];
            return TDB_OK;
        }

        if (node->is_leaf) break;

        node = node->children[idx];
    }

    return TDB_ERR_NOT_FOUND;
}

/* Delete helper: remove key at index from a leaf */
static void bt_remove_from_leaf(tdb_bt_node_t *node, int idx, uint16_t ks) {
    for (int i = idx; i < node->num_keys - 1; i++) {
        memcpy(bt_key_at(node, i, ks), bt_key_at(node, i + 1, ks), ks);
        node->rids[i] = node->rids[i + 1];
    }
    node->num_keys--;
}

/* Find the predecessor (rightmost key in left subtree) */
static void bt_get_predecessor(tdb_bt_node_t *node, int idx, uint16_t ks,
                                void *key_out, tdb_rid_t *rid_out) {
    tdb_bt_node_t *cur = node->children[idx];
    while (!cur->is_leaf) {
        cur = cur->children[cur->num_keys];
    }
    memcpy(key_out, bt_key_at(cur, cur->num_keys - 1, ks), ks);
    *rid_out = cur->rids[cur->num_keys - 1];
}

/* Find the successor (leftmost key in right subtree) */
static void bt_get_successor(tdb_bt_node_t *node, int idx, uint16_t ks,
                              void *key_out, tdb_rid_t *rid_out) {
    tdb_bt_node_t *cur = node->children[idx + 1];
    while (!cur->is_leaf) {
        cur = cur->children[0];
    }
    memcpy(key_out, bt_key_at(cur, 0, ks), ks);
    *rid_out = cur->rids[0];
}

/* Merge children[idx+1] into children[idx], pulling key from parent */
static void bt_merge(tdb_bt_node_t *parent, int idx, uint16_t ks) {
    tdb_bt_node_t *left = parent->children[idx];
    tdb_bt_node_t *right = parent->children[idx + 1];

    /* Pull down parent key */
    memcpy(bt_key_at(left, left->num_keys, ks),
           bt_key_at(parent, idx, ks), ks);
    left->rids[left->num_keys] = parent->rids[idx];
    left->num_keys++;

    /* Copy right's keys into left */
    memcpy(bt_key_at(left, left->num_keys, ks), right->keys, right->num_keys * ks);
    memcpy(&left->rids[left->num_keys], right->rids, right->num_keys * sizeof(tdb_rid_t));

    if (!left->is_leaf) {
        for (int i = 0; i <= right->num_keys; i++) {
            left->children[left->num_keys + i] = right->children[i];
            if (right->children[i]) right->children[i]->parent = left;
        }
    }
    left->num_keys += right->num_keys;

    /* Remove key[idx] and children[idx+1] from parent */
    for (int i = idx; i < parent->num_keys - 1; i++) {
        memcpy(bt_key_at(parent, i, ks), bt_key_at(parent, i + 1, ks), ks);
        parent->rids[i] = parent->rids[i + 1];
    }
    for (int i = idx + 1; i < parent->num_keys; i++) {
        parent->children[i] = parent->children[i + 1];
    }
    parent->num_keys--;

    free(right);
}

/* Ensure children[idx] has at least min_keys keys */
static void bt_fill(tdb_bt_node_t *node, int idx, uint16_t ks, uint16_t min_keys) {
    if (idx > 0 && node->children[idx - 1]->num_keys > min_keys) {
        /* Borrow from left sibling */
        tdb_bt_node_t *child = node->children[idx];
        tdb_bt_node_t *left = node->children[idx - 1];

        /* Shift child's keys right */
        for (int i = child->num_keys - 1; i >= 0; i--) {
            memcpy(bt_key_at(child, i + 1, ks), bt_key_at(child, i, ks), ks);
            child->rids[i + 1] = child->rids[i];
        }
        if (!child->is_leaf) {
            for (int i = child->num_keys; i >= 0; i--) {
                child->children[i + 1] = child->children[i];
            }
            child->children[0] = left->children[left->num_keys];
            if (child->children[0]) child->children[0]->parent = child;
        }

        /* Pull parent key down to child[0] */
        memcpy(bt_key_at(child, 0, ks), bt_key_at(node, idx - 1, ks), ks);
        child->rids[0] = node->rids[idx - 1];
        child->num_keys++;

        /* Move left sibling's last key up to parent */
        memcpy(bt_key_at(node, idx - 1, ks),
               bt_key_at(left, left->num_keys - 1, ks), ks);
        node->rids[idx - 1] = left->rids[left->num_keys - 1];
        left->num_keys--;
    } else if (idx < node->num_keys && node->children[idx + 1]->num_keys > min_keys) {
        /* Borrow from right sibling */
        tdb_bt_node_t *child = node->children[idx];
        tdb_bt_node_t *right = node->children[idx + 1];

        /* Append parent key to child */
        memcpy(bt_key_at(child, child->num_keys, ks), bt_key_at(node, idx, ks), ks);
        child->rids[child->num_keys] = node->rids[idx];
        if (!child->is_leaf) {
            child->children[child->num_keys + 1] = right->children[0];
            if (child->children[child->num_keys + 1])
                child->children[child->num_keys + 1]->parent = child;
        }
        child->num_keys++;

        /* Move right sibling's first key up to parent */
        memcpy(bt_key_at(node, idx, ks), bt_key_at(right, 0, ks), ks);
        node->rids[idx] = right->rids[0];

        /* Shift right sibling left */
        for (int i = 0; i < right->num_keys - 1; i++) {
            memcpy(bt_key_at(right, i, ks), bt_key_at(right, i + 1, ks), ks);
            right->rids[i] = right->rids[i + 1];
        }
        if (!right->is_leaf) {
            for (int i = 0; i < right->num_keys; i++) {
                right->children[i] = right->children[i + 1];
            }
        }
        right->num_keys--;
    } else {
        /* Merge with a sibling */
        if (idx < node->num_keys) {
            bt_merge(node, idx, ks);
        } else {
            bt_merge(node, idx - 1, ks);
        }
    }
}

static tdb_status_t bt_delete_internal(tdb_btree_t *tree, tdb_bt_node_t *node, const void *key);

static tdb_status_t bt_delete_internal(tdb_btree_t *tree, tdb_bt_node_t *node, const void *key) {
    uint16_t ks = tree->key_size;
    uint16_t min_keys = (tree->order - 1) / 2;
    int idx = bt_find_key(node, key, ks, tree->cmp);

    if (idx < node->num_keys &&
        tree->cmp(bt_key_at_const(node, idx, ks), key, ks) == 0) {
        /* Found the key at this node */
        if (node->is_leaf) {
            bt_remove_from_leaf(node, idx, ks);
            return TDB_OK;
        }

        /* Internal node: replace with predecessor or successor */
        tdb_bt_node_t *left_child = node->children[idx];
        tdb_bt_node_t *right_child = node->children[idx + 1];

        if (left_child->num_keys > min_keys) {
            uint8_t pred_key[256];
            tdb_rid_t pred_rid;
            bt_get_predecessor(node, idx, ks, pred_key, &pred_rid);
            memcpy(bt_key_at(node, idx, ks), pred_key, ks);
            node->rids[idx] = pred_rid;
            return bt_delete_internal(tree, left_child, pred_key);
        } else if (right_child->num_keys > min_keys) {
            uint8_t succ_key[256];
            tdb_rid_t succ_rid;
            bt_get_successor(node, idx, ks, succ_key, &succ_rid);
            memcpy(bt_key_at(node, idx, ks), succ_key, ks);
            node->rids[idx] = succ_rid;
            return bt_delete_internal(tree, right_child, succ_key);
        } else {
            bt_merge(node, idx, ks);
            return bt_delete_internal(tree, left_child, key);
        }
    }

    if (node->is_leaf) return TDB_ERR_NOT_FOUND;

    bool last_child = (idx == node->num_keys);
    tdb_bt_node_t *child = node->children[idx];

    if (child->num_keys <= min_keys) {
        bt_fill(node, idx, ks, min_keys);
    }

    /* After fill, idx may have changed due to merge */
    if (last_child && idx > node->num_keys) {
        return bt_delete_internal(tree, node->children[idx - 1], key);
    }
    return bt_delete_internal(tree, node->children[idx], key);
}

tdb_status_t tdb_btree_delete(tdb_btree_t *tree, const void *key) {
    if (!tree || !key) return TDB_ERR_INVALID_ARG;

    tdb_bt_node_t *root = (tdb_bt_node_t *)(uintptr_t)tree->root_page;
    tdb_status_t rc = bt_delete_internal(tree, root, key);
    if (rc != TDB_OK) return rc;

    /* If root has no keys and has a child, shrink tree */
    if (root->num_keys == 0 && !root->is_leaf) {
        tdb_bt_node_t *new_root = root->children[0];
        if (new_root) new_root->parent = NULL;
        tree->root_page = (tdb_page_id_t)(uintptr_t)new_root;
        free(root);
    }

    return TDB_OK;
}
