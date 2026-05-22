/*
 * jsengine.mm -- JavaScript engine using Apple JavaScriptCore.
 *
 * Embeds JSC (the WebKit JavaScript engine) which includes a production-grade
 * JIT compiler (DFG + FTL/B3) that compiles JavaScript to native ARM64/x86_64
 * code. This provides JavaScript scripting with full database access via a
 * `db` global object.
 *
 * Uses Objective-C++ (.mm) for the JSC Objective-C API which provides the
 * cleanest integration. The public API (jsengine.h) is pure C++.
 *
 * Copyright 2026 Rick Holland. Apache 2.0.
 */

#import <JavaScriptCore/JavaScriptCore.h>
#include "tdb/jsengine.h"
#include <stdexcept>

namespace tdb::script {

// ─── JSEngine::Impl ────────────────────────────────────────────────────

struct JSEngine::Impl {
    JSContext      *ctx  = nil;
    JSEngineOptions opts;
    bool            initialized = false;
};

// ─── Value conversion helpers ──────────────────────────────────────────

static JSValue *sql_to_js(JSContext *ctx, const sql::Value &v) {
    switch (v.type) {
    case sql::Value::Type::NULL_VAL:     return [JSValue valueWithNullInContext:ctx];
    case sql::Value::Type::BOOL:         return [JSValue valueWithBool:v.bool_val inContext:ctx];
    case sql::Value::Type::INT64:        return [JSValue valueWithInt32:(int32_t)v.int_val inContext:ctx];
    case sql::Value::Type::FLOAT64:      return [JSValue valueWithDouble:v.float_val inContext:ctx];
    case sql::Value::Type::STRING:
        return [JSValue valueWithObject:
            [NSString stringWithUTF8String:v.str_val.c_str()] inContext:ctx];
    case sql::Value::Type::DECIMAL:
        return [JSValue valueWithDouble:std::stod(v.str_val) inContext:ctx];
    case sql::Value::Type::DATE_VAL:
    case sql::Value::Type::TIME_VAL:
    case sql::Value::Type::TIMESTAMP_VAL:
        return [JSValue valueWithObject:
            [NSString stringWithUTF8String:v.to_string().c_str()] inContext:ctx];
    case sql::Value::Type::JSON_VAL: {
        // Parse JSON string into a JS object
        NSString *json = [NSString stringWithUTF8String:v.str_val.c_str()];
        JSValue *parsed = [ctx evaluateScript:
            [NSString stringWithFormat:@"JSON.parse(%@)",
                [NSString stringWithFormat:@"'%@'", json]]];
        if (parsed && ![parsed isUndefined]) return parsed;
        return [JSValue valueWithObject:json inContext:ctx];
    }
    case sql::Value::Type::ARRAY: {
        // Convert array elements
        NSMutableArray *arr = [NSMutableArray array];
        if (v.composite_fields) {
            for (auto &elem : *v.composite_fields) {
                [arr addObject:[sql_to_js(ctx, elem) toObject]];
            }
        }
        return [JSValue valueWithObject:arr inContext:ctx];
    }
    default:
        // Extended types: convert to string
        return [JSValue valueWithObject:
            [NSString stringWithUTF8String:v.to_string().c_str()] inContext:ctx];
    }
}

static sql::Value js_to_sql(JSValue *v) {
    if (!v || [v isNull] || [v isUndefined]) {
        return sql::Value::make_null();
    }
    if ([v isBoolean]) {
        return sql::Value::make_bool([v toBool]);
    }
    if ([v isNumber]) {
        double d = [v toDouble];
        // Check if it's an integer
        if (d == (double)(int64_t)d && d >= -9e18 && d <= 9e18) {
            return sql::Value::make_int((int64_t)d);
        }
        return sql::Value::make_float(d);
    }
    if ([v isString]) {
        NSString *s = [v toString];
        return sql::Value::make_string(s ? [s UTF8String] : "");
    }
    if ([v isArray]) {
        // Convert JS array to string representation
        NSString *json = [[NSString alloc] initWithData:
            [NSJSONSerialization dataWithJSONObject:[v toArray] options:0 error:nil]
            encoding:NSUTF8StringEncoding];
        return sql::Value::make_string(json ? [json UTF8String] : "[]");
    }
    if ([v isObject]) {
        // Convert JS object to JSON string
        NSDictionary *dict = [v toDictionary];
        NSData *data = [NSJSONSerialization dataWithJSONObject:dict options:0 error:nil];
        if (data) {
            NSString *json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            return sql::Value::make_string(json ? [json UTF8String] : "{}");
        }
        return sql::Value::make_string("{}");
    }
    return sql::Value::make_null();
}

// Convert a ResultSet to a JSValue (array of row objects)
static JSValue *resultset_to_js(JSContext *ctx, const sql::ResultSet &rs) {
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:rs.rows.size()];
    for (auto &row : rs.rows) {
        NSMutableDictionary *dict = [NSMutableDictionary dictionary];
        for (size_t c = 0; c < row.size() && c < rs.columns.size(); c++) {
            NSString *key = [NSString stringWithUTF8String:rs.columns[c].name.c_str()];
            dict[key] = [sql_to_js(ctx, row[c]) toObject];
        }
        [rows addObject:dict];
    }
    return [JSValue valueWithObject:rows inContext:ctx];
}

