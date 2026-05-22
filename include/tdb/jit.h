/*
 * jit.h -- Lua-to-LLVM-IR JIT compiler for TDB ScriptEngine.
 *
 * Compiles Lua 5.4 bytecodes to native machine code via LLVM ORC JIT.
 * The JIT is opt-in (TDB_WITH_LLVM=ON) and transparent to callers --
 * the ScriptEngine automatically dispatches to compiled code when
 * available, falling back to the Lua interpreter for unsupported
 * constructs.
 *
 * Copyright 2026 Rick Holland. Apache 2.0.
 */
#ifndef TDB_JIT_H
#define TDB_JIT_H

#include "tdb/catalog.h"
#include "tdb/sql/executor.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declare lua_State to avoid pulling in Lua headers
struct lua_State;

namespace tdb::script {

// ─── JITValue: C-compatible tagged union for JIT boundary ───────────────
// This is the runtime representation of values inside JIT-compiled code.
// It mirrors sql::Value but uses a flat C struct that LLVM IR can model.

enum JITValueType : int32_t {
    JVT_NIL    = 0,
    JVT_BOOL   = 1,
    JVT_INT    = 2,
    JVT_FLOAT  = 3,
    JVT_STRING = 4,
    JVT_TABLE  = 5,   // opaque pointer to Lua table or result set
};

struct JITValue {
    JITValueType type;
    int64_t      ival;     // integer, bool (0/1), or string length
    double       fval;     // float value
    const char  *sval;     // string data pointer (NOT owned)

    static JITValue make_nil()                          { return {JVT_NIL, 0, 0.0, nullptr}; }
    static JITValue make_bool(bool b)                   { return {JVT_BOOL, b ? 1 : 0, 0.0, nullptr}; }
    static JITValue make_int(int64_t v)                 { return {JVT_INT, v, 0.0, nullptr}; }
    static JITValue make_float(double v)                { return {JVT_FLOAT, 0, v, nullptr}; }
    static JITValue make_string(const char *s, int64_t len) { return {JVT_STRING, len, 0.0, s}; }
};

// ─── Marshalling helpers ────────────────────────────────────────────────

JITValue   marshal_to_jit(const sql::Value &v);
sql::Value marshal_from_jit(const JITValue &jv);

// ─── Forward declarations for internals ────────────────────────────────

struct ScriptEngineState;   // defined in script_engine.cpp

// ─── Native function signature ─────────────────────────────────────────
// Every JIT-compiled Lua function has this signature.
//   state  — opaque pointer to ScriptEngine::State (for db.* trampolines)
//   args   — array of JITValue arguments
//   nargs  — number of arguments
//   result — pointer to JITValue to store the return value
//   returns 0 on success, non-zero on error
using JITNativeFunc = int32_t(*)(void *state, JITValue *args,
                                  int32_t nargs, JITValue *result);

// ─── LuaJIT compiler class ─────────────────────────────────────────────

class LuaJIT {
public:
    LuaJIT();
    ~LuaJIT();

    LuaJIT(const LuaJIT &) = delete;
    LuaJIT &operator=(const LuaJIT &) = delete;

    // Returns true if the LLVM JIT initialized successfully.
    bool available() const;

    // Compile a Lua script to native code. Uses luaL_loadbuffer on the
    // provided lua_State to get bytecodes, then translates to LLVM IR.
    struct CompileResult {
        JITNativeFunc func = nullptr;
        bool failed_permanently = false;  // true = don't retry (unsupported opcode)
        std::string error;
    };

    CompileResult compile(lua_State *L, const catalog::ScriptInfo &script);

    // Look up a previously compiled function by name + version.
    JITNativeFunc lookup(const std::string &name,
                         const ObjectVersion &ver) const;

    // Register the bridge state pointer for trampoline callbacks.
    void set_bridge_state(void *state);

    // Statistics
    struct Stats {
        uint64_t compiled_count    = 0;
        uint64_t fallback_count    = 0;
        uint64_t cache_hits        = 0;
        uint64_t total_compile_us  = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tdb::script

#endif // TDB_JIT_H
