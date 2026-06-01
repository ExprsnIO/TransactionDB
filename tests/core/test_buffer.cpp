// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/buffer.h"
#include "tdb/storage.h"
#include "tdb/error.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

static std::string tmp_path() {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/tdb_test_buffer_%d.tdb", (int)getpid());
    return buf;
}

TDB_TEST(buffer_init_destroy) {
    auto path = tmp_path();
    ::unlink(path.c_str());

    tdb_storage_t s;
    std::memset(&s, 0, sizeof(s));
    TDB_REQUIRE_EQ(tdb_storage_open(&s, path.c_str(), 8192), TDB_OK);

    tdb_buffer_pool_t pool;
    std::memset(&pool, 0, sizeof(pool));
    TDB_REQUIRE_EQ(tdb_buffer_pool_init(&pool, 16, 8192, &s), TDB_OK);
    tdb_buffer_pool_destroy(&pool);

    tdb_storage_close(&s);
    ::unlink(path.c_str());
}

TDB_TEST_MAIN("buffer")
