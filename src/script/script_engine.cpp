/*
 * script_engine.cpp — Multi-language script host.
 *
 * Owns a Lua 5.4 interpreter (always available), an optional LLVM JIT compiler
 * for Lua (when TDB_WITH_LLVM=1), and a JavaScriptCore engine (on Apple
 * platforms via TDB_WITH_JSC=1). The SQL bridge is wired via caller-supplied
 * callbacks to avoid a dependency cycle on Database.
 *
 * Copyright 2026 Rick Holland. Apache 2.0.
 */

#include "tdb/script.h"
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <unordered_map>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#ifdef TDB_WITH_LLVM
#include "tdb/jit.h"
#endif

#ifdef TDB_WITH_JSC
#include "tdb/jsengine.h"
#endif

namespace tdb::script {

// ─── lua_State wrapper ──────────────────────────────────────────────────
struct ScriptEngine::State {
    lua_State *L = nullptr;
    ScriptEngineOptions opts;
    TriggerContext *active_trigger = nullptr;

    // JIT state
    JITMode jit_mode = JITMode::AUTO;
#ifdef TDB_WITH_LLVM
    std::unique_ptr<LuaJIT> jit;
    std::unordered_map<std::string, uint32_t> call_counts;
    static constexpr uint32_t JIT_THRESHOLD = 3;
#endif

    // JavaScript engine
#ifdef TDB_WITH_JSC
    std::unique_ptr<JSEngine> js;
#endif
};

// ─── Value <-> Lua bridging ─────────────────────────────────────────────

static void push_value(lua_State *L, const sql::Value &v) {
    switch (v.type) {
    case sql::Value::Type::NULL_VAL:     lua_pushnil(L); break;
    case sql::Value::Type::INT64:        lua_pushinteger(L, (lua_Integer)v.int_val); break;
    case sql::Value::Type::FLOAT64:      lua_pushnumber(L, (lua_Number)v.float_val); break;
    case sql::Value::Type::STRING:
    case sql::Value::Type::BLOB:
        lua_pushlstring(L, v.str_val.data(), v.str_val.size());
        break;
    case sql::Value::Type::BOOL:         lua_pushboolean(L, v.bool_val ? 1 : 0); break;
    case sql::Value::Type::DATE_VAL:
    case sql::Value::Type::TIME_VAL:
    case sql::Value::Type::TIMESTAMP_VAL:
        lua_pushinteger(L, (lua_Integer)v.int_val);
        break;
    default:
        lua_pushstring(L, v.to_string().c_str());
        break;
    }
}

static sql::Value pop_value(lua_State *L, int idx) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL:     return sql::Value::make_null();
    case LUA_TBOOLEAN: return sql::Value::make_bool(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) return sql::Value::make_int((int64_t)lua_tointeger(L, idx));
        return sql::Value::make_float((double)lua_tonumber(L, idx));
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        return sql::Value::make_string(std::string(s, len));
    }
    default: return sql::Value::make_null();
    }
}

// ─── `db` table functions ───────────────────────────────────────────────

static ScriptEngine::State *get_state(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "tdb_script_state");
    ScriptEngine::State *s = static_cast<ScriptEngine::State *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

static int l_db_execute(lua_State *L) {
    const char *sql = luaL_checkstring(L, 1);
    ScriptEngine::State *st = get_state(L);
    if (!st || !st->opts.execute_fn) {
        return luaL_error(L, "db.execute: no SQL bridge configured");
    }
    sql::ResultSet rs = st->opts.execute_fn(sql);
    if (!rs.success) {
        return luaL_error(L, "db.execute failed: %s", rs.error_message.c_str());
    }
    lua_pushinteger(L, (lua_Integer)rs.rows_affected);
    return 1;
}

static int l_db_query(lua_State *L) {
    const char *sql = luaL_checkstring(L, 1);
    ScriptEngine::State *st = get_state(L);
    if (!st || !st->opts.execute_fn) {
        return luaL_error(L, "db.query: no SQL bridge configured");
    }
    sql::ResultSet rs = st->opts.execute_fn(sql);
    if (!rs.success) {
        return luaL_error(L, "db.query failed: %s", rs.error_message.c_str());
    }
    lua_newtable(L);
    for (size_t i = 0; i < rs.rows.size(); i++) {
        lua_newtable(L);
        const auto &row = rs.rows[i];
        for (size_t c = 0; c < row.size() && c < rs.columns.size(); c++) {
            push_value(L, row[c]);
            lua_setfield(L, -2, rs.columns[c].name.c_str());
        }
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

static int l_db_now(lua_State *L) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

static int l_db_user(lua_State *L) {
    ScriptEngine::State *st = get_state(L);
    std::string u = (st && st->opts.current_user_fn) ? st->opts.current_user_fn() : "anonymous";
    lua_pushlstring(L, u.data(), u.size());
    return 1;
}

static int l_db_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    ScriptEngine::State *st = get_state(L);
    if (st && st->opts.log_fn) st->opts.log_fn(msg);
    return 0;
}

static void register_db_table(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_db_execute); lua_setfield(L, -2, "execute");
    lua_pushcfunction(L, l_db_query);   lua_setfield(L, -2, "query");
    lua_pushcfunction(L, l_db_now);     lua_setfield(L, -2, "now");
    lua_pushcfunction(L, l_db_user);    lua_setfield(L, -2, "user");
    lua_pushcfunction(L, l_db_log);     lua_setfield(L, -2, "log");
    lua_setglobal(L, "db");
}

