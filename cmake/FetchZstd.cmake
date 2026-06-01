# FetchZstd.cmake — make Zstandard available as the `zstd::libzstd` target.
#
# Strategy:
#   1. If a system zstd is installed, use it (find_package(zstd) or pkg-config).
#   2. Otherwise, allow the user to point us at a prebuilt copy via
#      TDB_ZSTD_ROOT (header path / library path).
#   3. As a last resort, fetch and build zstd from source via FetchContent —
#      the build is offline-friendly when TDB_ZSTD_SOURCE_DIR points at a
#      local checkout (e.g. third_party/zstd).
#
# In all cases the compiled target should expose an interface library
# usable as `target_link_libraries(<tgt> PRIVATE tdb_zstd)`.

include(FetchContent)

set(TDB_ZSTD_VERSION "1.5.6" CACHE STRING "Zstandard release tag for FetchContent fallback")
set(TDB_ZSTD_SOURCE_DIR "" CACHE PATH "Local Zstandard source tree (optional)")

# 1. system install via the modern config package
find_package(zstd CONFIG QUIET)
if(zstd_FOUND AND TARGET zstd::libzstd_shared)
  add_library(tdb_zstd INTERFACE)
  target_link_libraries(tdb_zstd INTERFACE zstd::libzstd_shared)
  return()
elseif(zstd_FOUND AND TARGET zstd::libzstd_static)
  add_library(tdb_zstd INTERFACE)
  target_link_libraries(tdb_zstd INTERFACE zstd::libzstd_static)
  return()
endif()

# 2. system install via pkg-config
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD_PC QUIET libzstd)
  if(ZSTD_PC_FOUND)
    add_library(tdb_zstd INTERFACE)
    target_include_directories(tdb_zstd INTERFACE ${ZSTD_PC_INCLUDE_DIRS})
    target_link_libraries(tdb_zstd INTERFACE ${ZSTD_PC_LIBRARIES})
    target_link_directories(tdb_zstd INTERFACE ${ZSTD_PC_LIBRARY_DIRS})
    return()
  endif()
endif()

# 3. FetchContent fallback (build zstd from source).
if(TDB_ZSTD_SOURCE_DIR AND EXISTS "${TDB_ZSTD_SOURCE_DIR}/lib/zstd.h")
  message(STATUS "Using local Zstandard at ${TDB_ZSTD_SOURCE_DIR}")
  FetchContent_Declare(tdb_fc_zstd SOURCE_DIR "${TDB_ZSTD_SOURCE_DIR}")
else()
  FetchContent_Declare(
    tdb_fc_zstd
    URL "https://github.com/facebook/zstd/releases/download/v${TDB_ZSTD_VERSION}/zstd-${TDB_ZSTD_VERSION}.tar.gz"
  )
endif()

# zstd ships its own CMakeLists under build/cmake/ — point FetchContent at it.
FetchContent_GetProperties(tdb_fc_zstd)
if(NOT tdb_fc_zstd_POPULATED)
  FetchContent_Populate(tdb_fc_zstd)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
  add_subdirectory("${tdb_fc_zstd_SOURCE_DIR}/build/cmake"
                   "${tdb_fc_zstd_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

add_library(tdb_zstd INTERFACE)
target_link_libraries(tdb_zstd INTERFACE libzstd_static)
target_include_directories(tdb_zstd INTERFACE "${tdb_fc_zstd_SOURCE_DIR}/lib")
