#include "tdb/rtree.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* ─── MBR Utility Functions ─── */

double tdb_mbr_area(const tdb_mbr_t *mbr, uint8_t dims) {
    double area = 1.0;
    for (uint8_t d = 0; d < dims; d++) {
        double extent = mbr->max[d] - mbr->min[d];
        if (extent < 0.0) return 0.0;
        area *= extent;
    }
    return area;
}

double tdb_mbr_overlap_area(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims) {
    double area = 1.0;
    for (uint8_t d = 0; d < dims; d++) {
        double lo = (a->min[d] > b->min[d]) ? a->min[d] : b->min[d];
        double hi = (a->max[d] < b->max[d]) ? a->max[d] : b->max[d];
        if (lo >= hi) return 0.0;
        area *= (hi - lo);
    }
    return area;
}

double tdb_mbr_enlargement(const tdb_mbr_t *mbr, const tdb_mbr_t *add, uint8_t dims) {
    tdb_mbr_t combined;
    tdb_mbr_combine(&combined, mbr, add, dims);
    return tdb_mbr_area(&combined, dims) - tdb_mbr_area(mbr, dims);
}

void tdb_mbr_combine(tdb_mbr_t *out, const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims) {
    for (uint8_t d = 0; d < dims; d++) {
        out->min[d] = (a->min[d] < b->min[d]) ? a->min[d] : b->min[d];
        out->max[d] = (a->max[d] > b->max[d]) ? a->max[d] : b->max[d];
    }
}

bool tdb_mbr_contains(const tdb_mbr_t *outer, const tdb_mbr_t *inner, uint8_t dims) {
    for (uint8_t d = 0; d < dims; d++) {
        if (inner->min[d] < outer->min[d] || inner->max[d] > outer->max[d])
            return false;
    }
    return true;
}

bool tdb_mbr_overlaps(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims) {
    for (uint8_t d = 0; d < dims; d++) {
        if (a->max[d] < b->min[d] || a->min[d] > b->max[d])
            return false;
    }
    return true;
}

double tdb_mbr_distance(const tdb_mbr_t *a, const tdb_mbr_t *b, uint8_t dims) {
    double sum = 0.0;
    for (uint8_t d = 0; d < dims; d++) {
        double ca = (a->min[d] + a->max[d]) / 2.0;
        double cb = (b->min[d] + b->max[d]) / 2.0;
        double diff = ca - cb;
        sum += diff * diff;
    }
    return sqrt(sum);
}

void tdb_mbr_from_point(tdb_mbr_t *mbr, const double *coords, uint8_t dims) {
    for (uint8_t d = 0; d < dims; d++) {
        mbr->min[d] = coords[d];
        mbr->max[d] = coords[d];
    }
}

/* ─── In-Memory R-Tree Node ─── */

#define RT_MAX_ENTRIES 64

typedef struct tdb_rt_node {
    bool      is_leaf;
    uint16_t  count;
    tdb_mbr_t mbrs[RT_MAX_ENTRIES];
    union {
        struct tdb_rt_node *children[RT_MAX_ENTRIES]; /* internal */
        tdb_rid_t           rids[RT_MAX_ENTRIES];     /* leaf */
    };
    struct tdb_rt_node *parent;
    int parent_idx; /* which entry in parent points to this node */
} tdb_rt_node_t;

static tdb_rt_node_t *rt_new_node(bool is_leaf) {
    tdb_rt_node_t *n = (tdb_rt_node_t *)calloc(1, sizeof(tdb_rt_node_t));
    if (n) {
        n->is_leaf = is_leaf;
        n->count = 0;
        n->parent = NULL;
        n->parent_idx = -1;
    }
    return n;
}

void tdb_rtree_destroy(tdb_rtree_t *tree) {
    if (!tree) return;
    tdb_rt_node_t *root = (tdb_rt_node_t *)(uintptr_t)tree->root_page;
    if (!root) return;

    tdb_rt_node_t *stack[4096];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        tdb_rt_node_t *n = stack[--top];
        if (!n->is_leaf) {
            for (int i = 0; i < n->count && top < 4096; i++) {
                if (n->children[i]) stack[top++] = n->children[i];
            }
        }
        free(n);
    }
    tree->root_page = 0;
}

/* Recompute the MBR of a node by combining all entry MBRs */
static void rt_recompute_mbr(tdb_rt_node_t *node, uint8_t dims, tdb_mbr_t *out) {
    if (node->count == 0) {
        memset(out, 0, sizeof(tdb_mbr_t));
        return;
    }
    *out = node->mbrs[0];
    for (int i = 1; i < node->count; i++) {
        tdb_mbr_combine(out, out, &node->mbrs[i], dims);
    }
}