// ─── ScriptEngine impl ──────────────────────────────────────────────────

ScriptEngine::ScriptEngine(const ScriptEngineOptions &opts) : impl_(new State) {
    impl_->opts = opts;
    impl_->jit_mode = opts.jit_mode;

    // Initialize Lua
    impl_->L = luaL_newstate();
    if (!impl_->L) throw std::runtime_error("luaL_newstate failed");
    luaL_openlibs(impl_->L);

    lua_pushlightuserdata(impl_->L, impl_.get());
    lua_setfield(impl_->L, LUA_REGISTRYINDEX, "tdb_script_state");
    register_db_table(impl_->L);

    // Initialize LLVM JIT (if compiled in)
#ifdef TDB_WITH_LLVM
    impl_->jit = std::make_unique<LuaJIT>();
    impl_->jit->set_bridge_state(impl_.get());
#endif

    // Initialize JavaScriptCore (if compiled in)
#ifdef TDB_WITH_JSC
    JSEngineOptions js_opts;
    js_opts.execute_fn = opts.execute_fn;
    js_opts.catalog_fn = opts.catalog_fn;
    js_opts.current_user_fn = opts.current_user_fn;
    js_opts.log_fn = opts.log_fn;
    impl_->js = std::make_unique<JSEngine>(js_opts);
#endif
}

ScriptEngine::~ScriptEngine() {
    // Destroy JIT and JS before Lua state
#ifdef TDB_WITH_LLVM
    impl_->jit.reset();
#endif
#ifdef TDB_WITH_JSC
    impl_->js.reset();
#endif
    if (impl_ && impl_->L) lua_close(impl_->L);
}

// ─── Lua compile_and_call (interpreter path) ───────────────────────────

