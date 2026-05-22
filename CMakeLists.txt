cmake_minimum_required(VERSION 3.20)
project(tdb
    VERSION 1.0.0
    DESCRIPTION "TDB - SQL Database Engine"
    LANGUAGES C CXX
)

# Standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# POSIX target
add_compile_definitions(_POSIX_C_SOURCE=200809L)

# Compiler warnings
add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Debug flags
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DTDB_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DTDB_DEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Include paths
include_directories(${PROJECT_SOURCE_DIR}/include)

# Options
option(TDB_BUILD_TESTS "Build test suite" ON)
option(TDB_PAGE_SIZE_DEFAULT "Default page size in bytes" 8192)
option(TDB_WITH_ZSTD "Link libzstd for page-level compression" OFF)
option(TDB_WITH_ICU  "Link libicu for full Unicode case folding" OFF)
option(TDB_WITH_OPENSSL "Link OpenSSL for AES-256-GCM and TLS" OFF)
option(TDB_WITH_LLVM "Enable LLVM JIT compiler for Lua scripts" OFF)
option(TDB_WITH_JSC  "Enable JavaScriptCore engine (macOS only)" OFF)

# Optional zstd page-level compression. Storage.c keeps a pass-through
# fallback when this is OFF; tdb_compression_available() reports 0 in that
# case so callers can branch on it.
if(TDB_WITH_ZSTD)
    find_package(zstd CONFIG QUIET)
    if(zstd_FOUND)
        add_compile_definitions(TDB_WITH_ZSTD=1)
        set(TDB_ZSTD_LIBS zstd::libzstd_shared)
    else()
        find_library(ZSTD_LIBRARY zstd REQUIRED)
        find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
        add_compile_definitions(TDB_WITH_ZSTD=1)
        include_directories(${ZSTD_INCLUDE_DIR})
        set(TDB_ZSTD_LIBS ${ZSTD_LIBRARY})
    endif()
endif()

if(TDB_WITH_ICU)
    find_package(ICU REQUIRED COMPONENTS uc i18n)
    add_compile_definitions(TDB_WITH_ICU=1)
    set(TDB_ICU_LIBS ICU::uc ICU::i18n)
endif()

if(TDB_WITH_OPENSSL)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_compile_definitions(TDB_WITH_OPENSSL=1)
    set(TDB_OPENSSL_LIBS OpenSSL::Crypto)
endif()

configure_file(
    ${PROJECT_SOURCE_DIR}/include/tdb/config.h.in
    ${PROJECT_SOURCE_DIR}/include/tdb/config.h
)

# Vendored Lua 5.4 (built before tdb_script consumes it)
add_subdirectory(third_party/lua)

