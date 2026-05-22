#include "tdb/hash_index.h"
#include <stdlib.h>
#include <string.h>

/*
 * Extendible hash index — in-memory implementation.
 *
 * Directory: array of 2^global_depth pointers to buckets.
 * Each bucket has a local_depth and holds up to BUCKET_CAP entries.
 * On overflow: if local_depth < global_depth, split bucket.
 * If local_depth == global_depth, double directory then split.
 */

#define BUCKET_CAP TDB_HASH_BUCKET_CAPACITY

/* Forward declare hash function from util/hash.c */
extern uint64_t tdb_hash_fnv1a(const void *data, size_t len);

typedef struct {
    uint64_t  hash;
    uint16_t  key_len;
    tdb_rid_t rid;
    uint8_t  *key_data;  /* malloc'd copy of the key */
} hash_entry_t;

typedef struct {
    uint32_t      local_depth;
    uint32_t      count;
    hash_entry_t  entries[BUCKET_CAP];
} hash_bucket_t;

typedef struct {
    hash_bucket_t **buckets;  /* directory: 2^global_depth pointers */
    uint32_t        dir_size; /* 2^global_depth */
} hash_directory_t;

/* Get directory index from hash and depth */
static uint32_t hash_dir_index(uint64_t hash, uint32_t depth) {
    return (uint32_t)(hash & ((1ULL << depth) - 1));
}

tdb_status_t tdb_hash_index_create(tdb_hash_index_t *idx, tdb_buffer_pool_t *pool,
                                    size_t key_max_len) {
    if (!idx) return TDB_ERR_INVALID_ARG;

    idx->global_depth = 1;
    idx->num_buckets = 2;
    idx->key_max_len = key_max_len;
    idx->pool = pool;
    idx->directory_page = 0; /* unused in in-memory mode */

    /* Allocate directory with internal pointer storage */
    hash_directory_t *dir = (hash_directory_t *)calloc(1, sizeof(hash_directory_t));
    if (!dir) return TDB_ERR_NOMEM;
    dir->dir_size = 2;
    dir->buckets = (hash_bucket_t **)calloc(2, sizeof(hash_bucket_t *));
    if (!dir->buckets) { free(dir); return TDB_ERR_NOMEM; }

    /* Create initial buckets */
    for (uint32_t i = 0; i < 2; i++) {
        dir->buckets[i] = (hash_bucket_t *)calloc(1, sizeof(hash_bucket_t));
        if (!dir->buckets[i]) return TDB_ERR_NOMEM;
        dir->buckets[i]->local_depth = 1;
        dir->buckets[i]->count = 0;
    }

    /* Store directory pointer in directory_page field */
    idx->directory_page = (tdb_page_id_t)(uintptr_t)dir;
    return TDB_OK;
}

static hash_directory_t *get_dir(tdb_hash_index_t *idx) {
    return (hash_directory_t *)(uintptr_t)idx->directory_page;
}

/* Double the directory */
static tdb_status_t hash_double_directory(hash_directory_t *dir) {
    uint32_t new_size = dir->dir_size * 2;
    hash_bucket_t **new_buckets = (hash_bucket_t **)calloc(new_size, sizeof(hash_bucket_t *));
    if (!new_buckets) return TDB_ERR_NOMEM;

    /* Each old entry[i] is copied to new_buckets[i] and new_buckets[i + old_size] */
    for (uint32_t i = 0; i < dir->dir_size; i++) {
        new_buckets[i] = dir->buckets[i];
        new_buckets[i + dir->dir_size] = dir->buckets[i];
    }

    free(dir->buckets);
    dir->buckets = new_buckets;
    dir->dir_size = new_size;
    return TDB_OK;
}