static int compile_and_call(lua_State *L, const catalog::ScriptInfo &script,
                            const std::vector<sql::Value> &args,
                            TriggerContext *ctx) {
    std::string wrapped = "local function __tdb_script(";
    for (size_t i = 0; i < script.params.size(); i++) {
        if (i) wrapped += ", ";
        wrapped += script.params[i].name;
    }
    wrapped += ")\n";
    wrapped += script.lua_source;
    wrapped += "\nend\nreturn __tdb_script(...)";

    if (luaL_loadbuffer(L, wrapped.data(), wrapped.size(), script.name.c_str()) != LUA_OK) {
        return -1;
    }

    if (ctx) {
        lua_newtable(L);
        lua_pushstring(L, ctx->timing == catalog::TriggerTiming::BEFORE ? "BEFORE" : "AFTER");
        lua_setfield(L, -2, "timing");
        const char *evt = ctx->event == catalog::TriggerEvent::INSERT ? "INSERT" :
                          ctx->event == catalog::TriggerEvent::UPDATE ? "UPDATE" : "DELETE";
        lua_pushstring(L, evt); lua_setfield(L, -2, "event");
        lua_pushlstring(L, ctx->table.data(), ctx->table.size());
        lua_setfield(L, -2, "table");

        auto push_row = [&](const std::vector<sql::Value> &row, const char *field){
            if (row.empty()) { lua_pushnil(L); lua_setfield(L, -2, field); return; }
            lua_newtable(L);
            for (size_t i = 0; i < row.size(); i++) {
                push_value(L, row[i]);
                if (i < ctx->column_names.size()) {
                    lua_setfield(L, -2, ctx->column_names[i].c_str());
                } else {
                    lua_rawseti(L, -2, (lua_Integer)(i + 1));
                }
            }
            lua_setfield(L, -2, field);
        };
        push_row(ctx->new_row, "new");
        push_row(ctx->old_row, "old");
        lua_setglobal(L, "trigger");
    } else {
        lua_pushnil(L); lua_setglobal(L, "trigger");
    }

    for (auto &a : args) push_value(L, a);

    if (lua_pcall(L, (int)args.size(), 1, 0) != LUA_OK) {
        return -1;
    }

    if (ctx) {
        lua_getglobal(L, "trigger");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "new");
            if (lua_istable(L, -1) && !ctx->new_row.empty()) {
                for (size_t i = 0; i < ctx->column_names.size(); i++) {
                    lua_getfield(L, -1, ctx->column_names[i].c_str());
                    if (!lua_isnil(L, -1)) {
                        ctx->new_row[i] = pop_value(L, -1);
                        ctx->mutated_new = true;
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return 0;
}

// ─── call() with JIT dispatch ──────────────────────────────────────────

sql::Value ScriptEngine::call(const catalog::ScriptInfo &script,
                              const std::vector<sql::Value> &args,
                              TriggerContext *ctx) {
    last_error_.clear();

#ifdef TDB_WITH_LLVM
    // JIT dispatch for non-trigger Lua calls
    if (!ctx && impl_->jit && impl_->jit->available() &&
        impl_->jit_mode != JITMode::OFF) {
        bool should_jit = (impl_->jit_mode == JITMode::ON) ||
                          (++impl_->call_counts[script.name] >= State::JIT_THRESHOLD);
        if (should_jit) {
            auto native = impl_->jit->lookup(script.name, script.version);
            if (!native) {
                auto result = impl_->jit->compile(impl_->L, script);
                if (result.func) native = result.func;
            }
            if (native) {
                // Marshal arguments
                std::vector<JITValue> jit_args(args.size());
                for (size_t i = 0; i < args.size(); i++)
                    jit_args[i] = marshal_to_jit(args[i]);
                JITValue jit_result = JITValue::make_nil();
                int32_t rc = native(impl_.get(), jit_args.data(),
                                    static_cast<int32_t>(jit_args.size()),
                                    &jit_result);
                if (rc == 0) return marshal_from_jit(jit_result);
                // JIT execution failed — fall through to interpreter
            }
        }
    }
#endif

    // Interpreter path
    int rc = compile_and_call(impl_->L, script, args, ctx);
    if (rc != 0) {
        const char *err = lua_tostring(impl_->L, -1);
        last_error_ = err ? err : "lua error";
        lua_pop(impl_->L, 1);
        throw std::runtime_error(last_error_);
    }
    sql::Value rv = pop_value(impl_->L, -1);
    lua_pop(impl_->L, 1);
    return rv;
}

sql::Value ScriptEngine::invoke_udf(const catalog::ScriptInfo &udf,
                                    const std::vector<sql::Value> &args) {
    return call(udf, args, nullptr);
}

void ScriptEngine::fire_trigger(const catalog::ScriptInfo &script,
                                TriggerContext &ctx) {
    (void)call(script, {}, &ctx);
}

sql::Value ScriptEngine::eval(const std::string &lua_code) {
    if (!impl_ || !impl_->L) {
        last_error_ = "Lua state not initialized";
        return sql::Value::make_null();
    }
    lua_State *L = impl_->L;
    last_error_.clear();

    int rc = luaL_dostring(L, lua_code.c_str());
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        last_error_ = err ? err : "Lua error";
        lua_pop(L, 1);
        return sql::Value::make_null();
    }

    if (lua_gettop(L) > 0) {
        sql::Value result = pop_value(L, -1);
        lua_settop(L, 0);
        return result;
    }
    return sql::Value::make_null();
}

// ─── JavaScript entry points ───────────────────────────────────────────

sql::Value ScriptEngine::eval_js(const std::string &js_code) {
#ifdef TDB_WITH_JSC
    if (impl_->js && impl_->js->available()) {
        try {
            return impl_->js->eval(js_code);
        } catch (const std::exception &e) {
            last_error_ = e.what();
            return sql::Value::make_null();
        }
    }
#endif
    last_error_ = "JavaScript engine not available";
    (void)js_code;
    return sql::Value::make_null();
}

sql::Value ScriptEngine::call_js(const std::string &function_body,
                                  const std::vector<std::string> &param_names,
                                  const std::vector<sql::Value> &args) {
#ifdef TDB_WITH_JSC
    if (impl_->js && impl_->js->available()) {
        try {
            return impl_->js->call(function_body, param_names, args);
        } catch (const std::exception &e) {
            last_error_ = e.what();
            return sql::Value::make_null();
        }
    }
#endif
    last_error_ = "JavaScript engine not available";
    (void)function_body; (void)param_names; (void)args;
    return sql::Value::make_null();
}

bool ScriptEngine::js_available() const {
#ifdef TDB_WITH_JSC
    return impl_->js && impl_->js->available();
#else
    return false;
#endif
}

void ScriptEngine::set_jit_mode(JITMode mode) {
    if (impl_) impl_->jit_mode = mode;
}

JITMode ScriptEngine::jit_mode() const {
    return impl_ ? impl_->jit_mode : JITMode::OFF;
}

} // namespace tdb::script
