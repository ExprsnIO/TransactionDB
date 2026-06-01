// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/hash_index.h"
#include "tdb/buffer.h"
#include "tdb/storage.h"
#include "tdb/error.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

TDB_TEST(hash_create_insert_lookup) {
    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/tdb_test_hash_%d.tdb", (int)getpid());
    ::unlink(path);

    tdb_storage_t s{};
    TDB_REQUIRE_EQ(tdb_storage_open(&s, path, 8192), TDB_OK);
    tdb_buffer_pool_t pool{};
    TDB_REQUIRE_EQ(tdb_buffer_pool_init(&pool, 32, 8192, &s), TDB_OK);

    tdb_hash_index_t idx{};
    TDB_REQUIRE_EQ(tdb_hash_index_create(&idx, &pool, 32), TDB_OK);

    const char *key = "hello";
    tdb_rid_t rid{};
    rid.page_id = 7;
    rid.slot = 3;
    TDB_REQUIRE_EQ(tdb_hash_index_insert(&idx, key, std::strlen(key), rid), TDB_OK);

    tdb_rid_t found{};
    TDB_REQUIRE_EQ(tdb_hash_index_lookup(&idx, key, std::strlen(key), &found), TDB_OK);
    TDB_REQUIRE_EQ(found.page_id, (tdb_page_id_t)7);

    tdb_buffer_pool_destroy(&pool);
    tdb_storage_close(&s);
    ::unlink(path);
}

TDB_TEST_MAIN("hash_index")