/* Split a bucket */
static tdb_status_t hash_split_bucket(tdb_hash_index_t *idx, uint32_t bucket_idx) {
    hash_directory_t *dir = get_dir(idx);
    hash_bucket_t *old_bucket = dir->buckets[bucket_idx];

    /* If local_depth == global_depth, need to double directory first */
    if (old_bucket->local_depth == idx->global_depth) {
        tdb_status_t rc = hash_double_directory(dir);
        if (rc != TDB_OK) return rc;
        idx->global_depth++;
    }

    /* Create new bucket with incremented local depth */
    hash_bucket_t *new_bucket = (hash_bucket_t *)calloc(1, sizeof(hash_bucket_t));
    if (!new_bucket) return TDB_ERR_NOMEM;

    uint32_t new_local_depth = old_bucket->local_depth + 1;
    old_bucket->local_depth = new_local_depth;
    new_bucket->local_depth = new_local_depth;
    new_bucket->count = 0;
    idx->num_buckets++;

    /* The bit that distinguishes old from new bucket */
    uint32_t split_bit = 1U << (new_local_depth - 1);

    /* Update directory: entries that have the split_bit set point to new_bucket */
    uint32_t mask = (1U << new_local_depth) - 1;
    uint32_t old_pattern = bucket_idx & (mask >> 1); /* pattern without split bit */
    for (uint32_t i = 0; i < dir->dir_size; i++) {
        if ((i & mask) == (old_pattern | split_bit)) {
            dir->buckets[i] = new_bucket;
        }
    }

    /* Redistribute entries */
    hash_entry_t temp[BUCKET_CAP];
    uint32_t temp_count = old_bucket->count;
    memcpy(temp, old_bucket->entries, temp_count * sizeof(hash_entry_t));
    old_bucket->count = 0;
    new_bucket->count = 0;

    for (uint32_t i = 0; i < temp_count; i++) {
        uint32_t dir_idx = hash_dir_index(temp[i].hash, idx->global_depth);
        hash_bucket_t *target = dir->buckets[dir_idx];
        target->entries[target->count] = temp[i];
        target->count++;
    }

    return TDB_OK;
}

tdb_status_t tdb_hash_index_insert(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len, tdb_rid_t rid) {
    if (!idx || !key || key_len == 0) return TDB_ERR_INVALID_ARG;

    hash_directory_t *dir = get_dir(idx);
    uint64_t h = tdb_hash_fnv1a(key, key_len);
    uint32_t dir_idx = hash_dir_index(h, idx->global_depth);
    hash_bucket_t *bucket = dir->buckets[dir_idx];

    /* Check if bucket is full */
    if (bucket->count >= BUCKET_CAP) {
        tdb_status_t rc = hash_split_bucket(idx, dir_idx);
        if (rc != TDB_OK) return rc;
        /* Recompute after split */
        dir = get_dir(idx);
        dir_idx = hash_dir_index(h, idx->global_depth);
        bucket = dir->buckets[dir_idx];

        /* If still full after split (all entries hashed to same bucket), error */
        if (bucket->count >= BUCKET_CAP) return TDB_ERR_FULL;
    }

    /* Insert entry */
    hash_entry_t *e = &bucket->entries[bucket->count];
    e->hash = h;
    e->key_len = (uint16_t)key_len;
    e->rid = rid;
    e->key_data = (uint8_t *)malloc(key_len);
    if (!e->key_data) return TDB_ERR_NOMEM;
    memcpy(e->key_data, key, key_len);
    bucket->count++;

    return TDB_OK;
}

tdb_status_t tdb_hash_index_lookup(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len, tdb_rid_t *rid_out) {
    if (!idx || !key || key_len == 0) return TDB_ERR_INVALID_ARG;

    hash_directory_t *dir = get_dir(idx);
    uint64_t h = tdb_hash_fnv1a(key, key_len);
    uint32_t dir_idx = hash_dir_index(h, idx->global_depth);
    hash_bucket_t *bucket = dir->buckets[dir_idx];

    for (uint32_t i = 0; i < bucket->count; i++) {
        hash_entry_t *e = &bucket->entries[i];
        if (e->hash == h && e->key_len == key_len &&
            memcmp(e->key_data, key, key_len) == 0) {
            if (rid_out) *rid_out = e->rid;
            return TDB_OK;
        }
    }

    return TDB_ERR_NOT_FOUND;
}

tdb_status_t tdb_hash_index_delete(tdb_hash_index_t *idx, const void *key,
                                    size_t key_len) {
    if (!idx || !key || key_len == 0) return TDB_ERR_INVALID_ARG;

    hash_directory_t *dir = get_dir(idx);
    uint64_t h = tdb_hash_fnv1a(key, key_len);
    uint32_t dir_idx = hash_dir_index(h, idx->global_depth);
    hash_bucket_t *bucket = dir->buckets[dir_idx];

    for (uint32_t i = 0; i < bucket->count; i++) {
        hash_entry_t *e = &bucket->entries[i];
        if (e->hash == h && e->key_len == key_len &&
            memcmp(e->key_data, key, key_len) == 0) {
            free(e->key_data);
            /* Shift remaining entries */
            for (uint32_t j = i; j < bucket->count - 1; j++) {
                bucket->entries[j] = bucket->entries[j + 1];
            }
            bucket->count--;
            return TDB_OK;
        }
    }

    return TDB_ERR_NOT_FOUND;
}
