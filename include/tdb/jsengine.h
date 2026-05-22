/*
 * jsengine.h -- JavaScript engine for TDB.
 *
 * Embeds JavaScriptCore (Apple's WebKit JS engine) to provide JavaScript
 * scripting with full access to all database objects via a `db` global.
 * On macOS, uses the system JavaScriptCore.framework which includes
 * a production-grade JIT compiler (FTL/B3 -> native code).
 *
 * JavaScript scripts have access to:
 *   db.execute(sql)  -> { rowsAffected: N }
 *   db.query(sql)    -> [{ col1: val, col2: val, ... }, ...]
 *   db.now()         -> milliseconds since epoch
 *   db.user()        -> current user string
 *   db.log(msg)      -> log message via engine callback
 *   db.tables()      -> ["table1", "table2", ...]
 *   db.views()       -> ["view1", "view2", ...]
 *   db.indexes()     -> ["idx1", "idx2", ...]
 *   db.sequences()   -> ["seq1", "seq2", ...]
 *   db.tablespaces() -> ["ts1", "ts2", ...]
 *   db.describe(name)-> [{ column: "id", type: "INTEGER", ... }, ...]
 *   db.catalog       -> { tables: [...], views: [...], ... }
 *
 * Copyright 2026 Rick Holland. Apache 2.0.
 */
#ifndef TDB_JSENGINE_H
#define TDB_JSENGINE_H

#include "tdb/catalog.h"
#include "tdb/sql/executor.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tdb::script {

// ─── JavaScript execution callback (same bridge as Lua) ────────────────
using JsSqlExecuteFn = std::function<sql::ResultSet(const std::string &sql)>;
using JsCatalogFn    = std::function<const catalog::Catalog &()>;

struct JSEngineOptions {
    JsSqlExecuteFn execute_fn;
    JsCatalogFn    catalog_fn;   // access to catalog for metadata queries
    std::function<std::string()> current_user_fn;
    std::function<void(const std::string &)> log_fn;
};

// ─── JSEngine ──────────────────────────────────────────────────────────
class JSEngine {
public:
    explicit JSEngine(const JSEngineOptions &opts = {});
    ~JSEngine();

    JSEngine(const JSEngine &) = delete;
    JSEngine &operator=(const JSEngine &) = delete;

    // Returns true if JSC initialized successfully.
    bool available() const;

    // Execute JavaScript source code. Returns the result as a sql::Value.
    // Throws std::runtime_error on JS exception.
    sql::Value eval(const std::string &js_source);

    // Execute a stored JavaScript function with arguments.
    sql::Value call(const std::string &function_body,
                    const std::vector<std::string> &param_names,
                    const std::vector<sql::Value> &args);

    // Last error from a failed eval/call.
    const std::string &last_error() const { return last_error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string last_error_;
};

} // namespace tdb::script

#endif // TDB_JSENGINE_H
