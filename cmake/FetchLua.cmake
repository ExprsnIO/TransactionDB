# FetchLua.cmake — vendor Lua 5.4 and compile it into a static library `lua_static`.
#
# Upstream Lua ships a hand-written Makefile, not a CMake build, so we fetch the
# source tarball and compile its core .c files (minus the `lua`/`luac` mains)
# ourselves. The URL_HASH pins the exact release for reproducible builds.
#
# If a vendored copy already exists under third_party/lua/src (e.g. a submodule
# populated offline), that is used instead of downloading.

include(FetchContent)

set(TDB_LUA_LOCAL "${CMAKE_SOURCE_DIR}/third_party/lua")

if(EXISTS "${TDB_LUA_LOCAL}/src/lua.h")
  message(STATUS "TransactionDB: using vendored Lua at ${TDB_LUA_LOCAL}")
  set(lua_SOURCE_DIR "${TDB_LUA_LOCAL}")
else()
  message(STATUS "TransactionDB: fetching Lua 5.4.7 via FetchContent")
  FetchContent_Declare(lua
    URL      https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)
  FetchContent_MakeAvailable(lua)
endif()

file(GLOB TDB_LUA_SRC "${lua_SOURCE_DIR}/src/*.c")
list(FILTER TDB_LUA_SRC EXCLUDE REGEX "(lua|luac)\\.c$")

add_library(lua_static STATIC ${TDB_LUA_SRC})
target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}/src")

if(UNIX AND NOT APPLE)
  target_compile_definitions(lua_static PUBLIC LUA_USE_LINUX)
elseif(APPLE)
  target_compile_definitions(lua_static PUBLIC LUA_USE_MACOSX)
endif()

# Lua's own source is not our concern for warnings.
if(NOT MSVC)
  target_compile_options(lua_static PRIVATE -w)
endif()
