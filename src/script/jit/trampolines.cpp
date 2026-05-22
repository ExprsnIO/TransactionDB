/*
 * trampolines.cpp -- C-linkage bridge functions for JIT-compiled Lua code.
 *
 * Each trampoline receives an opaque `state` pointer which is actually
 * a ScriptEngine::State*. This avoids exposing ScriptEngine internals
 * to LLVM-generated code.
 */

#include "trampolines.h"
#include "tdb/script.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// We need access to the ScriptEngine::State to call the SQL bridge.
// The State struct is defined in script_engine.cpp. We use the same
// layout contract through the public header's forward declaration.
// The JIT stores the State* as the opaque `state` parameter.

namespace {

// Helper to get the SQL execution callback from the opaque state.
// The state pointer layout: first member is lua_State*, second is
// ScriptEngineOptions. We access opts.execute_fn through a reinterpret.
// This is safe because we control the struct layout.
struct BridgeState {
    void *lua_state;     // lua_State* (unused here)
    tdb::script::ScriptEngineOptions opts;
};

static BridgeState *as_bridge(void *state) {
    return static_cast<BridgeState *>(state);
}

// Thread-local string buffers for returning strings from trampolines.
// The JITValue.sval pointer must remain valid until the caller copies it.
static thread_local std::string tl_buf1;
static thread_local std::string tl_buf2;
static thread_local std::string tl_concat_buf;

} // anon

// ─── Database bridge trampolines ───────────────────────────────────────

extern "C" int64_t jit_db_execute(void *state, const char *sql, int64_t sql_len) {
    auto *bs = as_bridge(state);
    if (!bs || !bs->opts.execute_fn) return -1;
    std::string sql_str(sql, static_cast<size_t>(sql_len));
    auto rs = bs->opts.execute_fn(sql_str);
    if (!rs.success) return -1;
    return rs.rows_affected;
}

extern "C" tdb::script::JITValue jit_db_query(void *state, const char *sql,
                                                int64_t sql_len) {
    auto *bs = as_bridge(state);
    if (!bs || !bs->opts.execute_fn)
        return tdb::script::JITValue::make_nil();

    std::string sql_str(sql, static_cast<size_t>(sql_len));
    auto rs = bs->opts.execute_fn(sql_str);
    if (!rs.success)
        return tdb::script::JITValue::make_nil();

    // Encode result as a JSON-like string for the JIT to consume.
    // Full table marshalling would require a Lua table trampoline;
    // for now, return the row count as an integer. Complex results
    // should go through db.execute() which returns rows_affected.
    return tdb::script::JITValue::make_int(static_cast<int64_t>(rs.rows.size()));
}

extern "C" int64_t jit_db_now(void * /*state*/) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

extern "C" tdb::script::JITValue jit_db_user(void *state) {
    auto *bs = as_bridge(state);
    if (bs && bs->opts.current_user_fn) {
        tl_buf1 = bs->opts.current_user_fn();
    } else {
        tl_buf1 = "anonymous";
    }
    return tdb::script::JITValue::make_string(tl_buf1.c_str(),
                                               static_cast<int64_t>(tl_buf1.size()));
}

extern "C" void jit_db_log(void *state, const char *msg, int64_t msg_len) {
    auto *bs = as_bridge(state);
    if (bs && bs->opts.log_fn) {
        bs->opts.log_fn(std::string(msg, static_cast<size_t>(msg_len)));
    }
}

// ─── Lua runtime helper trampolines ────────────────────────────────────

extern "C" tdb::script::JITValue jit_lua_concat(tdb::script::JITValue *a,
                                                  tdb::script::JITValue *b) {
    // Convert both to string representation and concatenate.
    auto to_str = [](const tdb::script::JITValue &v) -> std::string {
        switch (v.type) {
        case tdb::script::JVT_NIL:    return "nil";
        case tdb::script::JVT_BOOL:   return v.ival ? "true" : "false";
        case tdb::script::JVT_INT:    return std::to_string(v.ival);
        case tdb::script::JVT_FLOAT:  return std::to_string(v.fval);
        case tdb::script::JVT_STRING: return std::string(v.sval, static_cast<size_t>(v.ival));
        default: return "";
        }
    };
    tl_concat_buf = to_str(*a) + to_str(*b);
    return tdb::script::JITValue::make_string(
        tl_concat_buf.c_str(), static_cast<int64_t>(tl_concat_buf.size()));
}

extern "C" tdb::script::JITValue jit_lua_len(tdb::script::JITValue *v) {
    if (v->type == tdb::script::JVT_STRING) {
        return tdb::script::JITValue::make_int(v->ival); // ival = string length
    }
    return tdb::script::JITValue::make_int(0);
}