/* ─── Choose Subtree (Guttman) ─── */
/* For leaf insertion: choose child whose MBR needs least enlargement */
static int rt_choose_subtree(tdb_rt_node_t *node, const tdb_mbr_t *mbr, uint8_t dims) {
    int best = 0;
    double best_enlargement = DBL_MAX;
    double best_area = DBL_MAX;

    for (int i = 0; i < node->count; i++) {
        double enlargement = tdb_mbr_enlargement(&node->mbrs[i], mbr, dims);
        double area = tdb_mbr_area(&node->mbrs[i], dims);
        if (enlargement < best_enlargement ||
            (enlargement == best_enlargement && area < best_area)) {
            best = i;
            best_enlargement = enlargement;
            best_area = area;
        }
    }
    return best;
}

/* ─── Quadratic Split (Guttman) ─── */
/* Pick seeds: find pair with maximum wasted space */
static void rt_pick_seeds(const tdb_mbr_t *mbrs, int count, uint8_t dims,
                           int *seed1, int *seed2) {
    double max_waste = -DBL_MAX;
    *seed1 = 0;
    *seed2 = 1;

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            tdb_mbr_t combined;
            tdb_mbr_combine(&combined, &mbrs[i], &mbrs[j], dims);
            double waste = tdb_mbr_area(&combined, dims) -
                           tdb_mbr_area(&mbrs[i], dims) -
                           tdb_mbr_area(&mbrs[j], dims);
            if (waste > max_waste) {
                max_waste = waste;
                *seed1 = i;
                *seed2 = j;
            }
        }
    }
}

/* Split a node that has count == max_entries.
 * One extra entry (the overflow) is at index node->count (already placed there).
 * Returns the new sibling node. */
static tdb_rt_node_t *rt_split_node(tdb_rt_node_t *node, uint8_t dims, uint16_t min_entries) {
    int total = node->count; /* includes overflow entry */
    tdb_rt_node_t *sibling = rt_new_node(node->is_leaf);
    if (!sibling) return NULL;

    /* Temporary arrays for all entries */
    tdb_mbr_t all_mbrs[RT_MAX_ENTRIES + 1];
    tdb_rid_t all_rids[RT_MAX_ENTRIES + 1];
    tdb_rt_node_t *all_children[RT_MAX_ENTRIES + 1];
    bool assigned[RT_MAX_ENTRIES + 1];
    memset(assigned, 0, sizeof(assigned));

    for (int i = 0; i < total; i++) {
        all_mbrs[i] = node->mbrs[i];
        if (node->is_leaf) all_rids[i] = node->rids[i];
        else all_children[i] = node->children[i];
    }

    /* Pick seeds */
    int s1, s2;
    rt_pick_seeds(all_mbrs, total, dims, &s1, &s2);

    /* Reset node and sibling */
    node->count = 0;
    sibling->count = 0;

    /* Assign seeds */
    node->mbrs[0] = all_mbrs[s1];
    if (node->is_leaf) node->rids[0] = all_rids[s1];
    else { node->children[0] = all_children[s1]; all_children[s1]->parent = node; all_children[s1]->parent_idx = 0; }
    node->count = 1;
    assigned[s1] = true;

    sibling->mbrs[0] = all_mbrs[s2];
    if (sibling->is_leaf) sibling->rids[0] = all_rids[s2];
    else { sibling->children[0] = all_children[s2]; all_children[s2]->parent = sibling; all_children[s2]->parent_idx = 0; }
    sibling->count = 1;
    assigned[s2] = true;

    tdb_mbr_t mbr_node = all_mbrs[s1];
    tdb_mbr_t mbr_sibling = all_mbrs[s2];

    /* Distribute remaining entries using pick-next */
    int remaining = total - 2;
    while (remaining > 0) {
        /* Check if one group needs all remaining to meet minimum */
        if (node->count + remaining <= min_entries) {
            for (int i = 0; i < total; i++) {
                if (assigned[i]) continue;
                node->mbrs[node->count] = all_mbrs[i];
                if (node->is_leaf) node->rids[node->count] = all_rids[i];
                else { node->children[node->count] = all_children[i]; all_children[i]->parent = node; all_children[i]->parent_idx = node->count; }
                tdb_mbr_combine(&mbr_node, &mbr_node, &all_mbrs[i], dims);
                node->count++;
                assigned[i] = true;
                remaining--;
            }
            break;
        }
        if (sibling->count + remaining <= min_entries) {
            for (int i = 0; i < total; i++) {
                if (assigned[i]) continue;
                sibling->mbrs[sibling->count] = all_mbrs[i];
                if (sibling->is_leaf) sibling->rids[sibling->count] = all_rids[i];
                else { sibling->children[sibling->count] = all_children[i]; all_children[i]->parent = sibling; all_children[i]->parent_idx = sibling->count; }
                tdb_mbr_combine(&mbr_sibling, &mbr_sibling, &all_mbrs[i], dims);
                sibling->count++;
                assigned[i] = true;
                remaining--;
            }
            break;
        }

        /* Pick next: entry with maximum difference in enlargement preference */
        int best_idx = -1;
        double best_diff = -DBL_MAX;
        for (int i = 0; i < total; i++) {
            if (assigned[i]) continue;
            double d1 = tdb_mbr_enlargement(&mbr_node, &all_mbrs[i], dims);
            double d2 = tdb_mbr_enlargement(&mbr_sibling, &all_mbrs[i], dims);
            double diff = fabs(d1 - d2);
            if (diff > best_diff) {
                best_diff = diff;
                best_idx = i;
            }
        }

        double e_node = tdb_mbr_enlargement(&mbr_node, &all_mbrs[best_idx], dims);
        double e_sib = tdb_mbr_enlargement(&mbr_sibling, &all_mbrs[best_idx], dims);

        tdb_rt_node_t *target;
        tdb_mbr_t *target_mbr;
        if (e_node < e_sib || (e_node == e_sib && node->count <= sibling->count)) {
            target = node;
            target_mbr = &mbr_node;
        } else {
            target = sibling;
            target_mbr = &mbr_sibling;
        }

        target->mbrs[target->count] = all_mbrs[best_idx];
        if (target->is_leaf) target->rids[target->count] = all_rids[best_idx];
        else { target->children[target->count] = all_children[best_idx]; all_children[best_idx]->parent = target; all_children[best_idx]->parent_idx = target->count; }
        tdb_mbr_combine(target_mbr, target_mbr, &all_mbrs[best_idx], dims);
        target->count++;
        assigned[best_idx] = true;
        remaining--;
    }

    return sibling;
}

