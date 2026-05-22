#ifndef TDB_HASH_INDEX_H
#define TDB_HASH_INDEX_H

#include "tdb/types.h"
#include "tdb/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Extendible hash index.
 * Directory of 2^global_depth bucket pointers.
 * Buckets have local_depth <= global_depth.
 * On overflow: split bucket, double directory if needed.
 */

#define TDB_HASH_BUCKET_CAPACITY 64  /* entries per bucket */

typedef struct {
    tdb_rid_t rid;
    uint64_t  hash;
    uint16_t  key_len;
    /* key data follows inline */
} tdb_hash_entry_t;

typedef struct {
    uint32_t  local_depth;
    uint32_t  count;
    /* entries follow */
} tdb_hash_bucket_header_t;

typedef struct {
    tdb_page_id_t    directory_page;
    uint32_t         global_depth;
    uint32_t         num_buckets;
    size_t           key_max_len;
    tdb_buffer_pool_t *pool;
} tdb_hash_index_t;

tdb_status_t tdb_hash_index_create(tdb_hash_index_t *idx, tdb_buffer_pool_t *pool,
                                    size_t key_max_len);
tdb_status_t tdb_hash_index_insert(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len, tdb_rid_t rid);
tdb_status_t tdb_hash_index_lookup(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len, tdb_rid_t *rid_out);
tdb_status_t tdb_hash_index_delete(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* TDB_HASH_INDEX_H */
