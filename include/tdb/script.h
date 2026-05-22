#ifndef TDB_SCRIPT_H
#define TDB_SCRIPT_H

#include "tdb/catalog.h"
#include "tdb/sql/executor.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for optional subsystems
#ifdef TDB_WITH_LLVM
namespace tdb::script { class LuaJIT; }
#endif

namespace tdb::script {

// ─── Script language enum ───────────────────────────────────────────────
enum class ScriptLang { LUA, JAVASCRIPT };

// ─── JIT execution mode ────────────────────────────────────────────────
enum class JITMode {
    OFF,    // never JIT — always use interpreter
    ON,     // always attempt JIT compilation
    AUTO    // JIT after call count exceeds threshold (default)
};

// ─── Trigger context passed into Lua trigger scripts ─────────────────────
struct TriggerContext {
    catalog::TriggerTiming timing;
    catalog::TriggerEvent  event;
    std::string table;
    std::vector<std::string> column_names;
    std::vector<sql::Value>  new_row;
    std::vector<sql::Value>  old_row;
    bool mutated_new = false;
};

// ─── SQL execution callback (injected from Database to break a cycle) ───
using SqlExecuteFn = std::function<sql::ResultSet(const std::string &sql)>;
using CatalogFn    = std::function<const catalog::Catalog &()>;

struct ScriptEngineOptions {
    SqlExecuteFn execute_fn;
    CatalogFn    catalog_fn;     // for JS catalog access
    std::function<std::string()> current_user_fn;
    std::function<void(const std::string &)> log_fn;
    JITMode jit_mode = JITMode::AUTO;
};

// ─── ScriptEngine ────────────────────────────────────────────────────────
//
// Multi-language script engine supporting Lua (interpreted + optional LLVM JIT)
// and JavaScript (via JavaScriptCore with built-in FTL JIT on macOS).
//
// Entry points:
//   • call(script, args, ctx)   — invokes a script as a procedure
//   • invoke_udf(name, args)    — invokes a registered UDF by name
//   • fire_trigger(t, ctx)      — invokes a trigger
//   • eval(code)                — REPL evaluation (Lua)
//   • eval_js(code)             — REPL evaluation (JavaScript)
//
class ScriptEngine {
public:
    explicit ScriptEngine(const ScriptEngineOptions &opts = {});
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine &) = delete;
    ScriptEngine &operator=(const ScriptEngine &) = delete;

    // ─── Lua entry points ──────────────────────────────────────────
    sql::Value call(const catalog::ScriptInfo &script,
                    const std::vector<sql::Value> &args,
                    TriggerContext *ctx = nullptr);

    sql::Value invoke_udf(const catalog::ScriptInfo &udf,
                          const std::vector<sql::Value> &args);

    void fire_trigger(const catalog::ScriptInfo &script, TriggerContext &ctx);

    sql::Value eval(const std::string &lua_code);

    // ─── JavaScript entry points ───────────────────────────────────
    sql::Value eval_js(const std::string &js_code);

    sql::Value call_js(const std::string &function_body,
                       const std::vector<std::string> &param_names,
                       const std::vector<sql::Value> &args);

    // ─── Status ────────────────────────────────────────────────────
    const std::string &last_error() const { return last_error_; }
    bool js_available() const;

    // ─── JIT control ──────────────────────────────────────────────
    void set_jit_mode(JITMode mode);
    JITMode jit_mode() const;

    // Public state struct — used by C bridge functions
    struct State;

private:
    std::unique_ptr<State> impl_;
    std::string last_error_;
};

} // namespace tdb::script

#endif // TDB_SCRIPT_H
