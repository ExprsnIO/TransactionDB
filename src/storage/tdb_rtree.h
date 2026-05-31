/*
** tdb_rtree.h — a persistent R-tree spatial index over the pager.
**
** An R-tree groups 2-D bounding boxes into a balanced, height-paged tree so a
** window query (find everything whose bbox overlaps a query box) touches only
** the relevant subtrees instead of every row. It is the index behind spatial
** predicates (ST_Contains / ST_Within / ST_Intersects / ST_DWithin).
**
** One tree node per page. The root page number is stable for the life of the
** index (a root split rehouses the old root's entries into fresh children and
** rewrites the root in place), so the catalog can store the root once.
**
** Leaf entries map a bounding box to a table rowid; interior entries map a
** bounding box (the cover of a subtree) to a child page. Entries are
** fixed-size, so a node is a simple count-prefixed array — see tdb_rtree.c.
**
** The tree keys on bounding boxes only; the executor re-checks each candidate
** rowid against the exact predicate (and MVCC visibility) after the search, so
** false positives from the coarse bbox test are harmless.
*/
#ifndef TDB_RTREE_H
#define TDB_RTREE_H

#include "tdb_pager.h"
#include "../value/tdb_geom.h"   /* tdb_bbox */

/* Create an empty R-tree; returns its (stable) root page number. */
int tdb_rtree_create(tdb_pager *p, tdb_pgno *root_out);

/* Reclaim every page of the tree rooted at `root`. */
int tdb_rtree_destroy(tdb_pager *p, tdb_pgno root);

/* Insert (bbox -> rowid). The root page number never changes. */
int tdb_rtree_insert(tdb_pager *p, tdb_pgno root, const tdb_bbox *bb, tdb_rowid rowid);

/* Remove the entry for `rowid` whose box overlaps `bb`. *found reports whether
** a matching entry was removed. Nodes may be left under-full (no condense). */
int tdb_rtree_delete(tdb_pager *p, tdb_pgno root, const tdb_bbox *bb,
                     tdb_rowid rowid, int *found);

/* Visit every rowid whose stored box overlaps the query box `q`. The callback
** returns 0 to continue or non-zero to abort the walk (propagated as the
** return value). */
typedef int (*tdb_rtree_visit)(void *ctx, tdb_rowid rowid);
int tdb_rtree_search(tdb_pager *p, tdb_pgno root, const tdb_bbox *q,
                     tdb_rtree_visit cb, void *ctx);

#endif /* TDB_RTREE_H */
