cmake_minimum_required(VERSION 3.16)
project(TransactionDB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TDB_BUILD_TESTS   "Build the test suite" ON)
option(TDB_BUILD_CLI     "Build the CLI shell" ON)
option(TDB_BUILD_LUA     "Build with embedded Lua" ON)
option(TDB_BUILD_OPENSSL "Build crypto functions against OpenSSL (AES, CSPRNG)" OFF)

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

add_library(transactiondb STATIC ${TDB_SOURCES})
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
          "${CMAKE_SOURCE_DIR}/src/ext"
          "${CMAKE_SOURCE_DIR}/src/lua"
          "${CMAKE_SOURCE_DIR}/src/api")
target_link_libraries(transactiondb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
  target_link_libraries(transactiondb PUBLIC m)
endif()
tdb_set_warnings(transactiondb)

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
                plsql composite crypto)
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
        "${CMAKE_SOURCE_DIR}/src/plsql")
      add_test(NAME ${t} COMMAND test_${t})
    endif()
  endforeach()

  # C++ wrapper test
  add_executable(test_cpp tests/test_cpp.cpp)
  target_link_libraries(test_cpp PRIVATE transactiondb)
  target_include_directories(test_cpp PRIVATE
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/tests")
  add_test(NAME cpp COMMAND test_cpp)

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