// ─── JSEngine constructor ──────────────────────────────────────────────

JSEngine::JSEngine(const JSEngineOptions &opts)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;

    @autoreleasepool {
        impl_->ctx = [[JSContext alloc] init];
        if (!impl_->ctx) return;
        impl_->initialized = true;

        // Set up exception handler
        impl_->ctx.exceptionHandler = ^(JSContext * /*ctx*/, JSValue *exception) {
            // Store exception for later retrieval
            NSString *desc = [exception toString];
            (void)desc; // accessed through last_error_
        };

        // ─── Register db global object ─────────────────────────────

        // db.execute(sql) -> { rowsAffected: N, success: bool }
        if (opts.execute_fn) {
            auto exec_fn = opts.execute_fn;
            impl_->ctx[@"__tdb_execute"] = ^JSValue *(NSString *sql) {
                if (!sql) return [JSValue valueWithNullInContext:[JSContext currentContext]];
                std::string sql_str = [sql UTF8String];
                auto rs = exec_fn(sql_str);
                JSContext *c = [JSContext currentContext];
                if (!rs.success) {
                    NSString *err = [NSString stringWithUTF8String:rs.error_message.c_str()];
                    c.exception = [JSValue valueWithNewErrorFromMessage:err inContext:c];
                    return [JSValue valueWithNullInContext:c];
                }
                return [JSValue valueWithObject:@{
                    @"rowsAffected": @(rs.rows_affected),
                    @"success": @YES
                } inContext:c];
            };

            // db.query(sql) -> array of row objects
            impl_->ctx[@"__tdb_query"] = ^JSValue *(NSString *sql) {
                if (!sql) return [JSValue valueWithNullInContext:[JSContext currentContext]];
                std::string sql_str = [sql UTF8String];
                auto rs = exec_fn(sql_str);
                JSContext *c = [JSContext currentContext];
                if (!rs.success) {
                    NSString *err = [NSString stringWithUTF8String:rs.error_message.c_str()];
                    c.exception = [JSValue valueWithNewErrorFromMessage:err inContext:c];
                    return [JSValue valueWithNullInContext:c];
                }
                return resultset_to_js(c, rs);
            };
        }

        // db.now() -> epoch ms
        impl_->ctx[@"__tdb_now"] = ^double() {
            using namespace std::chrono;
            return static_cast<double>(
                duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch()).count());
        };

        // db.user() -> string
        if (opts.current_user_fn) {
            auto user_fn = opts.current_user_fn;
            impl_->ctx[@"__tdb_user"] = ^NSString *() {
                return [NSString stringWithUTF8String:user_fn().c_str()];
            };
        }

        // db.log(msg)
        if (opts.log_fn) {
            auto log_fn = opts.log_fn;
            impl_->ctx[@"__tdb_log"] = ^(NSString *msg) {
                if (msg) log_fn([msg UTF8String]);
            };
        }

        // Catalog metadata functions
        if (opts.catalog_fn) {
            auto cat_fn = opts.catalog_fn;

            impl_->ctx[@"__tdb_tables"] = ^JSValue *() {
                JSContext *c = [JSContext currentContext];
                auto &cat = cat_fn();
                auto tables = cat.list_tables();
                NSMutableArray *arr = [NSMutableArray arrayWithCapacity:tables.size()];
                for (auto &t : tables) {
                    [arr addObject:[NSString stringWithUTF8String:t.c_str()]];
                }
                return [JSValue valueWithObject:arr inContext:c];
            };

            impl_->ctx[@"__tdb_views"] = ^JSValue *() {
                JSContext *c = [JSContext currentContext];
                auto &cat = cat_fn();
                auto views = cat.list_views();
                NSMutableArray *arr = [NSMutableArray arrayWithCapacity:views.size()];
                for (auto &v : views) {
                    [arr addObject:[NSString stringWithUTF8String:v.c_str()]];
                }
                return [JSValue valueWithObject:arr inContext:c];
            };

            impl_->ctx[@"__tdb_indexes"] = ^JSValue *() {
                JSContext *c = [JSContext currentContext];
                auto &cat = cat_fn();
                auto idxs = cat.list_indexes();
                NSMutableArray *arr = [NSMutableArray arrayWithCapacity:idxs.size()];
                for (auto &i : idxs) {
                    [arr addObject:[NSString stringWithUTF8String:i.c_str()]];
                }
                return [JSValue valueWithObject:arr inContext:c];
            };

            impl_->ctx[@"__tdb_sequences"] = ^JSValue *() {
                JSContext *c = [JSContext currentContext];
                // Sequences don't have a list method; return empty array
                return [JSValue valueWithObject:@[] inContext:c];
            };

            impl_->ctx[@"__tdb_tablespaces"] = ^JSValue *() {
                JSContext *c = [JSContext currentContext];
                auto &cat = cat_fn();
                auto tss = cat.list_tablespaces();
                NSMutableArray *arr = [NSMutableArray arrayWithCapacity:tss.size()];
                for (auto &ts : tss) {
                    [arr addObject:[NSString stringWithUTF8String:ts.c_str()]];
                }
                return [JSValue valueWithObject:arr inContext:c];
            };

            impl_->ctx[@"__tdb_describe"] = ^JSValue *(NSString *name) {
                JSContext *c = [JSContext currentContext];
                if (!name) return [JSValue valueWithNullInContext:c];
                auto &cat = cat_fn();
                std::string tname = [name UTF8String];
                auto *ti = cat.find_table(tname);
                if (!ti) return [JSValue valueWithNullInContext:c];
                NSMutableArray *cols = [NSMutableArray array];
                for (auto &col : ti->columns) {
                    [cols addObject:@{
                        @"column": [NSString stringWithUTF8String:col.name.c_str()],
                        @"type": [NSString stringWithUTF8String:col.type.name.c_str()],
                        @"nullable": @(col.nullable),
                        @"ordinal": @(col.ordinal)
                    }];
                }
                return [JSValue valueWithObject:cols inContext:c];
            };
        }

        // ─── Build the db object in JavaScript ─────────────────────
        [impl_->ctx evaluateScript:@
            "var db = {\n"
            "  execute: function(sql) { return __tdb_execute(sql); },\n"
            "  query: function(sql) { return __tdb_query(sql); },\n"
            "  now: function() { return __tdb_now(); },\n"
            "  user: function() { return typeof __tdb_user !== 'undefined' ? __tdb_user() : 'anonymous'; },\n"
            "  log: function(msg) { if (typeof __tdb_log !== 'undefined') __tdb_log(String(msg)); },\n"
            "  tables: function() { return typeof __tdb_tables !== 'undefined' ? __tdb_tables() : []; },\n"
            "  views: function() { return typeof __tdb_views !== 'undefined' ? __tdb_views() : []; },\n"
            "  indexes: function() { return typeof __tdb_indexes !== 'undefined' ? __tdb_indexes() : []; },\n"
            "  sequences: function() { return typeof __tdb_sequences !== 'undefined' ? __tdb_sequences() : []; },\n"
            "  tablespaces: function() { return typeof __tdb_tablespaces !== 'undefined' ? __tdb_tablespaces() : []; },\n"
            "  describe: function(name) { return typeof __tdb_describe !== 'undefined' ? __tdb_describe(name) : null; },\n"
            "  get catalog() {\n"
            "    return {\n"
            "      tables: this.tables(),\n"
            "      views: this.views(),\n"
            "      indexes: this.indexes(),\n"
            "      sequences: this.sequences(),\n"
            "      tablespaces: this.tablespaces()\n"
            "    };\n"
            "  }\n"
            "};\n"
            // Convenience: console.log maps to db.log
            "var console = { log: function() {\n"
            "  var args = Array.prototype.slice.call(arguments);\n"
            "  db.log(args.map(String).join(' '));\n"
            "}};\n"
        ];
    }
}

