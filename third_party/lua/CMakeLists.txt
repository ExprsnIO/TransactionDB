# Lua 5.4.7 — vendored for TDB
# Builds as a static library (tdb_lua). License: MIT (see ./LICENSE).
#
# The lua.c (REPL) and luac.c (offline compiler) are intentionally excluded —
# we only need the embeddable core + standard libraries.

set(LUA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

file(GLOB LUA_SOURCES "${LUA_SRC}/*.c")

add_library(tdb_lua STATIC ${LUA_SOURCES})

target_include_directories(tdb_lua PUBLIC ${LUA_SRC})

# Platform configuration
if(APPLE)
    target_compile_definitions(tdb_lua PUBLIC LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(tdb_lua PUBLIC LUA_USE_LINUX)
endif()

# Lua's source uses some patterns that trip TDB's strict warnings.
# Disable -Werror and a few specific warnings for the vendored TU only.
target_compile_options(tdb_lua PRIVATE
    -Wno-error
    -Wno-implicit-fallthrough
    -Wno-deprecated-declarations
    -Wno-string-plus-int
    -Wno-misleading-indentation
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-parameter
)

# Lua expects C99+; TDB sets C11 globally which is fine.
set_target_properties(tdb_lua PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
)