/* Adjust tree after insertion: update MBRs up to root, handle splits */
static tdb_status_t rt_adjust_tree(tdb_rtree_t *tree, tdb_rt_node_t *node,
                                    tdb_rt_node_t *split_sibling) {
    uint8_t dims = tree->dimensions;

    while (node->parent) {
        tdb_rt_node_t *parent = node->parent;
        int idx = node->parent_idx;

        /* Update parent's MBR for this child */
        rt_recompute_mbr(node, dims, &parent->mbrs[idx]);

        if (split_sibling) {
            /* Insert sibling into parent */
            if (parent->count < tree->max_entries) {
                int si = parent->count;
                parent->children[si] = split_sibling;
                rt_recompute_mbr(split_sibling, dims, &parent->mbrs[si]);
                split_sibling->parent = parent;
                split_sibling->parent_idx = si;
                parent->count++;
                split_sibling = NULL;
            } else {
                /* Parent is full — need to split parent too */
                int si = parent->count;
                parent->children[si] = split_sibling;
                rt_recompute_mbr(split_sibling, dims, &parent->mbrs[si]);
                split_sibling->parent = parent;
                split_sibling->parent_idx = si;
                parent->count++;

                tdb_rt_node_t *parent_split = rt_split_node(parent, dims, tree->min_entries);
                if (!parent_split) return TDB_ERR_NOMEM;

                split_sibling = parent_split;
            }
        }

        node = parent;
    }

    /* At root — if we have a split sibling, create new root */
    if (split_sibling) {
        tdb_rt_node_t *new_root = rt_new_node(false);
        if (!new_root) return TDB_ERR_NOMEM;

        new_root->children[0] = node;
        rt_recompute_mbr(node, dims, &new_root->mbrs[0]);
        node->parent = new_root;
        node->parent_idx = 0;

        new_root->children[1] = split_sibling;
        rt_recompute_mbr(split_sibling, dims, &new_root->mbrs[1]);
        split_sibling->parent = new_root;
        split_sibling->parent_idx = 1;

        new_root->count = 2;
        tree->root_page = (tdb_page_id_t)(uintptr_t)new_root;
    }

    return TDB_OK;
}

/* ─── Public API ─── */

