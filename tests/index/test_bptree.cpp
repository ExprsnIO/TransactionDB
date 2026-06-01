// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/bptree.h"
#include "tdb/buffer.h"
#include "tdb/storage.h"
#include "tdb/error.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

struct Env {
    tdb_storage_t s{};
    tdb_buffer_pool_t pool{};
    std::string path;

    Env(const char *stem) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "/tmp/tdb_test_bp_%s_%d.tdb", stem, (int)getpid());
        path = buf;
        ::unlink(path.c_str());
        if (tdb_storage_open(&s, path.c_str(), 8192) != TDB_OK) throw std::runtime_error("storage_open");
        if (tdb_buffer_pool_init(&pool, 32, 8192, &s) != TDB_OK) throw std::runtime_error("pool_init");
    }
    ~Env() {
        tdb_buffer_pool_destroy(&pool);
        tdb_storage_close(&s);
        ::unlink(path.c_str());
    }
};

TDB_TEST(bptree_create_insert_search) {
    Env env("smoke");
    tdb_bptree_t tree{};
    TDB_REQUIRE_EQ(tdb_bptree_create(&tree, &env.pool, sizeof(int32_t), true, tdb_key_cmp_default), TDB_OK);

    int32_t k = 7;
    tdb_rid_t rid{};
    rid.page_id = 100;
    rid.slot = 0;
    TDB_REQUIRE_EQ(tdb_bptree_insert(&tree, &k, rid), TDB_OK);

    tdb_rid_t found{};
    TDB_REQUIRE_EQ(tdb_bptree_search(&tree, &k, &found), TDB_OK);
    TDB_REQUIRE_EQ(found.page_id, (tdb_page_id_t)100);
}

TDB_TEST_MAIN("bptree")