# Core C library (storage, buffer, WAL, page, index, txn, crypto, util)
add_library(tdb_core STATIC
    src/core/page/page.c
    src/core/storage/storage.c
    src/core/buffer/buffer.c
    src/core/wal/wal.c
    src/txn/txn.c
    src/crypto/crypto.c
    src/util/error.c
    src/util/hash.c
    src/util/log.c
)
target_include_directories(tdb_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
# Optional libs (zstd / OpenSSL) link into tdb_core when the matching CMake
# option is set. Each option is OFF by default and the C sources fall back
# to the pure-C path, so the default build is unchanged.
if(TDB_WITH_ZSTD)
    target_link_libraries(tdb_core PUBLIC ${TDB_ZSTD_LIBS})
endif()
if(TDB_WITH_OPENSSL)
    target_link_libraries(tdb_core PUBLIC ${TDB_OPENSSL_LIBS})
endif()

# Index library (B+Tree, B-Tree, R+Tree, R-Tree, Hash)
add_library(tdb_index STATIC
    src/index/bptree/bptree.c
    src/index/btree/btree.c
    src/index/rtree/rtree.c
    src/index/rptree/rptree.c
    src/index/hash/hash_index.c
)
target_link_libraries(tdb_index PRIVATE tdb_core m)

# SQL C++ library (lexer, parser, AST, optimizer, executor, type coercion, tuple serialization)
add_library(tdb_sql STATIC
    src/sql/lexer/lexer.cpp
    src/sql/parser/parser.cpp
    src/sql/ast/ast.cpp
    src/sql/optimizer/optimizer.cpp
    src/sql/executor/executor.cpp
    src/sql/typecoerce/typecoerce.cpp
    src/storage/tuple.cpp
)
target_link_libraries(tdb_sql PRIVATE tdb_core tdb_index)

# Catalog C++ library
add_library(tdb_catalog STATIC
    src/catalog/catalog.cpp
)
target_link_libraries(tdb_catalog PRIVATE tdb_core tdb_sql)

# Document query engine (XPath, XQuery, GraphQL on JSON/XML columns)
add_library(tdb_doc STATIC
    src/doc/docquery.cpp
)
target_link_libraries(tdb_doc PRIVATE tdb_sql tdb_catalog)

# ─── LLVM JIT compiler (optional) ────────────────────────────────────────
if(TDB_WITH_LLVM)
    set(LLVM_DIR "/opt/homebrew/opt/llvm/lib/cmake/llvm" CACHE PATH "LLVM CMake dir")
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

    add_compile_definitions(TDB_WITH_LLVM=1)

    llvm_map_components_to_libnames(LLVM_LIBS
        core orcjit native nativecodegen support
        irreader passes target
    )

    add_library(tdb_jit STATIC
        src/script/jit/lua_jit.cpp
        src/script/jit/ir_emitter.cpp
        src/script/jit/trampolines.cpp
    )
    target_include_directories(tdb_jit PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/lua/src
        ${LLVM_INCLUDE_DIRS}
    )
    target_include_directories(tdb_jit PUBLIC ${PROJECT_SOURCE_DIR}/include)
    # LLVM headers use -fno-rtti; match it for the JIT TU
    target_compile_options(tdb_jit PRIVATE
        -Wno-error -Wno-unused-parameter -Wno-covered-switch-default
        -Wno-switch-enum
    )
    target_link_libraries(tdb_jit PUBLIC tdb_lua PRIVATE ${LLVM_LIBS} tdb_sql tdb_catalog)
endif()

# ─── JavaScriptCore engine (optional, Apple platforms) ───────────────────
if(TDB_WITH_JSC)
    if(NOT APPLE)
        message(FATAL_ERROR "TDB_WITH_JSC requires macOS (JavaScriptCore.framework)")
    endif()
    add_compile_definitions(TDB_WITH_JSC=1)

    add_library(tdb_jsc STATIC
        src/script/js/jsengine.mm
    )
    target_include_directories(tdb_jsc PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(tdb_jsc PUBLIC tdb_sql tdb_catalog)
    target_link_libraries(tdb_jsc PRIVATE "-framework JavaScriptCore" "-framework Foundation")
    target_compile_options(tdb_jsc PRIVATE
        -Wno-error -fobjc-arc
        -Wno-unused-parameter
    )
    set_target_properties(tdb_jsc PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )
endif()

# ─── Lua scripting engine (CREATE SCRIPT / UDFs / triggers / JS bridge) ──
add_library(tdb_script STATIC
    src/script/script_engine.cpp
)
target_link_libraries(tdb_script PUBLIC tdb_lua PRIVATE tdb_sql tdb_catalog)

if(TDB_WITH_LLVM)
    target_link_libraries(tdb_script PUBLIC tdb_jit)
    target_compile_definitions(tdb_script PRIVATE TDB_WITH_LLVM=1)
endif()

if(TDB_WITH_JSC)
    target_link_libraries(tdb_script PUBLIC tdb_jsc)
    target_compile_definitions(tdb_script PRIVATE TDB_WITH_JSC=1)
    target_link_libraries(tdb_script PRIVATE "-framework JavaScriptCore" "-framework Foundation")
endif()

# Persistence bridge (C storage layer <-> C++ Database class)
add_library(tdb_persistence STATIC
    src/persistence/persistence.cpp
)
target_link_libraries(tdb_persistence PRIVATE tdb_core tdb_sql tdb_catalog)

# Main embedded library combining everything
add_library(tdb STATIC
    src/tdb.cpp
    src/util/threadsafe.cpp
    src/util/migrate.cpp
    src/dbfile/dbfile.cpp
    src/dbfile/dbfile_v1.cpp
    src/dbfile/dbfile_v2.cpp
)
target_link_libraries(tdb PUBLIC tdb_sql tdb_catalog tdb_doc tdb_script tdb_persistence tdb_core tdb_index)

# CLI binary
add_executable(tdbcli src/cli/tdbcli.cpp)
target_link_libraries(tdbcli PRIVATE tdb)

# TUI shell (rich interactive terminal)
add_executable(tdb-tui src/cli/tui_shell.cpp)
target_link_libraries(tdb-tui PRIVATE tdb)

# Install targets
install(TARGETS tdbcli tdb-tui RUNTIME DESTINATION bin)
install(TARGETS tdb tdb_sql tdb_catalog tdb_doc tdb_persistence tdb_core tdb_index
        ARCHIVE DESTINATION lib)
install(DIRECTORY include/tdb DESTINATION include)

# Tests
if(TDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Objective-C framework — Apple platforms only. The subdir CMakeLists has
# an early return on non-Apple so this is safe to include unconditionally.
if(APPLE)
    add_subdirectory(src/objc)
endif()