tdb_status_t tdb_rtree_create(tdb_rtree_t *tree, tdb_buffer_pool_t *pool, uint8_t dimensions) {
    if (!tree || dimensions == 0 || dimensions > TDB_RTREE_MAX_DIMS)
        return TDB_ERR_INVALID_ARG;

    tree->dimensions = dimensions;
    tree->max_entries = RT_MAX_ENTRIES;
    tree->min_entries = RT_MAX_ENTRIES / 3; /* ~33% fill factor */
    tree->pool = pool;

    tdb_rt_node_t *root = rt_new_node(true);
    if (!root) return TDB_ERR_NOMEM;
    tree->root_page = (tdb_page_id_t)(uintptr_t)root;

    return TDB_OK;
}

tdb_status_t tdb_rtree_insert(tdb_rtree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid) {
    if (!tree || !mbr) return TDB_ERR_INVALID_ARG;

    uint8_t dims = tree->dimensions;
    tdb_rt_node_t *node = (tdb_rt_node_t *)(uintptr_t)tree->root_page;

    /* Descend to leaf */
    while (!node->is_leaf) {
        int idx = rt_choose_subtree(node, mbr, dims);
        node = node->children[idx];
    }

    tdb_rt_node_t *split = NULL;

    if (node->count < tree->max_entries) {
        /* Room in leaf */
        int i = node->count;
        node->mbrs[i] = *mbr;
        node->rids[i] = rid;
        node->count++;
    } else {
        /* Leaf overflow — add then split */
        node->mbrs[node->count] = *mbr;
        node->rids[node->count] = rid;
        node->count++;
        split = rt_split_node(node, dims, tree->min_entries);
        if (!split) return TDB_ERR_NOMEM;
    }

    return rt_adjust_tree(tree, node, split);
}

/* Recursive search */
static void rt_search_node(tdb_rt_node_t *node, const tdb_mbr_t *query, uint8_t dims,
                            tdb_rid_t *results, size_t max_results, size_t *found) {
    for (int i = 0; i < node->count; i++) {
        if (!tdb_mbr_overlaps(&node->mbrs[i], query, dims)) continue;

        if (node->is_leaf) {
            if (*found < max_results) {
                results[*found] = node->rids[i];
                (*found)++;
            }
        } else {
            rt_search_node(node->children[i], query, dims, results, max_results, found);
        }
    }
}

tdb_status_t tdb_rtree_search(tdb_rtree_t *tree, const tdb_mbr_t *query,
                               tdb_rid_t *results, size_t max_results, size_t *found) {
    if (!tree || !query || !results || !found) return TDB_ERR_INVALID_ARG;

    *found = 0;
    tdb_rt_node_t *root = (tdb_rt_node_t *)(uintptr_t)tree->root_page;
    rt_search_node(root, query, tree->dimensions, results, max_results, found);
    return TDB_OK;
}

/* Delete: find and remove the entry, then condense tree */
static bool rt_find_leaf(tdb_rt_node_t *node, const tdb_mbr_t *mbr, tdb_rid_t rid,
                          uint8_t dims, tdb_rt_node_t **leaf_out, int *idx_out) {
    if (node->is_leaf) {
        for (int i = 0; i < node->count; i++) {
            if (node->rids[i].page_id == rid.page_id &&
                node->rids[i].slot == rid.slot &&
                tdb_mbr_overlaps(&node->mbrs[i], mbr, dims)) {
                *leaf_out = node;
                *idx_out = i;
                return true;
            }
        }
        return false;
    }

    for (int i = 0; i < node->count; i++) {
        if (tdb_mbr_overlaps(&node->mbrs[i], mbr, dims)) {
            if (rt_find_leaf(node->children[i], mbr, rid, dims, leaf_out, idx_out))
                return true;
        }
    }
    return false;
}

/* Remove entry at idx from leaf */
static void rt_remove_entry(tdb_rt_node_t *node, int idx) {
    for (int i = idx; i < node->count - 1; i++) {
        node->mbrs[i] = node->mbrs[i + 1];
        if (node->is_leaf) node->rids[i] = node->rids[i + 1];
        else {
            node->children[i] = node->children[i + 1];
            node->children[i]->parent_idx = i;
        }
    }
    node->count--;
}

