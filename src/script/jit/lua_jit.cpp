/*
 * lua_jit.cpp -- LLVM ORC JIT lifecycle, compilation, and caching.
 *
 * Manages the LLJIT instance, compiles Lua Proto bytecodes to native code
 * via the IREmitter, caches compiled functions, and registers trampoline
 * symbols for the db.* bridge.
 *
 * Copyright 2026 Rick Holland. Apache 2.0.
 */

#include "tdb/jit.h"
#include "ir_emitter.h"
#include "trampolines.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#pragma GCC diagnostic pop

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "lfunc.h"
}

#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace tdb::script {

// ─── Marshalling helpers ────────────────────────────────────────────────

JITValue marshal_to_jit(const sql::Value &v) {
    switch (v.type) {
    case sql::Value::Type::NULL_VAL:  return JITValue::make_nil();
    case sql::Value::Type::BOOL:      return JITValue::make_bool(v.bool_val);
    case sql::Value::Type::INT64:     return JITValue::make_int(v.int_val);
    case sql::Value::Type::FLOAT64:   return JITValue::make_float(v.float_val);
    case sql::Value::Type::STRING:
        return JITValue::make_string(v.str_val.c_str(),
                                      static_cast<int64_t>(v.str_val.size()));
    case sql::Value::Type::BLOB:
        return JITValue::make_string(v.str_val.c_str(),
                                      static_cast<int64_t>(v.str_val.size()));
    default: {
        // Extended types: convert to string representation
        // The string is stored in v.str_val which outlives the JIT call
        // because the sql::Value lives in the caller's scope
        static thread_local std::string tl_ext;
        tl_ext = v.to_string();
        return JITValue::make_string(tl_ext.c_str(),
                                      static_cast<int64_t>(tl_ext.size()));
    }
    }
}

sql::Value marshal_from_jit(const JITValue &jv) {
    switch (jv.type) {
    case JVT_NIL:    return sql::Value::make_null();
    case JVT_BOOL:   return sql::Value::make_bool(jv.ival != 0);
    case JVT_INT:    return sql::Value::make_int(jv.ival);
    case JVT_FLOAT:  return sql::Value::make_float(jv.fval);
    case JVT_STRING:
        return sql::Value::make_string(
            std::string(jv.sval, static_cast<size_t>(jv.ival)));
    default:         return sql::Value::make_null();
    }
}

// ─── LuaJIT::Impl ─────────────────────────────────────────────────────

struct LuaJIT::Impl {
    std::unique_ptr<llvm::orc::LLJIT> lljit;
    void *bridge_state = nullptr;

    struct CacheEntry {
        JITNativeFunc func;
    };
    std::unordered_map<std::string, CacheEntry> cache;
    std::unordered_set<std::string> blacklist; // permanently failed

    Stats stats;
    bool initialized = false;

    static std::string cache_key(const std::string &name,
                                  const ObjectVersion &ver) {
        return name + ":" + std::to_string(ver.major) + "."
                          + std::to_string(ver.minor) + "."
                          + std::to_string(ver.patch);
    }
};

// ─── Constructor / Destructor ──────────────────────────────────────────

LuaJIT::LuaJIT() : impl_(std::make_unique<Impl>()) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jit_or_err = llvm::orc::LLJITBuilder().create();
    if (!jit_or_err) {
        llvm::consumeError(jit_or_err.takeError());
        return; // JIT unavailable — will fall back to interpreter
    }
    impl_->lljit = std::move(*jit_or_err);
    impl_->initialized = true;

    // Register trampoline symbols as absolute symbols in the JIT session
    auto &es = impl_->lljit->getExecutionSession();
    auto &jd = impl_->lljit->getMainJITDylib();

    llvm::orc::SymbolMap symbols;
    auto add_sym = [&](llvm::StringRef name, void *addr) {
        symbols[es.intern(name)] = {
            llvm::orc::ExecutorAddr::fromPtr(addr),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    };

    add_sym("jit_db_execute",   reinterpret_cast<void*>(&jit_db_execute));
    add_sym("jit_db_query",     reinterpret_cast<void*>(&jit_db_query));
    add_sym("jit_db_now",       reinterpret_cast<void*>(&jit_db_now));
    add_sym("jit_db_user",      reinterpret_cast<void*>(&jit_db_user));
    add_sym("jit_db_log",       reinterpret_cast<void*>(&jit_db_log));
    add_sym("jit_lua_concat",   reinterpret_cast<void*>(&jit_lua_concat));
    add_sym("jit_lua_len",      reinterpret_cast<void*>(&jit_lua_len));
    add_sym("jit_lua_tostring", reinterpret_cast<void*>(&jit_lua_tostring));
    add_sym("jit_lua_tonumber", reinterpret_cast<void*>(&jit_lua_tonumber));
    add_sym("jit_lua_pow",      reinterpret_cast<void*>(&jit_lua_pow));
    add_sym("jit_lua_type",     reinterpret_cast<void*>(&jit_lua_type));
    add_sym("jit_lua_print",    reinterpret_cast<void*>(&jit_lua_print));
    add_sym("jit_lua_compare",  reinterpret_cast<void*>(&jit_lua_compare));

    auto err = jd.define(llvm::orc::absoluteSymbols(std::move(symbols)));
    if (err) {
        llvm::consumeError(std::move(err));
        impl_->initialized = false;
    }
}

LuaJIT::~LuaJIT() = default;

