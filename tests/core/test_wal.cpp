// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/wal.h"
#include "tdb/error.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

TDB_TEST(wal_init_destroy) {
    char path[256];
    std::snprintf(path, sizeof(path), "/tmp/tdb_test_wal_%d.log", (int)getpid());
    ::unlink(path);

    tdb_wal_t wal;
    std::memset(&wal, 0, sizeof(wal));
    TDB_REQUIRE_EQ(tdb_wal_init(&wal, path), TDB_OK);
    TDB_REQUIRE(wal.next_lsn >= 1);

    extern void tdb_wal_destroy(tdb_wal_t *);
    tdb_wal_destroy(&wal);
    ::unlink(path);
}

TDB_TEST_MAIN("wal")