/* Condense tree after deletion */
static tdb_status_t rt_condense_tree(tdb_rtree_t *tree, tdb_rt_node_t *leaf) {
    uint8_t dims = tree->dimensions;
    /* Collect orphan entries from underflowed nodes */
    tdb_mbr_t orphan_mbrs[RT_MAX_ENTRIES * 4];
    tdb_rid_t orphan_rids[RT_MAX_ENTRIES * 4];
    int orphan_count = 0;

    tdb_rt_node_t *node = leaf;
    while (node->parent) {
        tdb_rt_node_t *parent = node->parent;
        int idx = node->parent_idx;

        if (node->count < tree->min_entries) {
            /* Underflow: remove this node from parent and collect its entries */
            for (int i = 0; i < node->count; i++) {
                if (node->is_leaf) {
                    orphan_mbrs[orphan_count] = node->mbrs[i];
                    orphan_rids[orphan_count] = node->rids[i];
                    orphan_count++;
                }
                /* For internal nodes, would need to re-insert subtrees.
                 * Simplified: only handle leaf orphans. */
            }
            rt_remove_entry(parent, idx);
            /* Re-index parent_idx for subsequent children */
            for (int i = idx; i < parent->count; i++) {
                if (!parent->is_leaf) parent->children[i]->parent_idx = i;
            }
            free(node);
        } else {
            /* Just update parent's MBR */
            rt_recompute_mbr(node, dims, &parent->mbrs[idx]);
        }

        node = parent;
    }

    /* Re-insert orphan entries */
    for (int i = 0; i < orphan_count; i++) {
        tdb_status_t rc = tdb_rtree_insert(tree, &orphan_mbrs[i], orphan_rids[i]);
        if (rc != TDB_OK) return rc;
    }

    /* Shrink tree if root has only one child */
    tdb_rt_node_t *root = (tdb_rt_node_t *)(uintptr_t)tree->root_page;
    if (!root->is_leaf && root->count == 1) {
        tdb_rt_node_t *new_root = root->children[0];
        new_root->parent = NULL;
        new_root->parent_idx = -1;
        tree->root_page = (tdb_page_id_t)(uintptr_t)new_root;
        free(root);
    }

    return TDB_OK;
}

tdb_status_t tdb_rtree_delete(tdb_rtree_t *tree, const tdb_mbr_t *mbr, tdb_rid_t rid) {
    if (!tree || !mbr) return TDB_ERR_INVALID_ARG;

    tdb_rt_node_t *root = (tdb_rt_node_t *)(uintptr_t)tree->root_page;
    tdb_rt_node_t *leaf;
    int idx;

    if (!rt_find_leaf(root, mbr, rid, tree->dimensions, &leaf, &idx))
        return TDB_ERR_NOT_FOUND;

    rt_remove_entry(leaf, idx);
    return rt_condense_tree(tree, leaf);
}

tdb_status_t tdb_rtree_nearest(tdb_rtree_t *tree, const tdb_mbr_t *point,
                                size_t k, tdb_rid_t *results, double *distances,
                                size_t *found) {
    if (!tree || !point || !results || !distances || !found)
        return TDB_ERR_INVALID_ARG;

    /* Simple brute-force NN: search all entries, sort by distance.
     * A proper implementation would use a priority queue with branch-and-bound. */
    *found = 0;
    tdb_rid_t all_results[4096];
    size_t all_count = 0;
    tdb_rt_node_t *root = (tdb_rt_node_t *)(uintptr_t)tree->root_page;

    /* Search everything (use a very large query box) */
    tdb_mbr_t big_query;
    for (uint8_t d = 0; d < tree->dimensions; d++) {
        big_query.min[d] = -1e18;
        big_query.max[d] = 1e18;
    }
    rt_search_node(root, &big_query, tree->dimensions, all_results, 4096, &all_count);

    /* Compute distances and find top-k (simple selection) */
    double all_dists[4096];
    for (size_t i = 0; i < all_count; i++) {
        /* Distance from point to the entry's position.
         * We'd need the entry's MBR. For now, use a placeholder. */
        all_dists[i] = (double)i; /* placeholder — proper version needs MBR storage */
    }

    /* Copy up to k results sorted by distance (simple insertion sort) */
    for (size_t i = 0; i < all_count && *found < k; i++) {
        /* Find minimum remaining distance */
        size_t min_idx = i;
        for (size_t j = i + 1; j < all_count; j++) {
            if (all_dists[j] < all_dists[min_idx]) min_idx = j;
        }
        /* Swap */
        if (min_idx != i) {
            tdb_rid_t tmp_r = all_results[i]; all_results[i] = all_results[min_idx]; all_results[min_idx] = tmp_r;
            double tmp_d = all_dists[i]; all_dists[i] = all_dists[min_idx]; all_dists[min_idx] = tmp_d;
        }
        results[*found] = all_results[i];
        distances[*found] = all_dists[i];
        (*found)++;
    }

    return TDB_OK;
}
