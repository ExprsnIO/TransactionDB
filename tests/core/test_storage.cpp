// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Rick Holland
#include "tdb_test.h"
#include "tdb/storage.h"
#include "tdb/error.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

static std::string tmp_path(const char *stem) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/tdb_test_%s_%d.tdb", stem, (int)getpid());
    return buf;
}

TDB_TEST(open_extend_close) {
    auto path = tmp_path("storage_open");
    ::unlink(path.c_str());

    tdb_storage_t s;
    std::memset(&s, 0, sizeof(s));
    TDB_REQUIRE_EQ(tdb_storage_open(&s, path.c_str(), 8192), TDB_OK);

    tdb_page_id_t pid = 0;
    TDB_REQUIRE_EQ(tdb_storage_extend(&s, &pid), TDB_OK);
    TDB_REQUIRE(pid >= 1);

    TDB_REQUIRE_EQ(tdb_storage_close(&s), TDB_OK);
    ::unlink(path.c_str());
}

TDB_TEST(write_read_roundtrip) {
    auto path = tmp_path("storage_rw");
    ::unlink(path.c_str());

    tdb_storage_t s;
    std::memset(&s, 0, sizeof(s));
    TDB_REQUIRE_EQ(tdb_storage_open(&s, path.c_str(), 8192), TDB_OK);

    tdb_page_id_t pid = 0;
    TDB_REQUIRE_EQ(tdb_storage_extend(&s, &pid), TDB_OK);

    char buf[8192];
    std::memset(buf, 0, sizeof(buf));
    std::strcpy(buf, "hello tdb");
    TDB_REQUIRE_EQ(tdb_storage_write_page(&s, pid, buf), TDB_OK);

    char rbuf[8192];
    std::memset(rbuf, 0, sizeof(rbuf));
    TDB_REQUIRE_EQ(tdb_storage_read_page(&s, pid, rbuf), TDB_OK);
    TDB_REQUIRE_EQ(std::strcmp(rbuf, "hello tdb"), 0);

    TDB_REQUIRE_EQ(tdb_storage_close(&s), TDB_OK);
    ::unlink(path.c_str());
}

TDB_TEST_MAIN("storage")
