#include "tdb/buffer.h"
#include "tdb/page.h"
#include <stdlib.h>
#include <string.h>

/*
 * Buffer Pool Manager
 *
 * LRU-based page cache with dirty page tracking.
 * Uses a hash table (page_id % TDB_BUF_HASH_SIZE with chaining) for fast
 * page_id -> frame lookup, and a doubly-linked LRU list for eviction policy.
 *
 * Free list: initially all frames are linked via lru_next as a singly-linked
 * free list. When a frame is needed, the free list is checked first; only when
 * it is empty do we evict from the LRU tail.
 */

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

/* Hash a page_id to a bucket index */
static inline size_t hash_bucket(tdb_page_id_t page_id)
{
    return (size_t)(page_id % TDB_BUF_HASH_SIZE);
}

/* Look up a frame in the hash table by page_id. Returns NULL if not found. */
static tdb_buf_frame_t *hash_lookup(tdb_buffer_pool_t *pool, tdb_page_id_t page_id)
{
    size_t bucket = hash_bucket(page_id);
    tdb_buf_frame_t *frame = pool->hash_table[bucket];
    while (frame) {
        if (frame->page_id == page_id) {
            return frame;
        }
        frame = frame->hash_next;
    }
    return NULL;
}

/* Insert a frame into the hash table. */
static void hash_insert(tdb_buffer_pool_t *pool, tdb_buf_frame_t *frame)
{
    size_t bucket = hash_bucket(frame->page_id);
    frame->hash_next = pool->hash_table[bucket];
    pool->hash_table[bucket] = frame;
}

