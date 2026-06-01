# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Rick Holland
#
# Test suite for TDB. Each test is a standalone binary that uses the
# header-only harness in tests/harness/. CTest treats each binary as one
# test case.
#
# The roadmap (docs/ROADMAP_V2.md §3 M1) makes this directory the
# non-negotiable safety net for the v2 refactor. Add new tests here as
# new modules land.

# Header-only harness for all tests.
add_library(tdb_test_harness INTERFACE)
target_include_directories(tdb_test_harness INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/harness
)
target_compile_features(tdb_test_harness INTERFACE cxx_std_20)

# Helper: define one test binary linked against the given libraries.
function(tdb_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE tdb_test_harness ${ARGN})
    target_compile_options(${name} PRIVATE
        -Wno-error  # tests may include third-party-ish patterns
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
endfunction()

# ─── Core layer (C) ─────────────────────────────────────────────────────
tdb_add_test(test_storage core/test_storage.cpp tdb_core)
tdb_add_test(test_buffer  core/test_buffer.cpp  tdb_core)
tdb_add_test(test_wal     core/test_wal.cpp     tdb_core)
tdb_add_test(test_crypto  core/test_crypto.cpp  tdb_core)

# ─── Index layer (C) ────────────────────────────────────────────────────
tdb_add_test(test_bptree  index/test_bptree.cpp tdb_index tdb_core)
tdb_add_test(test_hash    index/test_hash.cpp   tdb_index tdb_core)

# ─── SQL layer (C++) ────────────────────────────────────────────────────
tdb_add_test(test_lexer     sql/test_lexer.cpp     tdb_sql tdb_core)
tdb_add_test(test_parser    sql/test_parser.cpp    tdb_sql tdb_core)
tdb_add_test(test_executor  sql/test_executor.cpp  tdb_sql tdb_core)
tdb_add_test(test_optimizer sql/test_optimizer.cpp tdb_sql tdb_core)

# ─── Catalog (C++) ──────────────────────────────────────────────────────
tdb_add_test(test_catalog catalog/test_catalog.cpp tdb_catalog tdb_sql tdb_core)

# ─── Database end-to-end ────────────────────────────────────────────────
tdb_add_test(test_database_smoke database/test_smoke.cpp tdb)

# ─── Script engine ──────────────────────────────────────────────────────
tdb_add_test(test_lua_bridge script/test_lua.cpp tdb)