extern "C" tdb::script::JITValue jit_lua_tostring(tdb::script::JITValue *v) {
    switch (v->type) {
    case tdb::script::JVT_STRING: return *v;
    case tdb::script::JVT_INT:
        tl_buf1 = std::to_string(v->ival);
        return tdb::script::JITValue::make_string(tl_buf1.c_str(),
                                                    static_cast<int64_t>(tl_buf1.size()));
    case tdb::script::JVT_FLOAT:
        tl_buf1 = std::to_string(v->fval);
        return tdb::script::JITValue::make_string(tl_buf1.c_str(),
                                                    static_cast<int64_t>(tl_buf1.size()));
    case tdb::script::JVT_BOOL:
        return tdb::script::JITValue::make_string(
            v->ival ? "true" : "false", v->ival ? 4 : 5);
    default:
        return tdb::script::JITValue::make_string("nil", 3);
    }
}

extern "C" tdb::script::JITValue jit_lua_tonumber(tdb::script::JITValue *v) {
    switch (v->type) {
    case tdb::script::JVT_INT:   return *v;
    case tdb::script::JVT_FLOAT: return *v;
    case tdb::script::JVT_STRING: {
        std::string s(v->sval, static_cast<size_t>(v->ival));
        try {
            size_t pos = 0;
            int64_t iv = std::stoll(s, &pos);
            if (pos == s.size()) return tdb::script::JITValue::make_int(iv);
        } catch (...) {}
        try {
            double fv = std::stod(s);
            return tdb::script::JITValue::make_float(fv);
        } catch (...) {}
        return tdb::script::JITValue::make_nil();
    }
    default: return tdb::script::JITValue::make_nil();
    }
}

extern "C" double jit_lua_pow(double base, double exp) {
    return std::pow(base, exp);
}

extern "C" tdb::script::JITValue jit_lua_type(tdb::script::JITValue *v) {
    const char *name = "nil";
    int64_t len = 3;
    switch (v->type) {
    case tdb::script::JVT_NIL:    name = "nil";     len = 3; break;
    case tdb::script::JVT_BOOL:   name = "boolean"; len = 7; break;
    case tdb::script::JVT_INT:    name = "number";  len = 6; break;
    case tdb::script::JVT_FLOAT:  name = "number";  len = 6; break;
    case tdb::script::JVT_STRING: name = "string";  len = 6; break;
    case tdb::script::JVT_TABLE:  name = "table";   len = 5; break;
    }
    return tdb::script::JITValue::make_string(name, len);
}

extern "C" void jit_lua_print(void *state, tdb::script::JITValue *args,
                               int32_t nargs) {
    std::string out;
    for (int32_t i = 0; i < nargs; i++) {
        if (i > 0) out += '\t';
        auto s = jit_lua_tostring(&args[i]);
        if (s.sval) out.append(s.sval, static_cast<size_t>(s.ival));
    }
    auto *bs = as_bridge(state);
    if (bs && bs->opts.log_fn) {
        bs->opts.log_fn(out);
    } else {
        std::puts(out.c_str());
    }
}

extern "C" int32_t jit_lua_compare(tdb::script::JITValue *a,
                                    tdb::script::JITValue *b) {
    if (a->type != b->type) {
        // Coerce int <-> float
        if (a->type == tdb::script::JVT_INT && b->type == tdb::script::JVT_FLOAT) {
            double av = static_cast<double>(a->ival);
            return av < b->fval ? -1 : (av > b->fval ? 1 : 0);
        }
        if (a->type == tdb::script::JVT_FLOAT && b->type == tdb::script::JVT_INT) {
            double bv = static_cast<double>(b->ival);
            return a->fval < bv ? -1 : (a->fval > bv ? 1 : 0);
        }
        return a->type < b->type ? -1 : 1;
    }
    switch (a->type) {
    case tdb::script::JVT_NIL:    return 0;
    case tdb::script::JVT_BOOL:   return (a->ival > b->ival) - (a->ival < b->ival);
    case tdb::script::JVT_INT:    return (a->ival > b->ival) - (a->ival < b->ival);
    case tdb::script::JVT_FLOAT:  return (a->fval > b->fval) - (a->fval < b->fval);
    case tdb::script::JVT_STRING: {
        size_t la = static_cast<size_t>(a->ival);
        size_t lb = static_cast<size_t>(b->ival);
        size_t mn = la < lb ? la : lb;
        int c = std::memcmp(a->sval, b->sval, mn);
        if (c != 0) return c < 0 ? -1 : 1;
        return (la > lb) - (la < lb);
    }
    default: return 0;
    }
}
