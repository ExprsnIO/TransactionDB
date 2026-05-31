/*
** tdb_btree.h — B-tree storage over the pager (tables and indexes).
**
** A single unified B-tree implementation backs both table and index storage.
** Keys are compared as opaque byte strings via a comparator chosen at open:
**
**   - TABLE  b-trees use an 8-byte big-endian rowid as the key (so unsigned
**            byte comparison yields numeric order) and store the row record
**            as the value. Large values spill into overflow page chains.
**   - INDEX  b-trees use an encoded key record as the key (compared with a
**            tdb_keyinfo, falling back to memcmp) and store an empty value.
**
** Pages are interior or leaf; inserts split full pages and propagate a
** separator upward. Deletes remove the cell (page merging is intentionally
** left simple: under-full pages are tolerated, reclaimed on the free-list only
** when a page becomes completely empty).
*/
#ifndef TDB_BTREE_H
#define TDB_BTREE_H

#include "tdb_pager.h"
#include "../value/tdb_record.h"   /* tdb_keyinfo */

typedef struct tdb_btree  tdb_btree;
typedef struct tdb_cursor tdb_cursor;

typedef enum { TDB_BT_TABLE = 0, TDB_BT_INDEX = 1 } tdb_btree_kind;

/* Create a new (empty) b-tree; returns its root page number. */
int  tdb_btree_create(tdb_pager *p, tdb_btree_kind kind, tdb_pgno *root_out);

/* Open an existing b-tree rooted at `root`. For index trees, `ki` (may be
** NULL for plain memcmp ordering) is copied for key comparison. */
int  tdb_btree_open(tdb_pager *p, tdb_pgno root, tdb_btree_kind kind,
                    const tdb_keyinfo *ki, tdb_btree **out);
void tdb_btree_close(tdb_btree *bt);

/* Free every page of the b-tree rooted at `root` (interior + leaf pages and
** any leaf overflow chains) back to the pager freelist. Used by DROP to
** reclaim space; the root becomes invalid afterward. */
int  tdb_btree_destroy(tdb_pager *p, tdb_pgno root, tdb_btree_kind kind);

tdb_pgno tdb_btree_root(tdb_btree *bt);

/* Table helpers (key = rowid). put replaces an existing rowid. */
int  tdb_btree_put(tdb_btree *bt, tdb_rowid rowid, const void *val, int n);
int  tdb_btree_get(tdb_btree *bt, tdb_rowid rowid, const uint8_t **val,
                   int *n, tdb_cursor *cur);  /* uses cur's buffer for overflow */
int  tdb_btree_del(tdb_btree *bt, tdb_rowid rowid, int *found);
int  tdb_btree_max_rowid(tdb_btree *bt, tdb_rowid *out);

/* Index helpers (key = encoded record, value empty). */
int  tdb_index_put(tdb_btree *bt, const void *key, int klen);
int  tdb_index_del(tdb_btree *bt, const void *key, int klen, int *found);

/* Generic keyed insert/delete (used internally and by the engine). */
int  tdb_btree_insert(tdb_btree *bt, const uint8_t *key, int klen,
                      const uint8_t *val, int vlen);
int  tdb_btree_remove(tdb_btree *bt, const uint8_t *key, int klen, int *found);

/* -------------------------------- cursor ------------------------------ */
int  tdb_cursor_open(tdb_btree *bt, tdb_cursor **out);
void tdb_cursor_close(tdb_cursor *cur);

int  tdb_cursor_first(tdb_cursor *cur);
int  tdb_cursor_last(tdb_cursor *cur);
int  tdb_cursor_next(tdb_cursor *cur);
int  tdb_cursor_prev(tdb_cursor *cur);
int  tdb_cursor_eof(tdb_cursor *cur);

/* Seek to the first entry with key >= the given key. *cmp (if non-NULL) is set
** to 0 on exact match, or the comparison of the landed key vs target. */
int  tdb_cursor_seek(tdb_cursor *cur, const void *key, int klen, int *cmp);
int  tdb_cursor_seek_rowid(tdb_cursor *cur, tdb_rowid rowid, int *found);

/* Accessors valid when !eof. */
int  tdb_cursor_key(tdb_cursor *cur, const uint8_t **key, int *klen);
int  tdb_cursor_data(tdb_cursor *cur, const uint8_t **val, int *vlen);
int  tdb_cursor_rowid(tdb_cursor *cur, tdb_rowid *rowid);

#endif /* TDB_BTREE_H */
