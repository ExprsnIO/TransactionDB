#ifndef TDB_BUFFER_H
#define TDB_BUFFER_H

#include "tdb/types.h"
#include "tdb/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TDB_INVALID_PAGE_ID ((tdb_page_id_t)-1)

/* Buffer frame: one cached page */
typedef struct tdb_buf_frame {
    tdb_page_id_t  page_id;
    void          *data;
    bool           dirty;
    uint32_t       pin_count;
    tdb_lsn_t      page_lsn;
    struct tdb_buf_frame *lru_prev;
    struct tdb_buf_frame *lru_next;
    struct tdb_buf_frame *hash_next; /* hash chain */
} tdb_buf_frame_t;

/* Hash table bucket for page_id -> frame lookup */
#define TDB_BUF_HASH_SIZE 1024

typedef struct {
    tdb_buf_frame_t *frames;
    size_t           capacity;     /* total frame count */
    size_t           page_size;
    tdb_storage_t   *storage;      /* backing disk storage */
    tdb_buf_frame_t *lru_head;     /* most recently used */
    tdb_buf_frame_t *lru_tail;     /* least recently used */
    tdb_buf_frame_t *hash_table[TDB_BUF_HASH_SIZE];
    tdb_buf_frame_t *free_list;    /* unoccupied frames */
} tdb_buffer_pool_t;

/* Initialize buffer pool with given capacity (# of frames) backed by storage */
tdb_status_t tdb_buffer_pool_init(tdb_buffer_pool_t *pool, size_t capacity,
                                   size_t page_size, tdb_storage_t *storage);

/* Destroy buffer pool and free all frames */
void tdb_buffer_pool_destroy(tdb_buffer_pool_t *pool);

/* Fetch a page into the pool. Returns pinned page pointer.
 * If the page is already cached, just pins and returns it.
 * Otherwise reads from disk, evicting an unpinned LRU page if needed. */
tdb_status_t tdb_buffer_pool_fetch(tdb_buffer_pool_t *pool, tdb_page_id_t page_id,
                                    void **page_out);

/* Allocate a new page on disk and return it pinned in the pool */
tdb_status_t tdb_buffer_pool_new_page(tdb_buffer_pool_t *pool, tdb_page_id_t *page_id_out,
                                       void **page_out);

/* Mark a pinned page as dirty */
tdb_status_t tdb_buffer_pool_mark_dirty(tdb_buffer_pool_t *pool, tdb_page_id_t page_id);

/* Unpin a page (decrements pin count, makes page eligible for eviction) */
tdb_status_t tdb_buffer_pool_unpin(tdb_buffer_pool_t *pool, tdb_page_id_t page_id);

/* Flush a specific dirty page to disk */
tdb_status_t tdb_buffer_pool_flush_page(tdb_buffer_pool_t *pool, tdb_page_id_t page_id);

/* Flush all dirty pages to disk */
tdb_status_t tdb_buffer_pool_flush_all(tdb_buffer_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* TDB_BUFFER_H */