/* Remove a frame from the hash table. */
static void hash_remove(tdb_buffer_pool_t *pool, tdb_buf_frame_t *frame)
{
    size_t bucket = hash_bucket(frame->page_id);
    tdb_buf_frame_t **pp = &pool->hash_table[bucket];
    while (*pp) {
        if (*pp == frame) {
            *pp = frame->hash_next;
            frame->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/* Remove a frame from the LRU doubly-linked list. */
static void lru_remove(tdb_buffer_pool_t *pool, tdb_buf_frame_t *frame)
{
    if (frame->lru_prev) {
        frame->lru_prev->lru_next = frame->lru_next;
    } else {
        pool->lru_head = frame->lru_next;
    }
    if (frame->lru_next) {
        frame->lru_next->lru_prev = frame->lru_prev;
    } else {
        pool->lru_tail = frame->lru_prev;
    }
    frame->lru_prev = NULL;
    frame->lru_next = NULL;
}

/* Insert a frame at the head (most recently used) of the LRU list. */
static void lru_promote(tdb_buffer_pool_t *pool, tdb_buf_frame_t *frame)
{
    /* If already at head, nothing to do */
    if (pool->lru_head == frame) {
        return;
    }

    /* Remove from current position if in the list */
    if (frame->lru_prev || frame->lru_next || pool->lru_head == frame) {
        lru_remove(pool, frame);
    }

    /* Insert at head */
    frame->lru_prev = NULL;
    frame->lru_next = pool->lru_head;
    if (pool->lru_head) {
        pool->lru_head->lru_prev = frame;
    }
    pool->lru_head = frame;
    if (!pool->lru_tail) {
        pool->lru_tail = frame;
    }
}

/* Pop a frame from the free list. Returns NULL if the free list is empty. */
static tdb_buf_frame_t *free_list_pop(tdb_buffer_pool_t *pool)
{
    tdb_buf_frame_t *frame = pool->free_list;
    if (frame) {
        pool->free_list = frame->lru_next;
        frame->lru_next = NULL;
    }
    return frame;
}

/*
 * Find or create a free frame for use. Tries free list first, then evicts the
 * LRU-tail unpinned frame. Returns NULL if all frames are pinned.
 */
static tdb_buf_frame_t *get_free_frame(tdb_buffer_pool_t *pool)
{
    /* Try the free list first */
    tdb_buf_frame_t *frame = free_list_pop(pool);
    if (frame) {
        return frame;
    }

    /* Walk from LRU tail to find an unpinned frame to evict */
    tdb_buf_frame_t *victim = pool->lru_tail;
    while (victim) {
        if (victim->pin_count == 0) {
            /* Found a victim. Remove from LRU and hash table. */
            lru_remove(pool, victim);

            /* Flush if dirty before evicting */
            if (victim->dirty) {
                tdb_storage_write_page(pool->storage, victim->page_id, victim->data);
                victim->dirty = false;
            }

            hash_remove(pool, victim);
            victim->page_id = TDB_INVALID_PAGE_ID;
            return victim;
        }
        victim = victim->lru_prev;
    }

    /* All frames are pinned — cannot evict */
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

tdb_status_t tdb_buffer_pool_init(tdb_buffer_pool_t *pool, size_t capacity,
                                   size_t page_size, tdb_storage_t *storage)
{
    if (!pool || capacity == 0 || !storage) {
        return TDB_ERR_INVALID_ARG;
    }

    pool->capacity  = capacity;
    pool->page_size = page_size;
    pool->storage   = storage;
    pool->lru_head  = NULL;
    pool->lru_tail  = NULL;
    pool->free_list = NULL;

    /* Zero-initialise the hash table */
    memset(pool->hash_table, 0, sizeof(pool->hash_table));

    /* Allocate frame array */
    pool->frames = (tdb_buf_frame_t *)calloc(capacity, sizeof(tdb_buf_frame_t));
    if (!pool->frames) {
        return TDB_ERR_NOMEM;
    }

    /* Allocate per-frame page buffers and build the free list */
    for (size_t i = 0; i < capacity; i++) {
        pool->frames[i].data = malloc(page_size);
        if (!pool->frames[i].data) {
            /* Roll back allocations already made */
            for (size_t j = 0; j < i; j++) {
                free(pool->frames[j].data);
            }
            free(pool->frames);
            pool->frames = NULL;
            return TDB_ERR_NOMEM;
        }
        pool->frames[i].page_id   = TDB_INVALID_PAGE_ID;
        pool->frames[i].dirty     = false;
        pool->frames[i].pin_count = 0;
        pool->frames[i].page_lsn  = 0;
        pool->frames[i].lru_prev  = NULL;
        pool->frames[i].lru_next  = NULL;
        pool->frames[i].hash_next = NULL;

        /* Push onto front of free list (using lru_next as the link) */
        pool->frames[i].lru_next = pool->free_list;
        pool->free_list = &pool->frames[i];
    }

    return TDB_OK;
}

void tdb_buffer_pool_destroy(tdb_buffer_pool_t *pool)
{
    if (!pool || !pool->frames) {
        return;
    }

    /* Flush all dirty pages before tearing down */
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->frames[i].dirty && pool->frames[i].page_id != TDB_INVALID_PAGE_ID) {
            tdb_storage_write_page(pool->storage, pool->frames[i].page_id,
                                   pool->frames[i].data);
        }
        free(pool->frames[i].data);
    }

    free(pool->frames);
    pool->frames    = NULL;
    pool->lru_head  = NULL;
    pool->lru_tail  = NULL;
    pool->free_list = NULL;
    memset(pool->hash_table, 0, sizeof(pool->hash_table));
}

tdb_status_t tdb_buffer_pool_fetch(tdb_buffer_pool_t *pool, tdb_page_id_t page_id,
                                    void **page_out)
{
    if (!pool || !page_out) {
        return TDB_ERR_INVALID_ARG;
    }

    /* 1. Look up the hash table */
    tdb_buf_frame_t *frame = hash_lookup(pool, page_id);
    if (frame) {
        /* Cache hit: pin and promote to MRU */
        frame->pin_count++;
        lru_promote(pool, frame);
        *page_out = frame->data;
        return TDB_OK;
    }

    /* 2. Cache miss: get a free frame (free list or eviction) */
    frame = get_free_frame(pool);
    if (!frame) {
        return TDB_ERR_FULL;  /* all frames pinned */
    }

    /* 3. Read page from disk */
    tdb_status_t rc = tdb_storage_read_page(pool->storage, page_id, frame->data);
    if (rc != TDB_OK) {
        /* Put the frame back on the free list */
        frame->lru_next = pool->free_list;
        pool->free_list = frame;
        return rc;
    }

    /* 4. Set up frame metadata */
    frame->page_id   = page_id;
    frame->dirty     = false;
    frame->pin_count = 1;
    frame->page_lsn  = 0;

    /* 5. Insert into hash table and LRU (at head) */
    hash_insert(pool, frame);
    lru_promote(pool, frame);

    *page_out = frame->data;
    return TDB_OK;
}

tdb_status_t tdb_buffer_pool_new_page(tdb_buffer_pool_t *pool,
                                       tdb_page_id_t *page_id_out,
                                       void **page_out)
{
    if (!pool || !page_id_out || !page_out) {
        return TDB_ERR_INVALID_ARG;
    }

    /* 1. Extend storage to get a new page id */
    tdb_page_id_t new_id;
    tdb_status_t rc = tdb_storage_extend(pool->storage, &new_id);
    if (rc != TDB_OK) {
        return rc;
    }

    /* 2. Get a free frame */
    tdb_buf_frame_t *frame = get_free_frame(pool);
    if (!frame) {
        return TDB_ERR_FULL;
    }

    /* 3. Initialize the page in memory */
    memset(frame->data, 0, pool->page_size);
    tdb_page_init(frame->data, pool->page_size, new_id);

    /* 4. Set up frame metadata */
    frame->page_id   = new_id;
    frame->dirty     = true;   /* new page needs to be written out */
    frame->pin_count = 1;
    frame->page_lsn  = 0;

    /* 5. Insert into hash table and LRU */
    hash_insert(pool, frame);
    lru_promote(pool, frame);

    *page_id_out = new_id;
    *page_out    = frame->data;
    return TDB_OK;
}

tdb_status_t tdb_buffer_pool_mark_dirty(tdb_buffer_pool_t *pool, tdb_page_id_t page_id)
{
    if (!pool) {
        return TDB_ERR_INVALID_ARG;
    }

    tdb_buf_frame_t *frame = hash_lookup(pool, page_id);
    if (!frame) {
        return TDB_ERR_NOT_FOUND;
    }

    if (frame->pin_count == 0) {
        return TDB_ERR_INVALID_ARG; /* must be pinned to mark dirty */
    }

    frame->dirty = true;
    return TDB_OK;
}

tdb_status_t tdb_buffer_pool_unpin(tdb_buffer_pool_t *pool, tdb_page_id_t page_id)
{
    if (!pool) {
        return TDB_ERR_INVALID_ARG;
    }

    tdb_buf_frame_t *frame = hash_lookup(pool, page_id);
    if (!frame) {
        return TDB_ERR_NOT_FOUND;
    }

    if (frame->pin_count == 0) {
        return TDB_ERR_INVALID_ARG; /* already fully unpinned */
    }

    frame->pin_count--;
    return TDB_OK;
}

tdb_status_t tdb_buffer_pool_flush_page(tdb_buffer_pool_t *pool, tdb_page_id_t page_id)
{
    if (!pool) {
        return TDB_ERR_INVALID_ARG;
    }

    tdb_buf_frame_t *frame = hash_lookup(pool, page_id);
    if (!frame) {
        return TDB_ERR_NOT_FOUND;
    }

    if (frame->dirty) {
        tdb_status_t rc = tdb_storage_write_page(pool->storage, frame->page_id,
                                                  frame->data);
        if (rc != TDB_OK) {
            return rc;
        }
        frame->dirty = false;
    }

    return TDB_OK;
}

tdb_status_t tdb_buffer_pool_flush_all(tdb_buffer_pool_t *pool)
{
    if (!pool || !pool->frames) {
        return TDB_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < pool->capacity; i++) {
        tdb_buf_frame_t *frame = &pool->frames[i];
        if (frame->dirty && frame->page_id != TDB_INVALID_PAGE_ID) {
            tdb_status_t rc = tdb_storage_write_page(pool->storage,
                                                      frame->page_id,
                                                      frame->data);
            if (rc != TDB_OK) {
                return rc;
            }
            frame->dirty = false;
        }
    }

    return TDB_OK;
}
