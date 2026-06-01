cmake_minimum_required(VERSION 3.16)
project(TransactionDB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TDB_BUILD_TESTS     "Build the test suite" ON)
option(TDB_BUILD_CLI       "Build the CLI shell" ON)
option(TDB_BUILD_LUA       "Build with embedded Lua" ON)
option(TDB_BUILD_OPENSSL   "Build crypto functions against OpenSSL (AES, CSPRNG)" OFF)
option(TDB_BUILD_ZSTD      "Build with Zstandard page/blob compression" OFF)
# LLVM-IR + LLVM-ORC JIT for PL/SQL, Lua, SQL, XPath and XQuery. When OFF the
# JIT sources still compile (frontends still produce IR text) but the runtime
# JIT entry points return TDB_UNSUPPORTED instead of executing.
option(TDB_BUILD_LLVM      "Build the LLVM-IR + LLJIT compiler backend" OFF)
# Package as an Apple .framework bundle (no-op off Apple); see cmake/AppleFramework.cmake.
option(TDB_BUILD_FRAMEWORK "Build as an Apple .framework bundle" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TDB_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.c")

# Lua integration sources are only compiled when Lua is enabled.
if(NOT TDB_BUILD_LUA)
  list(FILTER TDB_SOURCES EXCLUDE REGEX "/src/lua/")
endif()

# An Apple framework bundle must be a shared library; otherwise stay static.
if(APPLE AND TDB_BUILD_FRAMEWORK)
  set(TDB_LIB_TYPE SHARED)
else()
  set(TDB_LIB_TYPE STATIC)
endif()

add_library(transactiondb ${TDB_LIB_TYPE} ${TDB_SOURCES})
target_include_directories(transactiondb
  PUBLIC  "${CMAKE_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/src"
          "${CMAKE_SOURCE_DIR}/src/common"
          "${CMAKE_SOURCE_DIR}/src/value"
          "${CMAKE_SOURCE_DIR}/src/storage"
          "${CMAKE_SOURCE_DIR}/src/txn"
          "${CMAKE_SOURCE_DIR}/src/catalog"
          "${CMAKE_SOURCE_DIR}/src/sql"
          "${CMAKE_SOURCE_DIR}/src/plsql"
          "${CMAKE_SOURCE_DIR}/src/jit"
          "${CMAKE_SOURCE_DIR}/src/ext"
          "${CMAKE_SOURCE_DIR}/src/lua"
          "${CMAKE_SOURCE_DIR}/src/api")
target_link_libraries(transactiondb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
  target_link_libraries(transactiondb PUBLIC m)
endif()
tdb_set_warnings(transactiondb)

# Apple .framework packaging (no-op unless APPLE AND TDB_BUILD_FRAMEWORK).
include(AppleFramework)

if(TDB_BUILD_LUA)
  include(FetchLua)
  target_link_libraries(transactiondb PRIVATE lua_static)
  target_compile_definitions(transactiondb PRIVATE TDB_HAVE_LUA=1)
endif()

if(TDB_BUILD_OPENSSL)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(transactiondb PUBLIC OpenSSL::Crypto)
  target_compile_definitions(transactiondb PRIVATE TDB_HAVE_OPENSSL=1)
endif()

if(TDB_BUILD_ZSTD)
  include(FetchZstd)
  target_link_libraries(transactiondb PRIVATE tdb_zstd)
  target_compile_definitions(transactiondb PRIVATE TDB_HAVE_ZSTD=1)
endif()

# ---------------------------------------------------------------------------
# LLVM-IR + LLJIT backend (optional)
#
# When enabled, the JIT module (src/jit/) links against libLLVM and exposes
# a runtime compiler for PL/SQL, Lua, SQL, XPath and XQuery via the
# tdb_jit_* API in src/jit/tdb_jit.h. When disabled, the same sources are
# compiled but the runtime entry points return TDB_UNSUPPORTED, so any code
# that already depends on the JIT keeps building.
# ---------------------------------------------------------------------------
if(TDB_BUILD_LLVM)
  include(FindLLVMOpt)
  tdb_find_llvm(REQUIRED)
  target_include_directories(transactiondb PRIVATE ${TDB_LLVM_INCLUDE_DIRS})
  # PUBLIC: transactiondb is a STATIC library and the JIT sources keep
  # unresolved LLVM symbols which must be linked by the final consumer
  # (test executables, the shell, downstream embedders). Propagating the
  # library directory and the `-lLLVM-N` flag through the interface ensures
  # every consumer's link line picks them up.
  target_link_directories(transactiondb PUBLIC ${TDB_LLVM_LIBRARY_DIRS})
  target_link_libraries(transactiondb PUBLIC ${TDB_LLVM_LIBS})
  target_compile_definitions(transactiondb PRIVATE TDB_HAVE_LLVM=1)
endif()

# ---------------------------------------------------------------------------
# CLI shell
# ---------------------------------------------------------------------------
if(TDB_BUILD_CLI)
  add_executable(tdb_shell cli/shell.c)
  target_link_libraries(tdb_shell PRIVATE transactiondb)
  target_include_directories(tdb_shell PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/src/api"
    "${CMAKE_SOURCE_DIR}/src/catalog"
    "${CMAKE_SOURCE_DIR}/src/value"
    "${CMAKE_SOURCE_DIR}/src/storage"
    "${CMAKE_SOURCE_DIR}/src/txn"
    "${CMAKE_SOURCE_DIR}/src/sql")
  tdb_set_warnings(tdb_shell)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TDB_BUILD_TESTS)
  enable_testing()
  set(TDB_TESTS util value_record wal pager sqltype schema btree
                mvcc lock engine catalog lexer parser exec concurrency geom rtree
                plsql composite crypto routines keywords compress sqlite_compat
                jit)
  foreach(t ${TDB_TESTS})
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_${t}.c")
      add_executable(test_${t} "tests/test_${t}.c")
      target_link_libraries(test_${t} PRIVATE transactiondb)
      target_include_directories(test_${t} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/tests"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/common"
        "${CMAKE_SOURCE_DIR}/src/value"
        "${CMAKE_SOURCE_DIR}/src/storage"
        "${CMAKE_SOURCE_DIR}/src/txn"
        "${CMAKE_SOURCE_DIR}/src/catalog"
        "${CMAKE_SOURCE_DIR}/src/sql"
        "${CMAKE_SOURCE_DIR}/src/plsql"
        "${CMAKE_SOURCE_DIR}/src/jit")
      add_test(NAME ${t} COMMAND test_${t})
    endif()
  endforeach()

  # When the LLVM backend is enabled, propagate the define so the JIT test
  # can assert real codegen (not just the stubbed TDB_UNSUPPORTED path).
  if(TDB_BUILD_LLVM AND TARGET test_jit)
    target_compile_definitions(test_jit PRIVATE TDB_HAVE_LLVM=1)
  endif()

  # C++ wrapper test
  add_executable(test_cpp tests/test_cpp.cpp)
  target_link_libraries(test_cpp PRIVATE transactiondb)
  target_include_directories(test_cpp PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/tests")
  add_test(NAME cpp COMMAND test_cpp)

  # The OpenSSL-dependent assertions in test_crypto.c are guarded by
  # TDB_HAVE_OPENSSL; propagate the define when the parent build has it.
  if(TARGET test_crypto AND TDB_BUILD_OPENSSL)
    target_compile_definitions(test_crypto PRIVATE TDB_HAVE_OPENSSL=1)
  endif()

  # Example C++ plugin (shared module) + dynamic load test. The plugin's tdb_*
  # symbols are resolved from the host test executable at dlopen time, so
  # test_crypto is built with exported symbols (-rdynamic).
  if(TARGET test_crypto AND NOT WIN32)
    add_library(tdb_plugin_example MODULE examples/tdb_plugin_example.cpp)
    target_include_directories(tdb_plugin_example PRIVATE "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(tdb_plugin_example PROPERTIES PREFIX "")
    set_target_properties(test_crypto PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(test_crypto tdb_plugin_example)
    target_compile_definitions(test_crypto PRIVATE
      TDB_PLUGIN_PATH="$<TARGET_FILE:tdb_plugin_example>")
  endif()

  # Lua integration test (only when Lua is built in)
  if(TDB_BUILD_LUA)
    add_executable(test_lua tests/test_lua.c)
    target_link_libraries(test_lua PRIVATE transactiondb)
    target_include_directories(test_lua PRIVATE
      "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/tests")
    add_test(NAME lua COMMAND test_lua)
  endif()
endif()
