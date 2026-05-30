/*
** tdb_pager.h — the page cache and transaction/durability coordinator.
**
** The pager presents the database as an array of fixed-size pages (1-based
** page numbers; page 1 carries the 100-byte database header). It owns a page
** cache, the free-list, and a WAL. Reads consult the WAL before the main file;
** writes are buffered in cache and flushed atomically to the WAL on commit.
*/
#ifndef TDB_PAGER_H
#define TDB_PAGER_H

#include "tdb_file.h"

struct tdb_page {
  tdb_pgno pgno;
  uint8_t *data;     /* page_size bytes */
  int      dirty;
  int      refs;     /* pin count */
  struct tdb_page *hnext;            /* hash bucket chain */
  struct tdb_page *lru_prev, *lru_next;
};

int  tdb_pager_open(const tdb_vfs *vfs, const char *path, int flags,
                    tdb_pager **out);
int  tdb_pager_close(tdb_pager *p);

uint32_t tdb_pager_page_size(tdb_pager *p);
tdb_pgno tdb_pager_db_size(tdb_pager *p);
int      tdb_pager_is_readonly(tdb_pager *p);

/* Pin and fetch a page (read from cache/WAL/file). Release with unref. */
int  tdb_pager_get(tdb_pager *p, tdb_pgno pgno, tdb_page **out);
void tdb_pager_unref(tdb_pager *p, tdb_page *pg);

/* Mark a pinned page as modified by the current transaction. */
int  tdb_pager_write(tdb_pager *p, tdb_page *pg);

/* Allocate a fresh page (from the free-list or by growing the file). The page
** is returned pinned and already marked writable. */
int  tdb_pager_alloc(tdb_pager *p, tdb_page **out);
/* Return a page to the free-list. */
int  tdb_pager_free(tdb_pager *p, tdb_pgno pgno);

/* Transaction control. */
int  tdb_pager_begin(tdb_pager *p);     /* begin a write transaction */
int  tdb_pager_commit(tdb_pager *p);    /* atomically flush to WAL + fsync */
int  tdb_pager_rollback(tdb_pager *p);  /* discard buffered changes */
int  tdb_pager_checkpoint(tdb_pager *p);

/* Statement-level savepoints (within a write transaction). tdb_pager_savepoint
** returns a level index; release discards a savepoint (keeping its changes);
** rollback reverts to it. */
int  tdb_pager_savepoint(tdb_pager *p);
int  tdb_pager_savepoint_release(tdb_pager *p, int level);
int  tdb_pager_savepoint_rollback(tdb_pager *p, int level);

/* Header metadata accessors (persisted in page 1). */
uint64_t tdb_pager_max_txnid(tdb_pager *p);
void     tdb_pager_set_max_txnid(tdb_pager *p, uint64_t v);
uint32_t tdb_pager_schema_cookie(tdb_pager *p);
void     tdb_pager_set_schema_cookie(tdb_pager *p, uint32_t v);
uint32_t tdb_pager_catalog_root(tdb_pager *p);
void     tdb_pager_set_catalog_root(tdb_pager *p, uint32_t pgno);

#endif /* TDB_PAGER_H */
