/*
 * trampolines.h -- C-linkage bridge functions for JIT-compiled Lua code.
 *
 * These functions are registered as absolute symbols in the LLVM ORC JIT
 * session. JIT-compiled code emits `call` instructions directly to these
 * when it encounters db.execute(), db.query(), etc.
 */
#ifndef TDB_JIT_TRAMPOLINES_H
#define TDB_JIT_TRAMPOLINES_H

#include "tdb/jit.h"
#include <cstdint>

extern "C" {

// ─── Database bridge trampolines ───────────────────────────────────────
// The `state` parameter is a pointer to ScriptEngine::State.

// db.execute(sql) -> rows_affected (integer)
int64_t jit_db_execute(void *state, const char *sql, int64_t sql_len);

// db.query(sql) -> result as JITValue (table/array)
tdb::script::JITValue jit_db_query(void *state, const char *sql, int64_t sql_len);

// db.now() -> milliseconds since epoch
int64_t jit_db_now(void *state);

// db.user() -> username string
tdb::script::JITValue jit_db_user(void *state);

// db.log(msg) -> void
void jit_db_log(void *state, const char *msg, int64_t msg_len);

// ─── Lua runtime helper trampolines ────────────────────────────────────

// String concatenation: a .. b
tdb::script::JITValue jit_lua_concat(tdb::script::JITValue *a,
                                      tdb::script::JITValue *b);

// # operator (length of string or table)
tdb::script::JITValue jit_lua_len(tdb::script::JITValue *v);

// tostring() / tonumber() coercions
tdb::script::JITValue jit_lua_tostring(tdb::script::JITValue *v);
tdb::script::JITValue jit_lua_tonumber(tdb::script::JITValue *v);

// math.pow (Lua ^ operator)
double jit_lua_pow(double base, double exp);

// Type name for error messages
tdb::script::JITValue jit_lua_type(tdb::script::JITValue *v);

// print() — maps to db.log if available, else stdout
void jit_lua_print(void *state, tdb::script::JITValue *args, int32_t nargs);

// Comparison helper for mixed types
int32_t jit_lua_compare(tdb::script::JITValue *a, tdb::script::JITValue *b);

} // extern "C"

#endif // TDB_JIT_TRAMPOLINES_H