JSEngine::~JSEngine() {
    @autoreleasepool {
        if (impl_ && impl_->ctx) {
            impl_->ctx = nil;
        }
    }
}

bool JSEngine::available() const {
    return impl_ && impl_->initialized;
}

// ─── eval ──────────────────────────────────────────────────────────────

sql::Value JSEngine::eval(const std::string &js_source) {
    last_error_.clear();

    if (!impl_ || !impl_->initialized) {
        last_error_ = "JSEngine not initialized";
        throw std::runtime_error(last_error_);
    }

    @autoreleasepool {
        NSString *source = [NSString stringWithUTF8String:js_source.c_str()];
        JSValue *result = [impl_->ctx evaluateScript:source];

        // Check for exceptions
        JSValue *exception = impl_->ctx.exception;
        if (exception && ![exception isUndefined] && ![exception isNull]) {
            last_error_ = [[exception toString] UTF8String];
            impl_->ctx.exception = nil;
            throw std::runtime_error(last_error_);
        }

        return js_to_sql(result);
    }
}

// ─── call ──────────────────────────────────────────────────────────────

sql::Value JSEngine::call(const std::string &function_body,
                           const std::vector<std::string> &param_names,
                           const std::vector<sql::Value> &args) {
    last_error_.clear();

    if (!impl_ || !impl_->initialized) {
        last_error_ = "JSEngine not initialized";
        throw std::runtime_error(last_error_);
    }

    @autoreleasepool {
        // Build: (function(param1, param2) { <body> })(arg1, arg2)
        NSMutableString *script = [NSMutableString stringWithString:@"(function("];
        for (size_t i = 0; i < param_names.size(); i++) {
            if (i > 0) [script appendString:@", "];
            [script appendString:[NSString stringWithUTF8String:param_names[i].c_str()]];
        }
        [script appendString:@") {\n"];
        [script appendString:[NSString stringWithUTF8String:function_body.c_str()]];
        [script appendString:@"\n})("];

        // Serialize arguments
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) [script appendString:@", "];
            // Serialize argument values inline
            switch (args[i].type) {
            case sql::Value::Type::NULL_VAL:
                [script appendString:@"null"]; break;
            case sql::Value::Type::BOOL:
                [script appendString:args[i].bool_val ? @"true" : @"false"]; break;
            case sql::Value::Type::INT64:
                [script appendFormat:@"%lld", (long long)args[i].int_val]; break;
            case sql::Value::Type::FLOAT64:
                [script appendFormat:@"%.17g", args[i].float_val]; break;
            case sql::Value::Type::STRING: {
                // JSON-escape the string
                NSString *s = [NSString stringWithUTF8String:args[i].str_val.c_str()];
                NSData *d = [NSJSONSerialization dataWithJSONObject:@[s] options:0 error:nil];
                NSString *json = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
                // json is like ["str"], extract the string literal
                if (json.length > 2) {
                    [script appendString:[json substringWithRange:NSMakeRange(1, json.length - 2)]];
                } else {
                    [script appendString:@"\"\""];
                }
                break;
            }
            default:
                [script appendFormat:@"\"%s\"", args[i].to_string().c_str()];
                break;
            }
        }
        [script appendString:@")"];

        JSValue *result = [impl_->ctx evaluateScript:script];

        JSValue *exception = impl_->ctx.exception;
        if (exception && ![exception isUndefined] && ![exception isNull]) {
            last_error_ = [[exception toString] UTF8String];
            impl_->ctx.exception = nil;
            throw std::runtime_error(last_error_);
        }

        return js_to_sql(result);
    }
}

} // namespace tdb::script