bool LuaJIT::available() const {
    return impl_ && impl_->initialized;
}

void LuaJIT::set_bridge_state(void *state) {
    if (impl_) impl_->bridge_state = state;
}

LuaJIT::Stats LuaJIT::stats() const {
    return impl_ ? impl_->stats : Stats{};
}

// ─── Lookup ────────────────────────────────────────────────────────────

JITNativeFunc LuaJIT::lookup(const std::string &name,
                              const ObjectVersion &ver) const {
    if (!impl_) return nullptr;
    auto key = Impl::cache_key(name, ver);
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) {
        impl_->stats.cache_hits++;
        return it->second.func;
    }
    return nullptr;
}

// ─── Compile ───────────────────────────────────────────────────────────

LuaJIT::CompileResult LuaJIT::compile(lua_State *L,
                                        const catalog::ScriptInfo &script) {
    if (!impl_ || !impl_->initialized) {
        return {nullptr, false, "JIT not initialized"};
    }

    auto key = Impl::cache_key(script.name, script.version);

    // Check blacklist
    if (impl_->blacklist.count(key)) {
        return {nullptr, true, "previously failed"};
    }

    // Check cache
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) {
        impl_->stats.cache_hits++;
        return {it->second.func, false, ""};
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Build the same wrapped source as the interpreter
    std::string wrapped = "local function __tdb_script(";
    for (size_t i = 0; i < script.params.size(); i++) {
        if (i) wrapped += ", ";
        wrapped += script.params[i].name;
    }
    wrapped += ")\n";
    wrapped += script.lua_source;
    wrapped += "\nend\nreturn __tdb_script(...)";

    // Compile to Lua bytecode
    if (luaL_loadbuffer(L, wrapped.data(), wrapped.size(),
                        script.name.c_str()) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        std::string errmsg = err ? err : "Lua parse error";
        lua_pop(L, 1);
        impl_->blacklist.insert(key);
        return {nullptr, true, errmsg};
    }

    // Extract Proto* from the Lua closure on top of stack.
    // The compiled chunk is an LClosure. Its Proto has one child Proto
    // (the __tdb_script function) which is what we want to JIT-compile.
    const LClosure *cl = nullptr;
    const Proto *chunk_proto = nullptr;
    const Proto *func_proto = nullptr;

    // Access the closure at stack top using Lua internals
    StkId top = L->top.p;
    if (top > L->stack.p) {
        const TValue *tv = s2v(top - 1);
        if (ttisLclosure(tv)) {
            cl = clLvalue(tv);
            chunk_proto = cl->p;
            if (chunk_proto && chunk_proto->sizep > 0) {
                func_proto = chunk_proto->p[0];
            }
        }
    }

    lua_pop(L, 1); // pop the closure

    if (!func_proto) {
        impl_->blacklist.insert(key);
        return {nullptr, true, "could not extract Proto from compiled chunk"};
    }

    // Create an LLVM module and emit IR
    auto tsc = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("tdb_jit_" + script.name, *tsc);
    mod->setDataLayout(impl_->lljit->getDataLayout());

    jit::IREmitter emitter(*tsc, *mod);
    std::string jit_func_name = "jit_" + script.name;
    llvm::Function *ir_func = emitter.emit(func_proto, jit_func_name);

    if (!ir_func) {
        impl_->blacklist.insert(key);
        impl_->stats.fallback_count++;
        return {nullptr, true, emitter.error()};
    }

    // Add module to LLJIT. ThreadSafeModule takes ownership of both the
    // Module and its LLVMContext. We re-emit into a fresh context/module pair
    // because the first context was used for Proto extraction above.
    {
        auto ctx2 = std::make_unique<llvm::LLVMContext>();
        auto mod2 = std::make_unique<llvm::Module>("tdb_jit_" + script.name, *ctx2);
        mod2->setDataLayout(impl_->lljit->getDataLayout());

        jit::IREmitter emitter2(*ctx2, *mod2);
        llvm::Function *ir_func2 = emitter2.emit(func_proto, jit_func_name);
        if (!ir_func2) {
            impl_->blacklist.insert(key);
            impl_->stats.fallback_count++;
            return {nullptr, true, emitter2.error()};
        }

        // LLVM 22: ThreadSafeModule(unique_ptr<Module>, unique_ptr<LLVMContext>)
        auto tsm2 = llvm::orc::ThreadSafeModule(std::move(mod2), std::move(ctx2));

        auto err = impl_->lljit->addIRModule(std::move(tsm2));
        if (err) {
            std::string errmsg;
            llvm::raw_string_ostream oss(errmsg);
            oss << err;
            llvm::consumeError(std::move(err));
            impl_->blacklist.insert(key);
            impl_->stats.fallback_count++;
            return {nullptr, true, errmsg};
        }
    }

    // Look up the compiled symbol
    auto sym_or_err = impl_->lljit->lookup(jit_func_name);
    if (!sym_or_err) {
        std::string errmsg;
        llvm::raw_string_ostream oss(errmsg);
        oss << sym_or_err.takeError();
        impl_->blacklist.insert(key);
        impl_->stats.fallback_count++;
        return {nullptr, true, errmsg};
    }

    auto native = sym_or_err->toPtr<JITNativeFunc>();

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    impl_->cache[key] = {native};
    impl_->stats.compiled_count++;
    impl_->stats.total_compile_us += static_cast<uint64_t>(us);

    return {native, false, ""};
}

} // namespace tdb::script
