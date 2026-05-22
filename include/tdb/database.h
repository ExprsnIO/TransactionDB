#ifndef TDB_DATABASE_H
#define TDB_DATABASE_H

#include "tdb/sql/parser.h"
#include "tdb/sql/ast.h"
#include "tdb/sql/executor.h"
#include "tdb/catalog.h"
#include "tdb/migrate.h"
#include "tdb/dbfile.h"
#include "tdb/crypto.h"
#include "tdb/sql/typecoerce.h"
#include <iomanip>
#include <cmath>
#include <chrono>

#ifdef TDB_WITH_ICU
#include <unicode/ustring.h>
#include <unicode/uchar.h>
namespace tdb {
inline std::string icu_fold_case(const std::string &in, bool to_upper) {
    // UTF-8 -> UTF-16 -> case fold -> UTF-8. ICU's u_strToLower/u_strToUpper
    // expect UTF-16; we go via UnicodeString for convenience.
    int32_t cap = (int32_t)in.size() * 2 + 16;
    std::vector<UChar> u16((size_t)cap);
    UErrorCode err = U_ZERO_ERROR;
    int32_t u16_len = 0;
    u_strFromUTF8(u16.data(), cap, &u16_len, in.data(), (int32_t)in.size(), &err);
    if (U_FAILURE(err)) return in;
    std::vector<UChar> folded((size_t)cap * 2);
    err = U_ZERO_ERROR;
    int32_t folded_len = to_upper
        ? u_strToUpper(folded.data(), (int32_t)folded.size(), u16.data(), u16_len, nullptr, &err)
        : u_strToLower(folded.data(), (int32_t)folded.size(), u16.data(), u16_len, nullptr, &err);
    if (U_FAILURE(err)) return in;
    std::vector<char> u8((size_t)folded_len * 4 + 16);
    err = U_ZERO_ERROR;
    int32_t u8_len = 0;
    u_strToUTF8(u8.data(), (int32_t)u8.size(), &u8_len, folded.data(), folded_len, &err);
    if (U_FAILURE(err)) return in;
    return std::string(u8.data(), (size_t)u8_len);
}
inline std::string icu_fold_to_upper(const std::string &s) { return icu_fold_case(s, true); }
inline std::string icu_fold_to_lower(const std::string &s) { return icu_fold_case(s, false); }
}
#endif
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <set>
#include <cmath>
#include <cstring>
#include <cctype>
#include <sstream>
#include <chrono>
#include <ctime>
#include <fstream>
#include <unordered_map>
#include <regex>
#include <functional>

namespace tdb {

class Database {
public:
    Database() = default;
    ~Database() = default;

    bool open(const std::string &path) {
        db_path_ = path;
        if (!path.empty()) {
            dbfile::load(path, catalog_);
        }
        return true;
    }

    void close() {
        if (in_transaction_) {
            in_transaction_ = false;
            txn_id_ = 0;
        }
        if (!db_path_.empty()) save();
    }

    bool save() {
        if (db_path_.empty()) return false;
        return dbfile::save(db_path_, catalog_);
    }

    bool save_as(const std::string &path) {
        return dbfile::save(path, catalog_);
    }

    const std::string &path() const { return db_path_; }

    sql::ResultSet execute(const std::string &sql_text) {
        auto start = std::chrono::high_resolution_clock::now();
        sql::ResultSet result;
        try {
            sql::Parser parser(sql_text);
            auto stmt = parser.parse();
            result = execute_stmt(*stmt);
        } catch (const sql::ParseError &e) {
            result.success = false;
            result.error_message = std::string("Parse error: ") + e.what();
        } catch (const std::exception &e) {
            result.success = false;
            result.error_message = std::string("Error: ") + e.what();
        }
        auto end = std::chrono::high_resolution_clock::now();
        last_exec_time_ms_ = std::chrono::duration<double, std::milli>(end - start).count();
        return result;
    }

    // Execute multiple statements
    std::vector<sql::ResultSet> execute_batch(const std::string &sql_text) {
        std::vector<sql::ResultSet> results;
        try {
            sql::Parser parser(sql_text);
            auto stmts = parser.parse_all();
            for (auto &stmt : stmts) {
                results.push_back(execute_stmt(*stmt));
            }
        } catch (const std::exception &e) {
            sql::ResultSet err;
            err.success = false;
            err.error_message = e.what();
            results.push_back(err);
        }
        return results;
    }

    double last_execution_time_ms() const { return last_exec_time_ms_; }

    catalog::Catalog &catalog() { return catalog_; }
    const catalog::Catalog &catalog() const { return catalog_; }

private:
    std::string db_path_;
    catalog::Catalog catalog_;
    // CTE temp storage for current query
    std::unordered_map<std::string, std::pair<sql::Schema, std::vector<sql::Tuple>>> cte_tables_;
    // Transaction state
    bool in_transaction_ = false;
    uint64_t txn_id_ = 0;
    uint64_t next_txn_id_ = 1;
    // Savepoint stack: name -> snapshot of table rows
    struct SavepointData {
        std::string name;
        std::unordered_map<std::string, std::vector<sql::Tuple>> table_snapshots;
    };
    std::vector<SavepointData> savepoints_;
    // Execution timing
    double last_exec_time_ms_ = 0;
    // Outer-scope stack for correlated subqueries. Each entry is the (row, schema)
    // of an enclosing query. Lookups resolve inner-to-outer (top of stack first).
    struct OuterScope {
        const sql::Tuple *row;
        const sql::Schema *schema;
    };
    std::vector<OuterScope> outer_scopes_;
    // Telemetry: counts the number of SELECT statements that took the spatial
    // bbox-prefilter pushdown path. Tests assert this to verify pushdown
    // actually fired.
    uint64_t pushed_down_spatial_count_ = 0;
    // GROUPING SETS / CUBE / ROLLUP: tracks which group_by column names
    // are aggregated away (NULL) for the current output row.
    std::set<std::string> grouping_null_columns_;
    // Stashed per-group row sets for HAVING evaluation against aggregates
    std::vector<std::vector<sql::Tuple>> having_groups_;
public:
    uint64_t pushed_down_spatial_count() const { return pushed_down_spatial_count_; }
private:
    struct ScopePush {
        Database *db;
        ScopePush(Database *d, const sql::Tuple &r, const sql::Schema &s) : db(d) {
            db->outer_scopes_.push_back({&r, &s});
        }
        ~ScopePush() { db->outer_scopes_.pop_back(); }
    };

    // ===============================================
    // Statement Dispatch
    // ===============================================

    sql::ResultSet execute_stmt(const sql::ast::Statement &stmt) {
        using ST = sql::ast::StmtType;
        switch (stmt.type) {
            case ST::CREATE_TABLE: return exec_create_table(std::get<sql::ast::CreateTableStmt>(stmt.data));
            case ST::CREATE_TYPE:  return exec_create_type(std::get<sql::ast::CreateTypeStmt>(stmt.data));
            case ST::CREATE_DOMAIN: return exec_create_domain(std::get<sql::ast::CreateTypeStmt>(stmt.data));
            case ST::ALTER_TYPE:   return exec_alter_type(std::get<sql::ast::AlterTypeStmt>(stmt.data));
            case ST::DROP_TABLE: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_TYPE:  return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_DOMAIN: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_INDEX: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_VIEW: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_MATERIALIZED_VIEW: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_TABLESPACE: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_SAVED_QUERY: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::INSERT: return exec_insert(std::get<sql::ast::InsertStmt>(stmt.data));
            case ST::SELECT: return exec_select(std::get<sql::ast::SelectStmt>(stmt.data));
            case ST::UPDATE: return exec_update(std::get<sql::ast::UpdateStmt>(stmt.data));
            case ST::DELETE: return exec_delete(std::get<sql::ast::DeleteStmt>(stmt.data));
            case ST::CREATE_INDEX: return exec_create_index(std::get<sql::ast::CreateIndexStmt>(stmt.data));
            case ST::CREATE_VIEW: return exec_create_view(std::get<sql::ast::CreateViewStmt>(stmt.data));
            case ST::CREATE_MATERIALIZED_VIEW: return exec_create_matview(std::get<sql::ast::CreateMaterializedViewStmt>(stmt.data));
            case ST::REFRESH_MATERIALIZED_VIEW: return exec_refresh_matview(std::get<sql::ast::RefreshMaterializedViewStmt>(stmt.data));
            case ST::CREATE_SAVED_QUERY: return exec_create_saved_query(std::get<sql::ast::CreateSavedQueryStmt>(stmt.data));
            case ST::CREATE_TABLESPACE: return exec_create_tablespace(std::get<sql::ast::CreateTablespaceStmt>(stmt.data));
            case ST::LOCK_STMT: return {true, "", {}, {}, 0};
            case ST::UNLOCK_STMT: return {true, "", {}, {}, 0};
            case ST::LOGGING_STMT: return {true, "", {}, {}, 0};
            case ST::TRUNCATE: return exec_truncate(std::get<sql::ast::TruncateStmt>(stmt.data));
            case ST::ALTER_TABLE: return exec_alter_table(std::get<sql::ast::AlterTableStmt>(stmt.data));
            case ST::MERGE: return exec_merge(std::get<sql::ast::MergeStmt>(stmt.data));
            case ST::BEGIN_TXN: return exec_begin(std::get<sql::ast::BeginStmt>(stmt.data));
            case ST::COMMIT_TXN: return exec_commit();
            case ST::ROLLBACK_TXN: return exec_rollback(stmt);
            case ST::SAVEPOINT: return exec_savepoint(std::get<sql::ast::SavepointStmt>(stmt.data));
            case ST::RELEASE_SAVEPOINT: return exec_release_savepoint(std::get<sql::ast::SavepointStmt>(stmt.data));
            case ST::EXPLAIN: return exec_explain(std::get<sql::ast::ExplainStmt>(stmt.data));
            case ST::CREATE_SEQUENCE: return exec_create_sequence(std::get<sql::ast::CreateSequenceStmt>(stmt.data));
            case ST::XPATH_QUERY: return exec_doc_query(stmt);
            case ST::XQUERY_QUERY: return exec_doc_query(stmt);
            case ST::GRAPHQL_QUERY: return exec_doc_query(stmt);
            case ST::ALTER_MATERIALIZED_VIEW: return {true, "", {}, {}, 0}; // stub
            case ST::SET_VARIABLE: return {true, "", {}, {}, 0}; // stub
            // v1.2.0: New statement types
            case ST::DROP_TRIGGER: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_SEQUENCE: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_PROCEDURE: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::DROP_FUNCTION: return exec_drop(std::get<sql::ast::DropStmt>(stmt.data));
            case ST::CREATE_TRIGGER: return exec_create_trigger(std::get<sql::ast::CreateTriggerStmt>(stmt.data));
            case ST::CREATE_PROCEDURE: return exec_create_procedure(std::get<sql::ast::CreateProcedureStmt>(stmt.data));
            case ST::CREATE_FUNCTION: return exec_create_procedure(std::get<sql::ast::CreateProcedureStmt>(stmt.data));
            case ST::CALL_STMT: return exec_call(std::get<sql::ast::CallStmt>(stmt.data));
            case ST::GRANT_STMT: return exec_grant(std::get<sql::ast::GrantStmt>(stmt.data));
            case ST::REVOKE_STMT: return exec_grant(std::get<sql::ast::GrantStmt>(stmt.data));
            case ST::PREPARE_STMT: return exec_prepare(std::get<sql::ast::PrepareStmt>(stmt.data));
            case ST::EXECUTE_STMT: return exec_execute(std::get<sql::ast::ExecuteStmt>(stmt.data));
            case ST::DEALLOCATE_STMT: return exec_deallocate(std::get<sql::ast::DeallocateStmt>(stmt.data));
            case ST::DECLARE_CURSOR: return exec_declare_cursor(std::get<sql::ast::DeclareCursorStmt>(stmt.data));
            case ST::OPEN_CURSOR: return exec_open_cursor(std::get<sql::ast::OpenCursorStmt>(stmt.data));
            case ST::CLOSE_CURSOR: return exec_close_cursor(std::get<sql::ast::CloseCursorStmt>(stmt.data));
            case ST::FETCH_CURSOR: return exec_fetch_cursor(std::get<sql::ast::FetchCursorStmt>(stmt.data));
            case ST::GRAPH_MATCH: return exec_graph_match(std::get<sql::ast::GraphMatchStmt>(stmt.data));
            default: return {false, "Statement type not yet supported", {}, {}, 0};
        }
    }

    // ===============================================
    // DDL
    // ===============================================

    sql::ResultSet exec_create_table(const sql::ast::CreateTableStmt &ct) {
        if (catalog_.find_table(ct.name) && !ct.if_not_exists)
            return {false, "Table '" + ct.name + "' already exists", {}, {}, 0};
        catalog::TableInfo ti;
        ti.name = ct.name; ti.schema = ct.schema.value_or("");
        ti.encrypted = ct.encrypted; ti.temporary = ct.temporary; ti.columnar = ct.columnar;
        for (auto &col : ct.columns) {
            catalog::ColumnInfo ci;
            ci.name = col.name; ci.type = col.type;
            ci.nullable = col.nullable; ci.encrypted = col.encrypted;
            ci.ordinal = (uint16_t)ti.columns.size();
            ci.generated = col.generated;
            ci.primary_key = col.primary_key;
            ci.unique = col.unique;
            ci.auto_increment = col.auto_increment;
            if (col.default_value)
                ci.default_value = std::move(const_cast<sql::ast::ExprPtr&>(col.default_value));
            if (col.check_expr)
                ci.check_expr = std::move(const_cast<sql::ast::ExprPtr&>(col.check_expr));
            if (col.generated && col.generated_expr) {
                ci.generated_expr = std::move(const_cast<sql::ast::ExprPtr&>(col.generated_expr));
                ci.generated_stored = (col.generated_mode == sql::ast::ColumnDef::GeneratedMode::STORED);
            }
            if (col.primary_key) ti.primary_key_cols.push_back(col.name);
            ti.columns.push_back(std::move(ci));
        }
        // Store table-level constraints (PK, UNIQUE, FK)
        for (auto &tc : ct.constraints) {
            using CT = sql::ast::TableConstraint::Type;
            if (tc.type == CT::PRIMARY_KEY) {
                ti.primary_key_cols = tc.columns;
                for (auto &pkc : tc.columns)
                    for (auto &ci : ti.columns) if (ci.name == pkc) ci.primary_key = true;
            } else if (tc.type == CT::UNIQUE) {
                for (auto &uc : tc.columns)
                    for (auto &ci : ti.columns) if (ci.name == uc) ci.unique = true;
            } else if (tc.type == CT::FOREIGN_KEY) {
                catalog::ForeignKeyInfo fk;
                fk.name = tc.name.value_or("");
                fk.columns = tc.columns; fk.ref_table = tc.ref_table;
                fk.ref_columns = tc.ref_columns;
                fk.on_delete = tc.on_delete; fk.on_update = tc.on_update;
                ti.foreign_keys.push_back(std::move(fk));
            }
        }
        // Partition spec
        if (ct.partition.has_value()) {
            auto &ps = ct.partition.value();
            catalog::TablePartitionSpec tps;
            tps.type = ps.type;
            tps.columns = ps.columns;
            // Resolve column indices
            for (auto &pcol : ps.columns) {
                int idx = -1;
                for (size_t i = 0; i < ti.columns.size(); i++) {
                    if (ti.columns[i].name == pcol) { idx = (int)i; break; }
                }
                if (idx < 0) return {false, "Partition column '" + pcol + "' not found", {}, {}, 0};
                tps.column_indices.push_back(idx);
            }
            // Build partitions
            for (auto &pd : ps.partitions) {
                catalog::PartitionInfo pi;
                pi.name = pd.name;
                pi.bound.is_maxvalue = pd.bound.is_maxvalue;
                pi.bound.is_default = pd.bound.is_default;
                if (pd.bound.modulus.has_value()) pi.bound.modulus = pd.bound.modulus.value();
                if (pd.bound.remainder.has_value()) pi.bound.remainder_val = pd.bound.remainder.value();
                for (auto &expr : pd.bound.values) {
                    if (expr) pi.bound.values.push_back(eval_expr(*expr, {}, {}));
                }
                tps.partitions.push_back(std::move(pi));
            }
            ti.partition_spec = std::move(tps);
        }
        catalog_.add_table(ct.name, std::move(ti));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_drop(const sql::ast::DropStmt &ds) {
        using ST = sql::ast::StmtType;
        bool found = false;
        switch (ds.target_type) {
            case ST::DROP_TABLE: found = catalog_.drop_table(ds.name); break;
            case ST::DROP_TYPE: {
                // Try composite, enum, then domain in that order.
                found = catalog_.drop_composite_type(ds.name)
                     || catalog_.drop_enum_type(ds.name)
                     || catalog_.drop_domain_type(ds.name);
                break;
            }
            case ST::DROP_DOMAIN: found = catalog_.drop_domain_type(ds.name); break;
            case ST::DROP_VIEW: found = catalog_.drop_view(ds.name); break;
            case ST::DROP_MATERIALIZED_VIEW: found = catalog_.drop_materialized_view(ds.name); break;
            case ST::DROP_TABLESPACE: found = catalog_.drop_tablespace(ds.name); break;
            case ST::DROP_SAVED_QUERY: found = catalog_.drop_saved_query(ds.name); break;
            case ST::DROP_INDEX: found = catalog_.drop_index(ds.name); break;
            case ST::DROP_TRIGGER: found = catalog_.drop_trigger(ds.name); break;
            case ST::DROP_SEQUENCE: found = catalog_.drop_sequence(ds.name); break;
            case ST::DROP_PROCEDURE: found = catalog_.drop_script(ds.name); break;
            case ST::DROP_FUNCTION: found = catalog_.drop_script(ds.name); break;
            default: break;
        }
        if (!found && !ds.if_exists)
            return {false, "Object '" + ds.name + "' does not exist", {}, {}, 0};
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_index(const sql::ast::CreateIndexStmt &ci) {
        catalog::IndexInfo ii;
        ii.name = ci.name; ii.table = ci.table; ii.method = ci.method; ii.unique = ci.unique;
        for (auto &col : ci.columns) ii.columns.push_back(col.name);
        catalog_.add_index(ci.name, std::move(ii));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_view(const sql::ast::CreateViewStmt &cv) {
        if (catalog_.find_view(cv.name)) {
            if (cv.if_not_exists) return {true, "", {}, {}, 0};
            if (!cv.or_replace) return {false, "View '" + cv.name + "' already exists", {}, {}, 0};
        }
        catalog::ViewInfo vi;
        vi.name = cv.name; vi.schema = cv.schema.value_or("");
        vi.columns = cv.columns; vi.or_replace = cv.or_replace;
        if (cv.query) vi.query = std::move(const_cast<sql::ast::SelectPtr&>(cv.query));
        catalog_.add_view(cv.name, std::move(vi));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_matview(const sql::ast::CreateMaterializedViewStmt &mv) {
        if (catalog_.find_materialized_view(mv.name) && !mv.if_not_exists)
            return {false, "Materialized view '" + mv.name + "' already exists", {}, {}, 0};
        catalog::MaterializedViewInfo mvi;
        mvi.name = mv.name; mvi.schema = mv.schema.value_or("");
        mvi.writable = mv.writable; mvi.with_data = mv.with_data;
        mvi.tablespace = mv.tablespace.value_or("");
        catalog_.add_materialized_view(mv.name, std::move(mvi));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_refresh_matview(const sql::ast::RefreshMaterializedViewStmt &rmv) {
        auto *mv = catalog_.find_materialized_view(rmv.name);
        if (!mv) return {false, "Materialized view '" + rmv.name + "' not found", {}, {}, 0};
        mv->rows.clear();
        mv->with_data = rmv.with_data;
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_saved_query(const sql::ast::CreateSavedQueryStmt &sq) {
        catalog::SavedQueryInfo sqi;
        sqi.name = sq.name; sqi.schema = sq.schema.value_or("");
        sqi.parameters = sq.parameters;
        catalog_.add_saved_query(sq.name, std::move(sqi));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_tablespace(const sql::ast::CreateTablespaceStmt &ts) {
        catalog::TablespaceInfo tsi;
        tsi.name = ts.name; tsi.location = ts.location;
        tsi.owner = ts.owner.value_or(""); tsi.options = ts.options;
        catalog_.add_tablespace(ts.name, std::move(tsi));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_truncate(const sql::ast::TruncateStmt &ts) {
        auto *table = catalog_.find_table(ts.table);
        if (!table) return {false, "Table '" + ts.table + "' not found", {}, {}, 0};
        int64_t count = (int64_t)table->total_row_count();
        table->rows.clear();
        if (table->is_partitioned()) {
            for (auto &p : table->partition_spec->partitions) p.rows.clear();
        }
        return {true, "", {}, {}, count};
    }

    sql::ResultSet exec_alter_table(const sql::ast::AlterTableStmt &at) {
        auto *table = catalog_.find_table(at.table);
        if (!table) return {false, "Table '" + at.table + "' not found", {}, {}, 0};
        using A = sql::ast::AlterTableStmt::Action;
        switch (at.action) {
            case A::ADD_COLUMN: {
                // Reject duplicate column name.
                for (auto &c : table->columns) {
                    if (c.name == at.column.name)
                        return {false, "Column '" + at.column.name + "' already exists", {}, {}, 0};
                }
                catalog::ColumnInfo ci;
                ci.name = at.column.name; ci.type = at.column.type;
                ci.nullable = at.column.nullable; ci.ordinal = (uint16_t)table->columns.size();
                ci.primary_key = at.column.primary_key;
                ci.unique = at.column.unique;
                ci.auto_increment = at.column.auto_increment;
                // Evaluate DEFAULT (if any) BEFORE moving the AST node into the catalog,
                // so we can backfill existing rows with the default value.
                sql::Value default_val = at.column.default_value
                    ? eval_expr_with_row(*at.column.default_value, {}, {})
                    : sql::Value::make_null();
                if (at.column.check_expr)
                    ci.check_expr = std::move(const_cast<sql::ast::ExprPtr&>(at.column.check_expr));
                if (at.column.default_value)
                    ci.default_value = std::move(const_cast<sql::ast::ExprPtr&>(at.column.default_value));
                table->columns.push_back(std::move(ci));
                for (auto &row : table->rows) row.push_back(default_val);
                break;
            }
            case A::DROP_COLUMN: {
                int idx = -1;
                for (size_t i = 0; i < table->columns.size(); i++) {
                    if (table->columns[i].name == at.target_name) { idx = (int)i; break; }
                }
                if (idx < 0) return {false, "Column '" + at.target_name + "' not found", {}, {}, 0};
                table->columns.erase(table->columns.begin() + idx);
                for (auto &row : table->rows) {
                    if (idx < (int)row.size()) row.erase(row.begin() + idx);
                }
                // Re-number ordinals so they stay contiguous.
                for (size_t i = 0; i < table->columns.size(); i++)
                    table->columns[i].ordinal = (uint16_t)i;
                // Drop foreign keys that referenced this column.
                table->foreign_keys.erase(
                    std::remove_if(table->foreign_keys.begin(), table->foreign_keys.end(),
                        [&](const catalog::ForeignKeyInfo &fk) {
                            for (auto &c : fk.columns)
                                if (c == at.target_name) return true;
                            return false;
                        }),
                    table->foreign_keys.end());
                // Remove from primary_key_cols if present.
                table->primary_key_cols.erase(
                    std::remove(table->primary_key_cols.begin(), table->primary_key_cols.end(), at.target_name),
                    table->primary_key_cols.end());
                break;
            }
            case A::RENAME_COLUMN: {
                bool found = false;
                for (auto &col : table->columns) {
                    if (col.name == at.target_name) { col.name = at.new_name; found = true; break; }
                }
                if (!found) return {false, "Column '" + at.target_name + "' not found", {}, {}, 0};
                // Rename in primary_key_cols and foreign_keys too.
                for (auto &pk : table->primary_key_cols)
                    if (pk == at.target_name) pk = at.new_name;
                for (auto &fk : table->foreign_keys)
                    for (auto &c : fk.columns)
                        if (c == at.target_name) c = at.new_name;
                break;
            }
            case A::RENAME_TABLE: {
                catalog::TableInfo ti = std::move(*table);
                catalog_.drop_table(at.table);
                ti.name = at.new_name;
                catalog_.add_table(at.new_name, std::move(ti));
                break;
            }
            case A::ALTER_COLUMN: {
                int idx = -1;
                for (size_t i = 0; i < table->columns.size(); i++) {
                    if (table->columns[i].name == at.target_name) { idx = (int)i; break; }
                }
                if (idx < 0) return {false, "Column '" + at.target_name + "' not found", {}, {}, 0};
                auto &col = table->columns[(size_t)idx];
                // ALTER COLUMN supports type and nullability changes. Other knobs are
                // accepted but ignored — see ROADMAP_MVP.md.
                col.type = at.column.type;
                col.nullable = at.column.nullable;
                if (at.column.default_value)
                    col.default_value = std::move(const_cast<sql::ast::ExprPtr&>(at.column.default_value));
                break;
            }
            case A::ADD_CONSTRAINT: {
                using CT = sql::ast::TableConstraint::Type;
                const auto &con = at.constraint;
                switch (con.type) {
                    case CT::PRIMARY_KEY: {
                        if (!table->primary_key_cols.empty())
                            return {false, "Primary key already defined on '" + at.table + "'", {}, {}, 0};
                        for (auto &c : con.columns) {
                            bool exists = false;
                            for (auto &tc : table->columns) {
                                if (tc.name == c) { tc.primary_key = true; exists = true; break; }
                            }
                            if (!exists)
                                return {false, "Column '" + c + "' not found", {}, {}, 0};
                        }
                        table->primary_key_cols = con.columns;
                        break;
                    }
                    case CT::UNIQUE: {
                        if (con.columns.size() != 1)
                            return {false, "Multi-column UNIQUE constraints are not yet supported", {}, {}, 0};
                        bool exists = false;
                        for (auto &tc : table->columns) {
                            if (tc.name == con.columns[0]) { tc.unique = true; exists = true; break; }
                        }
                        if (!exists)
                            return {false, "Column '" + con.columns[0] + "' not found", {}, {}, 0};
                        break;
                    }
                    case CT::FOREIGN_KEY: {
                        catalog::ForeignKeyInfo fki;
                        fki.name = con.name.value_or(at.table + "_" + con.columns.front() + "_fkey");
                        fki.columns = con.columns;
                        fki.ref_table = con.ref_table;
                        fki.ref_columns = con.ref_columns;
                        fki.on_delete = con.on_delete;
                        fki.on_update = con.on_update;
                        table->foreign_keys.push_back(std::move(fki));
                        break;
                    }
                    case CT::CHECK: {
                        // Without table-level CHECK storage we only support single-column CHECKs.
                        if (con.columns.size() == 1) {
                            for (auto &tc : table->columns) {
                                if (tc.name == con.columns[0]) {
                                    if (con.check_expr)
                                        tc.check_expr = std::move(const_cast<sql::ast::ExprPtr&>(con.check_expr));
                                    break;
                                }
                            }
                        } else {
                            return {false, "Table-level CHECK constraints are not yet supported", {}, {}, 0};
                        }
                        break;
                    }
                }
                break;
            }
            case A::DROP_CONSTRAINT: {
                // Look for an FK by name first.
                auto fk_it = std::find_if(table->foreign_keys.begin(), table->foreign_keys.end(),
                    [&](const catalog::ForeignKeyInfo &fk) { return fk.name == at.target_name; });
                if (fk_it != table->foreign_keys.end()) {
                    table->foreign_keys.erase(fk_it);
                    break;
                }
                // Treat 'PRIMARY' or the conventional pk name as dropping the PK.
                std::string lower = at.target_name;
                for (auto &c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower == "primary" || lower.find("_pkey") != std::string::npos) {
                    for (auto &tc : table->columns) tc.primary_key = false;
                    table->primary_key_cols.clear();
                    break;
                }
                return {false, "Constraint '" + at.target_name + "' not found", {}, {}, 0};
            }
            case A::ADD_PARTITION: {
                if (!table->is_partitioned())
                    return {false, "Table '" + at.table + "' is not partitioned", {}, {}, 0};
                auto &spec = table->partition_spec.value();
                // Check duplicate name
                for (auto &p : spec.partitions) {
                    if (p.name == at.partition.name)
                        return {false, "Partition '" + at.partition.name + "' already exists", {}, {}, 0};
                }
                catalog::PartitionInfo pi;
                pi.name = at.partition.name;
                pi.bound.is_maxvalue = at.partition.bound.is_maxvalue;
                pi.bound.is_default = at.partition.bound.is_default;
                if (at.partition.bound.modulus.has_value())
                    pi.bound.modulus = at.partition.bound.modulus.value();
                if (at.partition.bound.remainder.has_value())
                    pi.bound.remainder_val = at.partition.bound.remainder.value();
                for (auto &expr : at.partition.bound.values) {
                    if (expr) pi.bound.values.push_back(eval_expr(*expr, {}, {}));
                }
                spec.partitions.push_back(std::move(pi));
                break;
            }
            case A::DROP_PARTITION: {
                if (!table->is_partitioned())
                    return {false, "Table '" + at.table + "' is not partitioned", {}, {}, 0};
                auto &parts = table->partition_spec->partitions;
                auto it = std::find_if(parts.begin(), parts.end(),
                    [&](const catalog::PartitionInfo &p) { return p.name == at.partition.name; });
                if (it == parts.end())
                    return {false, "Partition '" + at.partition.name + "' not found", {}, {}, 0};
                parts.erase(it);
                break;
            }
        }
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_explain(const sql::ast::ExplainStmt &es) {
        sql::ResultSet result;
        result.success = true;
        result.columns = {{"QUERY PLAN", ""}};
        std::string plan = "Statement(" + sql::ast::ast_to_string(*es.statement) + ")";
        result.rows.push_back({sql::Value::make_string(plan)});
        return result;
    }

    sql::ResultSet exec_create_type(const sql::ast::CreateTypeStmt &ct) {
        // Dispatch on the shape — ENUM, DOMAIN, or composite.
        if (ct.is_enum) {
            if (catalog_.find_enum_type(ct.name)) {
                if (ct.if_not_exists) return {true, "", {}, {}, 0};
                return {false, "Enum type '" + ct.name + "' already exists", {}, {}, 0};
            }
            catalog::EnumTypeInfo info;
            info.name = ct.name;
            info.schema = ct.schema.value_or("");
            info.labels = ct.enum_labels;
            catalog_.add_enum_type(ct.name, std::move(info));
            return {true, "", {}, {}, 0};
        }
        if (ct.is_domain) {
            return exec_create_domain_inline(ct);
        }
        if (catalog_.find_composite_type(ct.name)) {
            if (ct.if_not_exists) return {true, "", {}, {}, 0};
            return {false, "Type '" + ct.name + "' already exists", {}, {}, 0};
        }
        catalog::CompositeTypeInfo info;
        info.name = ct.name;
        info.schema = ct.schema.value_or("");
        for (auto &f : ct.fields) {
            catalog::CompositeFieldInfo fi;
            fi.name = f.field_name;
            fi.type = f.type;
            info.fields.push_back(std::move(fi));
        }
        catalog_.add_composite_type(ct.name, std::move(info));
        return {true, "", {}, {}, 0};
    }

    // Shared path for both `CREATE DOMAIN` and `CREATE TYPE ... AS BASETYPE`
    // forms — they end up at the same catalog object.
    sql::ResultSet exec_create_domain_inline(const sql::ast::CreateTypeStmt &ct) {
        if (catalog_.find_domain_type(ct.name)) {
            if (ct.if_not_exists) return {true, "", {}, {}, 0};
            return {false, "Domain '" + ct.name + "' already exists", {}, {}, 0};
        }
        catalog::DomainTypeInfo info;
        info.name = ct.name;
        info.schema = ct.schema.value_or("");
        info.base_type = ct.domain_base;
        if (ct.check_expr)
            info.check_expr = std::move(const_cast<sql::ast::ExprPtr&>(ct.check_expr));
        catalog_.add_domain_type(ct.name, std::move(info));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_domain(const sql::ast::CreateTypeStmt &ct) {
        // Parser routes CREATE DOMAIN to a CreateTypeStmt with is_domain=true.
        return exec_create_domain_inline(ct);
    }

    sql::ResultSet exec_alter_type(const sql::ast::AlterTypeStmt &at) {
        using Action = sql::ast::AlterTypeStmt::Action;
        // Try composite first, then enum.
        auto *comp = catalog_.find_composite_type(at.name);
        auto *en   = catalog_.find_enum_type(at.name);
        if (!comp && !en)
            return {false, "Type '" + at.name + "' not found", {}, {}, 0};
        switch (at.action) {
            case Action::ADD_ATTRIBUTE: {
                if (!comp) return {false, "ADD ATTRIBUTE requires a composite type", {}, {}, 0};
                for (auto &f : comp->fields)
                    if (f.name == at.attr_name)
                        return {false, "Attribute '" + at.attr_name + "' already exists", {}, {}, 0};
                catalog::CompositeFieldInfo fi;
                fi.name = at.attr_name; fi.type = at.attr_type;
                comp->fields.push_back(std::move(fi));
                return {true, "", {}, {}, 0};
            }
            case Action::DROP_ATTRIBUTE: {
                if (!comp) return {false, "DROP ATTRIBUTE requires a composite type", {}, {}, 0};
                auto it = std::find_if(comp->fields.begin(), comp->fields.end(),
                    [&](const catalog::CompositeFieldInfo &f){ return f.name == at.target_name; });
                if (it == comp->fields.end())
                    return {false, "Attribute '" + at.target_name + "' not found", {}, {}, 0};
                comp->fields.erase(it);
                return {true, "", {}, {}, 0};
            }
            case Action::RENAME_ATTRIBUTE: {
                if (!comp) return {false, "RENAME ATTRIBUTE requires a composite type", {}, {}, 0};
                for (auto &f : comp->fields) {
                    if (f.name == at.new_name)
                        return {false, "Attribute '" + at.new_name + "' already exists", {}, {}, 0};
                }
                for (auto &f : comp->fields) {
                    if (f.name == at.target_name) { f.name = at.new_name; return {true, "", {}, {}, 0}; }
                }
                return {false, "Attribute '" + at.target_name + "' not found", {}, {}, 0};
            }
            case Action::RENAME_TYPE: {
                // Works for composites and enums uniformly.
                if (comp) {
                    catalog::CompositeTypeInfo ti = std::move(*comp);
                    catalog_.drop_composite_type(at.name);
                    ti.name = at.new_name;
                    catalog_.add_composite_type(at.new_name, std::move(ti));
                } else {
                    catalog::EnumTypeInfo ti = std::move(*en);
                    catalog_.drop_enum_type(at.name);
                    ti.name = at.new_name;
                    catalog_.add_enum_type(at.new_name, std::move(ti));
                }
                return {true, "", {}, {}, 0};
            }
            case Action::ADD_ENUM_VALUE: {
                if (!en) return {false, "ADD VALUE requires an enum type", {}, {}, 0};
                for (auto &l : en->labels)
                    if (l == at.enum_value)
                        return {false, "Enum label '" + at.enum_value + "' already exists", {}, {}, 0};
                en->labels.push_back(at.enum_value);
                return {true, "", {}, {}, 0};
            }
            case Action::RENAME_ENUM_VALUE: {
                if (!en) return {false, "RENAME VALUE requires an enum type", {}, {}, 0};
                // Reject if the new label already exists (and isn't a no-op rename).
                for (auto &l : en->labels)
                    if (l == at.enum_value && l != at.target_name)
                        return {false, "Enum label '" + at.enum_value + "' already exists", {}, {}, 0};
                for (auto &l : en->labels) {
                    if (l == at.target_name) { l = at.enum_value; return {true, "", {}, {}, 0}; }
                }
                return {false, "Enum label '" + at.target_name + "' not found", {}, {}, 0};
            }
        }
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_sequence(const sql::ast::CreateSequenceStmt &cs) {
        if (cs.if_not_exists && catalog_.find_sequence(cs.name))
            return {true, "", {}, {}, 0};
        catalog::SequenceInfo si;
        si.name = cs.name;
        si.schema = cs.schema.value_or("");
        si.start = cs.start;
        si.increment = cs.increment;
        si.current_value = cs.start - cs.increment; // nextval will advance to start
        si.min_value = cs.min_value.value_or(1);
        si.max_value = cs.max_value.value_or(INT64_MAX);
        si.cycle = cs.cycle;
        catalog_.add_sequence(cs.name, std::move(si));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_doc_query(const sql::ast::Statement &/*stmt*/) {
        return {true, "Document query executed", {{"result",""}}, {}, 0};
    }

    // ===============================================
    // v1.2.0: Triggers, Procedures, GRANT, Prepared, Cursors
    // ===============================================

    sql::ResultSet exec_create_trigger(const sql::ast::CreateTriggerStmt &ct) {
        if (ct.if_not_exists && catalog_.find_trigger(ct.name))
            return {true, "", {}, {}, 0};
        if (!ct.or_replace && catalog_.find_trigger(ct.name))
            return {false, "Trigger '" + ct.name + "' already exists", {}, {}, 0};
        if (!catalog_.find_table(ct.table))
            return {false, "Table '" + ct.table + "' not found for trigger", {}, {}, 0};
        // Store trigger and its body as a script
        catalog::ScriptInfo si;
        si.name = "__trigger_" + ct.name;
        si.lua_source = ct.body; // works for lua; for plsql, body is stored as-is
        catalog_.add_script(si.name, std::move(si));
        catalog::TriggerInfo ti;
        ti.name = ct.name;
        ti.table = ct.table;
        ti.timing = (ct.timing == "BEFORE") ? catalog::TriggerTiming::BEFORE : catalog::TriggerTiming::AFTER;
        ti.event = (ct.event == "INSERT") ? catalog::TriggerEvent::INSERT :
                   (ct.event == "UPDATE") ? catalog::TriggerEvent::UPDATE : catalog::TriggerEvent::DELETE;
        ti.script_name = "__trigger_" + ct.name;
        if (ct.or_replace) catalog_.drop_trigger(ct.name);
        catalog_.add_trigger(ct.name, std::move(ti));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_create_procedure(const sql::ast::CreateProcedureStmt &cp) {
        if (cp.if_not_exists && catalog_.find_script(cp.name))
            return {true, "", {}, {}, 0};
        if (!cp.or_replace && catalog_.find_script(cp.name))
            return {false, std::string(cp.is_function ? "Function" : "Procedure") + " '" + cp.name + "' already exists", {}, {}, 0};
        catalog::ScriptInfo si;
        si.name = cp.name;
        si.schema = cp.schema.value_or("");
        si.lua_source = cp.body; // stores the body for any language
        si.is_udf = cp.is_function;
        si.has_return = cp.is_function;
        for (auto &p : cp.params) {
            si.params.push_back({p.name, p.type.name});
        }
        if (cp.is_function) si.return_type = cp.return_type.name;
        if (cp.or_replace) catalog_.drop_script(cp.name);
        catalog_.add_script(cp.name, std::move(si));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_call(const sql::ast::CallStmt &cs) {
        auto *si = catalog_.find_script(cs.name);
        if (!si) return {false, "Procedure '" + cs.name + "' not found", {}, {}, 0};
        // Evaluate arguments
        std::vector<sql::Value> args;
        for (auto &ae : cs.args)
            args.push_back(eval_expr_with_row(*ae, {}, {}));

        // Bind parameters as session variables
        for (size_t i = 0; i < si->params.size() && i < args.size(); i++) {
            plsql_vars_[si->params[i].name] = args[i];
        }

        // Execute the body using the PL/SQL interpreter
        auto result = exec_plsql_body(si->lua_source);

        // Clean up parameter bindings
        for (size_t i = 0; i < si->params.size(); i++) {
            plsql_vars_.erase(si->params[i].name);
        }
        return result;
    }

    // ─── PL/SQL variable storage ───
    std::unordered_map<std::string, sql::Value> plsql_vars_;

    // ─── PL/SQL body interpreter ───
    // Executes a PL/SQL block: supports variable declarations (DECLARE),
    // assignments (:=), IF/ELSIF/ELSE/END IF, WHILE/LOOP/END LOOP,
    // FOR ... IN ... LOOP, RETURN, RAISE, and embedded SQL statements.
    // ─── PL/SQL variable substitution (single-pass, safe) ───
    std::string plsql_subst_vars(const std::string &expr) {
        std::string result = expr;
        std::vector<std::pair<std::string, std::string>> sorted_vars;
        for (auto &[name, val] : plsql_vars_) {
            // Format values as SQL-safe literals: quote strings, leave numbers as-is
            std::string val_str;
            if (val.type == sql::Value::Type::STRING)
                val_str = "'" + val.str_val + "'";
            else if (val.type == sql::Value::Type::NULL_VAL)
                val_str = "NULL";
            else if (val.type == sql::Value::Type::BOOL)
                val_str = val.bool_val ? "TRUE" : "FALSE";
            else
                val_str = val.to_string();
            sorted_vars.push_back({name, val_str});
        }
        std::sort(sorted_vars.begin(), sorted_vars.end(),
            [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
        for (auto &[name, val_str] : sorted_vars) {
            if (name.empty()) continue; // skip empty variable names
            size_t pos = 0;
            while (pos < result.size()) {
                pos = result.find(name, pos);
                if (pos == std::string::npos) break;
                // Check word boundary: ensure not part of a longer identifier
                bool left_ok = (pos == 0 || !std::isalnum((unsigned char)result[pos - 1]));
                bool right_ok = (pos + name.size() >= result.size() ||
                                 !std::isalnum((unsigned char)result[pos + name.size()]));
                if (left_ok && right_ok) {
                    result.replace(pos, name.size(), val_str);
                    pos += val_str.size(); // skip past the replacement
                } else {
                    pos += name.size();
                }
            }
        }
        return result;
    }

    // ─── PL/SQL tokenizer: splits body into logical tokens ───
    struct PLToken {
        std::string text;
        std::string upper; // uppercased for keyword matching
    };

    std::vector<PLToken> plsql_tokenize(const std::string &body) {
        std::vector<PLToken> tokens;
        std::string buf;
        bool in_string = false;
        for (size_t i = 0; i < body.size(); i++) {
            char c = body[i];
            if (c == '\'' && !in_string) { in_string = true; buf += c; continue; }
            if (c == '\'' && in_string) { in_string = false; buf += c; continue; }
            if (in_string) { buf += c; continue; }
            if (c == '-' && i + 1 < body.size() && body[i+1] == '-') {
                while (i < body.size() && body[i] != '\n') i++;
                continue;
            }
            if (c == ';') {
                if (!buf.empty()) {
                    std::string upper = buf;
                    for (auto &ch : upper) ch = (char)std::toupper((unsigned char)ch);
                    // Trim
                    size_t s = upper.find_first_not_of(" \t\r\n");
                    if (s != std::string::npos) { upper = upper.substr(s); buf = buf.substr(s); }
                    while (!upper.empty() && (upper.back() == ' ' || upper.back() == '\t' || upper.back() == '\n')) { upper.pop_back(); buf.pop_back(); }
                    if (!buf.empty()) tokens.push_back({buf, upper});
                }
                buf.clear();
                continue;
            }
            buf += c;
        }
        if (!buf.empty()) {
            std::string upper = buf;
            for (auto &ch : upper) ch = (char)std::toupper((unsigned char)ch);
            size_t s = upper.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) { upper = upper.substr(s); buf = buf.substr(s); }
            while (!upper.empty() && (upper.back() == ' ' || upper.back() == '\t' || upper.back() == '\n')) { upper.pop_back(); buf.pop_back(); }
            if (!buf.empty()) tokens.push_back({buf, upper});
        }
        return tokens;
    }

    // ─── PL/SQL block interpreter ───
    // Processes tokens[pos..end), returns last result. Sets `returned` if RETURN hit.
    sql::ResultSet plsql_exec_block(const std::vector<PLToken> &tokens,
                                     size_t &pos, size_t end, bool &returned) {
        sql::ResultSet last_result;
        last_result.success = true;

        int _max_iters = 100000;
        while (pos < end && !returned && _max_iters-- > 0) {
            auto &tok = tokens[pos];
            (void)_max_iters; // used for safety

            // DECLARE var type [:= expr]
            if (tok.upper.substr(0, 7) == "DECLARE") {
                // Simple variable declaration: DECLARE x INT := 10
                // or just skip unrecognized single-word tokens
                if (tok.upper.substr(0, 7) == "DECLARE") {
                    auto rest = tok.text.substr(7);
                    auto assign_pos = rest.find(":=");
                    if (assign_pos != std::string::npos) {
                        std::string var = rest.substr(0, assign_pos);
                        // Trim leading and trailing whitespace
                        while (!var.empty() && var.front() == ' ') var.erase(var.begin());
                        while (!var.empty() && var.back() == ' ') var.pop_back();
                        // Extract just the variable name (first word before type)
                        auto sp = var.find_first_of(" \t");
                        if (sp != std::string::npos) var = var.substr(0, sp);
                        std::string val_str = rest.substr(assign_pos + 2);
                        while (!val_str.empty() && val_str.front() == ' ') val_str.erase(val_str.begin());
                        while (!val_str.empty() && val_str.back() == ' ') val_str.pop_back();
                        // Try to parse as literal
                        if (!val_str.empty() && (std::isdigit((unsigned char)val_str[0]) || val_str[0] == '-')) {
                            try { plsql_vars_[var] = sql::Value::make_int(std::stoll(val_str)); }
                            catch (...) {
                                try { plsql_vars_[var] = sql::Value::make_float(std::stod(val_str)); }
                                catch (...) { plsql_vars_[var] = sql::Value::make_string(val_str); }
                            }
                        } else if (val_str.front() == '\'') {
                            // String literal
                            std::string s = val_str.substr(1);
                            if (!s.empty() && s.back() == '\'') s.pop_back();
                            plsql_vars_[var] = sql::Value::make_string(s);
                        } else if (val_str == "TRUE" || val_str == "true") {
                            plsql_vars_[var] = sql::Value::make_bool(true);
                        } else if (val_str == "FALSE" || val_str == "false") {
                            plsql_vars_[var] = sql::Value::make_bool(false);
                        } else if (val_str == "NULL" || val_str == "null") {
                            plsql_vars_[var] = sql::Value::make_null();
                        } else {
                            plsql_vars_[var] = sql::Value::make_string(val_str);
                        }
                    } else {
                        // DECLARE var type (no assignment)
                        // Just register the variable as null
                        std::string var = rest;
                        while (!var.empty() && var.front() == ' ') var.erase(var.begin());
                        auto sp = var.find_first_of(" \t");
                        if (sp != std::string::npos) var = var.substr(0, sp);
                        if (!var.empty()) plsql_vars_[var] = sql::Value::make_null();
                    }
                    pos++;
                    continue;
                }
            }

            // Variable assignment: var := expr
            // IMPORTANT: Must check AFTER FOR/WHILE/IF because those tokens may contain :=
            // in their body (e.g., "FOR i IN 1..5 LOOP x := x + 1")
            if (tok.text.find(":=") != std::string::npos
                && tok.upper.substr(0, 3) != "FOR"
                && tok.upper.substr(0, 5) != "WHILE"
                && tok.upper.substr(0, 2) != "IF") {
                auto ap = tok.text.find(":=");
                std::string var = tok.text.substr(0, ap);
                while (!var.empty() && var.back() == ' ') var.pop_back();
                while (!var.empty() && var.front() == ' ') var.erase(var.begin());
                std::string val_expr = tok.text.substr(ap + 2);
                val_expr = plsql_subst_vars(val_expr);
                auto r = execute("SELECT " + val_expr);
                if (r.success && !r.rows.empty() && !r.rows[0].empty() && !r.rows[0][0].is_null()) {
                    plsql_vars_[var] = r.rows[0][0];
                } else {
                    // If execute fails or returns NULL, treat the value as a string literal
                    std::string trimmed_val = val_expr;
                    while (!trimmed_val.empty() && trimmed_val.front() == ' ') trimmed_val.erase(trimmed_val.begin());
                    while (!trimmed_val.empty() && trimmed_val.back() == ' ') trimmed_val.pop_back();
                    // Remove quotes if present
                    if (trimmed_val.size() >= 2 && trimmed_val.front() == '\'' && trimmed_val.back() == '\'')
                        trimmed_val = trimmed_val.substr(1, trimmed_val.size() - 2);
                    plsql_vars_[var] = sql::Value::make_string(trimmed_val);
                }
                pos++;
                continue;
            }

            // RETURN [expr]
            if (tok.upper.substr(0, 6) == "RETURN") {
                if (tok.text.size() > 6) {
                    std::string val_expr = tok.text.substr(6);
                    val_expr = plsql_subst_vars(val_expr);
                    auto r = execute("SELECT " + val_expr);
                    if (r.success && !r.rows.empty()) last_result = r;
                }
                returned = true;
                pos++;
                return last_result;
            }

            // IF condition THEN ... [ELSIF condition THEN ...] [ELSE ...] END IF
            if (tok.upper.substr(0, 2) == "IF" && tok.upper.find("THEN") != std::string::npos) {
                auto then_pos = tok.upper.find("THEN");
                std::string cond_text = tok.text.substr(2, then_pos - 2);
                cond_text = plsql_subst_vars(cond_text);
                auto cond_r = execute("SELECT CASE WHEN (" + cond_text + ") THEN 1 ELSE 0 END");
                bool cond = cond_r.success && !cond_r.rows.empty() && !cond_r.rows[0].empty()
                            && cond_r.rows[0][0].int_val != 0;

                // Extract inline body after THEN in the same token
                std::string inline_then;
                if (then_pos + 4 < tok.text.size()) {
                    inline_then = tok.text.substr(then_pos + 4);
                    while (!inline_then.empty() && inline_then.front() == ' ') inline_then.erase(inline_then.begin());
                }

                // Collect branch bodies by scanning tokens for ELSIF/ELSE/END IF
                pos++;
                struct IfBranch { bool condition; std::string body; };
                std::vector<IfBranch> branches;
                std::string cur_body = inline_then;
                bool cur_cond = cond;

                while (pos < end) {
                    auto &t = tokens[pos];
                    if (t.upper == "END IF" || t.upper.substr(0, 6) == "END IF") {
                        branches.push_back({cur_cond, cur_body});
                        pos++;
                        break;
                    }
                    if (t.upper.substr(0, 5) == "ELSIF" && t.upper.find("THEN") != std::string::npos) {
                        branches.push_back({cur_cond, cur_body});
                        auto ep = t.upper.find("THEN");
                        std::string ec = t.text.substr(5, ep - 5);
                        ec = plsql_subst_vars(ec);
                        auto er = execute("SELECT CASE WHEN (" + ec + ") THEN 1 ELSE 0 END");
                        cur_cond = er.success && !er.rows.empty() && !er.rows[0].empty()
                                   && er.rows[0][0].int_val != 0;
                        cur_body = "";
                        if (ep + 4 < t.text.size()) {
                            cur_body = t.text.substr(ep + 4);
                            while (!cur_body.empty() && cur_body.front() == ' ') cur_body.erase(cur_body.begin());
                        }
                        pos++;
                        continue;
                    }
                    if (t.upper == "ELSE") {
                        branches.push_back({cur_cond, cur_body});
                        cur_cond = true; // ELSE always runs if reached
                        cur_body = "";
                        // Check for inline text after ELSE
                        if (t.text.size() > 4) {
                            cur_body = t.text.substr(4);
                            while (!cur_body.empty() && cur_body.front() == ' ') cur_body.erase(cur_body.begin());
                        }
                        pos++;
                        continue;
                    }
                    // Regular body token — append to current branch
                    cur_body += "; " + t.text;
                    pos++;
                }

                // Execute the first branch whose condition is true
                bool executed = false;
                for (auto &br : branches) {
                    if (!executed && br.condition && !br.body.empty()) {
                        auto body_tokens = plsql_tokenize(br.body);
                        size_t bp = 0;
                        last_result = plsql_exec_block(body_tokens, bp, body_tokens.size(), returned);
                        executed = true;
                    }
                }
                continue;
            }

            // WHILE condition LOOP ... END LOOP
            if (tok.upper.substr(0, 5) == "WHILE" && tok.upper.find("LOOP") != std::string::npos) {
                auto loop_pos = tok.upper.find("LOOP");
                std::string cond_text = tok.text.substr(5, loop_pos - 5);

                // Extract inline body after LOOP keyword
                std::string inline_body;
                if (loop_pos + 4 < tok.text.size()) {
                    inline_body = tok.text.substr(loop_pos + 4);
                    while (!inline_body.empty() && inline_body.front() == ' ') inline_body.erase(inline_body.begin());
                }
                pos++;
                std::string full_body = inline_body;
                while (pos < end) {
                    auto &t = tokens[pos];
                    if (t.upper == "END LOOP" || t.upper.substr(0, 8) == "END LOOP") {
                        pos++; break;
                    }
                    full_body += "; " + t.text;
                    pos++;
                }
                auto body_tokens = plsql_tokenize(full_body);

                int max_iter = 10000;
                while (max_iter-- > 0 && !returned) {
                    std::string subst_cond = cond_text;
                    subst_cond = plsql_subst_vars(subst_cond);
                    auto cr = execute("SELECT CASE WHEN (" + subst_cond + ") THEN 1 ELSE 0 END");
                    bool cont = cr.success && !cr.rows.empty() && !cr.rows[0].empty()
                                && cr.rows[0][0].int_val != 0;
                    if (!cont) break;
                    size_t bp = 0;
                    last_result = plsql_exec_block(body_tokens, bp, body_tokens.size(), returned);
                }
                continue;
            }

            // FOR var IN start..end LOOP ... END LOOP
            if (tok.upper.substr(0, 3) == "FOR" && tok.upper.find("LOOP") != std::string::npos) {
                auto in_pos = tok.upper.find(" IN ");
                auto loop_kw = tok.upper.find("LOOP");
                if (in_pos != std::string::npos && loop_kw != std::string::npos) {
                    std::string var_name = tok.text.substr(3, in_pos - 3);
                    while (!var_name.empty() && var_name.front() == ' ') var_name.erase(var_name.begin());
                    while (!var_name.empty() && var_name.back() == ' ') var_name.pop_back();
                    std::string range_text = tok.text.substr(in_pos + 4, loop_kw - in_pos - 4);
                    while (!range_text.empty() && range_text.front() == ' ') range_text.erase(range_text.begin());
                    while (!range_text.empty() && range_text.back() == ' ') range_text.pop_back();

                    bool reverse = false;
                    std::string upper_range = range_text;
                    for (auto &ch : upper_range) ch = (char)std::toupper((unsigned char)ch);
                    if (upper_range.substr(0, 7) == "REVERSE") {
                        reverse = true; range_text = range_text.substr(7);
                        while (!range_text.empty() && range_text.front() == ' ') range_text.erase(range_text.begin());
                    }
                    auto dot_pos = range_text.find("..");
                    int64_t range_start = 1, range_end = 10;
                    if (dot_pos != std::string::npos) {
                        try { range_start = std::stoll(range_text.substr(0, dot_pos)); } catch (...) {}
                        try { range_end = std::stoll(range_text.substr(dot_pos + 2)); } catch (...) {}
                    }

                    // Extract inline body (text after LOOP keyword in the same token)
                    std::string inline_body;
                    if (loop_kw + 4 < tok.text.size()) {
                        inline_body = tok.text.substr(loop_kw + 4);
                        while (!inline_body.empty() && inline_body.front() == ' ') inline_body.erase(inline_body.begin());
                    }

                    // Collect body: inline body + subsequent tokens until END LOOP
                    std::string full_body = inline_body;
                    pos++;
                    while (pos < end) {
                        auto &t = tokens[pos];
                        if (t.upper == "END LOOP" || t.upper.substr(0, 8) == "END LOOP") {
                            pos++; break;
                        }
                        full_body += "; " + t.text;
                        pos++;
                    }

                    // Tokenize the body and execute for each iteration
                    auto body_tokens = plsql_tokenize(full_body);
                    if (reverse) {
                        for (int64_t i = range_end; i >= range_start && !returned; i--) {
                            plsql_vars_[var_name] = sql::Value::make_int(i);
                            size_t bp = 0;
                            last_result = plsql_exec_block(body_tokens, bp, body_tokens.size(), returned);
                        }
                    } else {
                        for (int64_t i = range_start; i <= range_end && !returned; i++) {
                            plsql_vars_[var_name] = sql::Value::make_int(i);
                            size_t bp = 0;
                            last_result = plsql_exec_block(body_tokens, bp, body_tokens.size(), returned);
                        }
                    }
                    plsql_vars_.erase(var_name);
                    continue;
                }
            }

            // RAISE NOTICE/EXCEPTION 'message'
            if (tok.upper.substr(0, 5) == "RAISE") {
                std::string rest = tok.text.substr(5);
                while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
                std::string upper_rest = rest;
                for (auto &c : upper_rest) c = (char)std::toupper((unsigned char)c);
                if (upper_rest.substr(0, 9) == "EXCEPTION") {
                    std::string msg = rest.substr(9);
                    while (!msg.empty() && msg.front() == ' ') msg.erase(msg.begin());
                    if (msg.front() == '\'') msg = msg.substr(1);
                    if (!msg.empty() && msg.back() == '\'') msg.pop_back();
                    returned = true;
                    return {false, "EXCEPTION: " + msg, {}, {}, 0};
                }
                // RAISE NOTICE — just log it
                pos++;
                continue;
            }

            // NULL — no-op statement
            if (tok.upper == "NULL") {
                pos++;
                continue;
            }

            // Otherwise treat as embedded SQL (with variable substitution)
            std::string sql = tok.text;
            sql = plsql_subst_vars(sql);
            last_result = execute(sql);
            if (!last_result.success) {
                returned = true;
                return last_result;
            }
            pos++;
        }
        return last_result;
    }

    sql::ResultSet exec_plsql_body(const std::string &body) {
        auto tokens = plsql_tokenize(body);
        bool returned = false;
        size_t pos = 0;
        return plsql_exec_block(tokens, pos, tokens.size(), returned);
    }

    sql::ResultSet exec_grant(const sql::ast::GrantStmt &gs) {
        if (gs.is_revoke) {
            // Remove matching privileges
            // TODO: implement catalog_.revoke()
            return {true, "", {}, {}, 0};
        }
        // Grant each privilege
        for (auto &priv_name : gs.privileges) {
            catalog::Privilege p;
            p.grantee = gs.grantee;
            p.privilege = priv_name;
            p.object_kind = gs.object_type;
            p.object_name = gs.object_name;
            p.with_grant_option = gs.with_grant_option;
            catalog_.grant(p);
        }
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_prepare(const sql::ast::PrepareStmt &ps) {
        catalog::PreparedStmtInfo psi;
        psi.name = ps.name;
        psi.sql_text = ps.sql_text;
        // Pre-parse the SQL
        try {
            sql::Parser parser(ps.sql_text);
            psi.parsed = parser.parse();
        } catch (const std::exception &e) {
            return {false, "PREPARE parse error: " + std::string(e.what()), {}, {}, 0};
        }
        catalog_.add_prepared(ps.name, std::move(psi));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_execute(const sql::ast::ExecuteStmt &es) {
        auto *psi = catalog_.find_prepared(es.name);
        if (!psi) return {false, "Prepared statement '" + es.name + "' not found", {}, {}, 0};
        if (psi->parsed) return execute_stmt(*psi->parsed);
        // Fall back to re-parsing
        return execute(psi->sql_text);
    }

    sql::ResultSet exec_deallocate(const sql::ast::DeallocateStmt &ds) {
        if (ds.all) {
            // Drop all prepared statements — would need catalog_.clear_prepared()
            return {true, "", {}, {}, 0};
        }
        catalog_.drop_prepared(ds.name);
        return {true, "", {}, {}, 0};
    }

    // ─── Cursor state ───
    struct CursorState {
        std::string name;
        sql::ResultSet result;
        int64_t position = -1; // -1 = before first row
        bool open = false;
        bool scroll = false;
        bool hold = false;
    };
    std::unordered_map<std::string, CursorState> cursors_;

    sql::ResultSet exec_declare_cursor(const sql::ast::DeclareCursorStmt &dc) {
        if (cursors_.count(dc.name))
            return {false, "Cursor '" + dc.name + "' already declared", {}, {}, 0};
        CursorState cs;
        cs.name = dc.name;
        cs.scroll = dc.scroll;
        cs.hold = dc.hold;
        // Pre-execute the query to get the result set
        if (dc.query) cs.result = exec_select(*dc.query);
        cursors_[dc.name] = std::move(cs);
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_open_cursor(const sql::ast::OpenCursorStmt &oc) {
        auto it = cursors_.find(oc.name);
        if (it == cursors_.end())
            return {false, "Cursor '" + oc.name + "' not found", {}, {}, 0};
        it->second.open = true;
        it->second.position = -1;
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_close_cursor(const sql::ast::CloseCursorStmt &cc) {
        auto it = cursors_.find(cc.name);
        if (it == cursors_.end())
            return {false, "Cursor '" + cc.name + "' not found", {}, {}, 0};
        it->second.open = false;
        cursors_.erase(it);
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_fetch_cursor(const sql::ast::FetchCursorStmt &fc) {
        auto it = cursors_.find(fc.name);
        if (it == cursors_.end())
            return {false, "Cursor '" + fc.name + "' not found", {}, {}, 0};
        auto &cs = it->second;
        if (!cs.open)
            return {false, "Cursor '" + fc.name + "' is not open", {}, {}, 0};

        sql::ResultSet result;
        result.success = true;
        result.columns = cs.result.columns;

        int64_t total = static_cast<int64_t>(cs.result.rows.size());
        int64_t new_pos = cs.position;

        switch (fc.direction) {
        case sql::ast::FetchCursorStmt::Direction::NEXT:     new_pos++; break;
        case sql::ast::FetchCursorStmt::Direction::PRIOR:    new_pos--; break;
        case sql::ast::FetchCursorStmt::Direction::FIRST:    new_pos = 0; break;
        case sql::ast::FetchCursorStmt::Direction::LAST:     new_pos = total - 1; break;
        case sql::ast::FetchCursorStmt::Direction::FORWARD:  new_pos += fc.count; break;
        case sql::ast::FetchCursorStmt::Direction::BACKWARD: new_pos -= fc.count; break;
        case sql::ast::FetchCursorStmt::Direction::ABSOLUTE: new_pos = fc.count - 1; break;
        case sql::ast::FetchCursorStmt::Direction::RELATIVE: new_pos += fc.count; break;
        }

        if (new_pos >= 0 && new_pos < total) {
            result.rows.push_back(cs.result.rows[static_cast<size_t>(new_pos)]);
            cs.position = new_pos;
        }
        return result;
    }

    sql::ResultSet exec_graph_match(const sql::ast::GraphMatchStmt &gm) {
        sql::ResultSet result;
        result.success = true;

        if (gm.nodes.empty()) return {false, "MATCH requires at least one node", {}, {}, 0};

        // Build a combined schema and row set by joining node tables via edges.
        // Start with the first node table.
        sql::Schema combined_schema;
        std::vector<sql::Tuple> combined_rows;

        // Load first node
        auto *first_table = catalog_.find_table(gm.nodes[0].label);
        if (!first_table)
            return {false, "Table '" + gm.nodes[0].label + "' not found for node '" + gm.nodes[0].alias + "'", {}, {}, 0};
        combined_schema = catalog_.get_table_schema(gm.nodes[0].label);
        for (auto &c : combined_schema) c.table = gm.nodes[0].alias;
        combined_rows = first_table->is_partitioned() ? first_table->all_rows() : first_table->rows;

        // For each edge, join with the next node table
        for (auto &edge : gm.edges) {
            if (edge.to_node < 0 || edge.to_node >= (int)gm.nodes.size()) continue;
            auto &target_node = gm.nodes[edge.to_node];
            auto *target_table = catalog_.find_table(target_node.label);
            if (!target_table)
                return {false, "Table '" + target_node.label + "' not found for node '" + target_node.alias + "'", {}, {}, 0};
            auto target_schema = catalog_.get_table_schema(target_node.label);
            for (auto &c : target_schema) c.table = target_node.alias;
            auto target_rows = target_table->is_partitioned() ? target_table->all_rows() : target_table->rows;

            // Find FK relationship between source and target (or use edge label as join column)
            int src_join_col = -1, tgt_join_col = -1;

            // Strategy 1: Look for FK from source table referencing target
            auto &src_node = gm.nodes[edge.from_node];
            auto *src_table = catalog_.find_table(src_node.label);
            if (src_table) {
                for (auto &fk : src_table->foreign_keys) {
                    if (fk.ref_table == target_node.label && !fk.columns.empty()) {
                        src_join_col = find_col_index(combined_schema, fk.columns[0]);
                        tgt_join_col = find_col_index(target_schema, fk.ref_columns[0]);
                        break;
                    }
                }
            }
            // Strategy 2: Look for FK from target table referencing source
            if (src_join_col < 0) {
                for (auto &fk : target_table->foreign_keys) {
                    if (fk.ref_table == src_node.label && !fk.columns.empty()) {
                        tgt_join_col = find_col_index(target_schema, fk.columns[0]);
                        auto src_full_schema = catalog_.get_table_schema(src_node.label);
                        int local_col = -1;
                        for (size_t ci = 0; ci < src_full_schema.size(); ci++) {
                            if (src_full_schema[ci].name == fk.ref_columns[0]) { local_col = (int)ci; break; }
                        }
                        if (local_col >= 0) {
                            // Find this column in combined_schema by table alias
                            for (size_t ci = 0; ci < combined_schema.size(); ci++) {
                                if (combined_schema[ci].table == src_node.alias &&
                                    combined_schema[ci].name == fk.ref_columns[0]) {
                                    src_join_col = (int)ci; break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
            // Strategy 3: Use edge label as column name
            if (src_join_col < 0 && !edge.label.empty()) {
                src_join_col = find_col_index(combined_schema, edge.label);
                // Assume target PK is the join target
                if (!target_table->primary_key_cols.empty())
                    tgt_join_col = find_col_index(target_schema, target_table->primary_key_cols[0]);
            }

            // Perform nested-loop join
            sql::Schema new_schema = combined_schema;
            new_schema.insert(new_schema.end(), target_schema.begin(), target_schema.end());
            std::vector<sql::Tuple> new_rows;

            if (src_join_col >= 0 && tgt_join_col >= 0) {
                for (auto &left : combined_rows) {
                    for (auto &right : target_rows) {
                        if (src_join_col < (int)left.size() && tgt_join_col < (int)right.size() &&
                            left[src_join_col].to_string() == right[tgt_join_col].to_string()) {
                            sql::Tuple combined = left;
                            combined.insert(combined.end(), right.begin(), right.end());
                            new_rows.push_back(std::move(combined));
                        }
                    }
                }
            } else {
                // No join condition found — cross join
                for (auto &left : combined_rows) {
                    for (auto &right : target_rows) {
                        sql::Tuple combined = left;
                        combined.insert(combined.end(), right.begin(), right.end());
                        new_rows.push_back(std::move(combined));
                    }
                }
            }

            combined_schema = new_schema;
            combined_rows = std::move(new_rows);
        }

        // Apply WHERE filter
        if (gm.where_clause) {
            std::vector<sql::Tuple> filtered;
            for (auto &row : combined_rows) {
                if (eval_predicate(*gm.where_clause, row, combined_schema))
                    filtered.push_back(std::move(row));
            }
            combined_rows = std::move(filtered);
        }

        // RETURN clause
        if (!gm.return_list.empty()) {
            for (size_t i = 0; i < gm.return_list.size(); i++) {
                std::string name = i < gm.return_aliases.size() ? gm.return_aliases[i]
                                    : get_expr_name(*gm.return_list[i]);
                result.columns.push_back({name, ""});
            }
            for (auto &row : combined_rows) {
                sql::Tuple out;
                for (auto &expr : gm.return_list)
                    out.push_back(eval_expr_with_row(*expr, row, combined_schema));
                result.rows.push_back(std::move(out));
            }
        } else {
            // Return all columns
            result.columns = combined_schema;
            result.rows = std::move(combined_rows);
        }

        // ORDER BY
        if (!gm.order_by.empty()) {
            sort_results(result.rows, gm.order_by, result.columns);
        }

        // LIMIT
        if (gm.limit) {
            int64_t lim = eval_expr(*gm.limit, {}, {}).int_val;
            if (lim >= 0 && (size_t)lim < result.rows.size())
                result.rows.resize((size_t)lim);
        }

        return result;
    }

    // ─── Trigger firing helper ───
    // Fires all matching triggers for a table/event/timing combination.
    // For BEFORE INSERT, the row can be mutated by the trigger.
    void fire_triggers(const std::string &table_name,
                       catalog::TriggerEvent event,
                       catalog::TriggerTiming timing,
                       sql::Tuple &/*new_row*/,
                       const sql::Tuple &/*old_row*/,
                       const sql::Schema &/*schema*/) {
        auto triggers = catalog_.triggers_for(table_name, event, timing);
        if (triggers.empty()) return;
        for (auto *ti : triggers) {
            auto *si = catalog_.find_script(ti->script_name);
            if (!si) continue;
            // Try to execute the trigger body as SQL
            if (!si->lua_source.empty()) {
                // Simple PL/SQL: just execute the body as SQL
                // For Lua triggers, would need ScriptEngine integration
                execute(si->lua_source);
            }
        }
    }

    // ===============================================
    // Transaction Control
    // ===============================================

    sql::ResultSet exec_begin(const sql::ast::BeginStmt &bs) {
        if (in_transaction_)
            return {false, "Transaction already in progress", {}, {}, 0};
        in_transaction_ = true;
        txn_id_ = next_txn_id_++;
        savepoints_.clear();
        (void)bs; // isolation level stored but not enforced yet
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_commit() {
        if (!in_transaction_)
            return {true, "", {}, {}, 0}; // auto-commit: no-op
        in_transaction_ = false;
        txn_id_ = 0;
        savepoints_.clear();
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_rollback(const sql::ast::Statement &stmt) {
        // Check for ROLLBACK TO SAVEPOINT
        if (stmt.type == sql::ast::StmtType::ROLLBACK_TXN) {
            // Check if it's a ROLLBACK TO SAVEPOINT (stored as SavepointStmt variant)
            if (auto *sp = std::get_if<sql::ast::SavepointStmt>(&stmt.data)) {
                return rollback_to_savepoint(sp->name);
            }
        }

        if (!in_transaction_)
            return {true, "", {}, {}, 0};

        // Restore from the oldest savepoint if any, otherwise just end txn
        // In a full implementation, we'd undo all changes since BEGIN
        in_transaction_ = false;
        txn_id_ = 0;
        savepoints_.clear();
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_savepoint(const sql::ast::SavepointStmt &sp) {
        if (!in_transaction_)
            return {false, "No active transaction for SAVEPOINT", {}, {}, 0};

        SavepointData spd;
        spd.name = sp.name;
        // Snapshot all table rows
        for (auto &tname : catalog_.list_tables()) {
            auto *table = catalog_.find_table(tname);
            if (table) spd.table_snapshots[tname] = table->rows;
        }
        savepoints_.push_back(std::move(spd));
        return {true, "", {}, {}, 0};
    }

    sql::ResultSet exec_release_savepoint(const sql::ast::SavepointStmt &sp) {
        for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
            if (it->name == sp.name) {
                savepoints_.erase(std::next(it).base());
                return {true, "", {}, {}, 0};
            }
        }
        return {false, "Savepoint '" + sp.name + "' not found", {}, {}, 0};
    }

    sql::ResultSet rollback_to_savepoint(const std::string &name) {
        // Find the savepoint and restore
        for (auto it = savepoints_.rbegin(); it != savepoints_.rend(); ++it) {
            if (it->name == name) {
                // Restore table rows
                for (auto &[tname, rows] : it->table_snapshots) {
                    auto *table = catalog_.find_table(tname);
                    if (table) table->rows = rows;
                }
                // Remove this savepoint and all newer ones
                savepoints_.erase(std::next(it).base(), savepoints_.end());
                return {true, "", {}, {}, 0};
            }
        }
        return {false, "Savepoint '" + name + "' not found", {}, {}, 0};
    }

    // ===============================================
    // INSERT
    // ===============================================

    // ─── Privilege enforcement ───
    std::string current_user_ = ""; // empty = superuser (no checks)

    bool check_privilege(const std::string &table_name, const std::string &priv) {
        if (current_user_.empty()) return true; // superuser
        auto &privs = catalog_.all_privileges();
        for (auto &p : privs) {
            if (p.grantee == current_user_ &&
                (p.object_name == table_name || p.object_name == "*") &&
                (p.privilege == priv || p.privilege == "ALL"))
                return true;
        }
        return false;
    }

    sql::ResultSet exec_insert(const sql::ast::InsertStmt &is) {
        if (!check_privilege(is.table, "INSERT"))
            return {false, "Permission denied: INSERT on '" + is.table + "' for user '" + current_user_ + "'", {}, {}, 0};
        auto *table = catalog_.find_table(is.table);
        if (!table) return {false, "Table '" + is.table + "' not found", {}, {}, 0};
        auto schema = catalog_.get_table_schema(is.table);

        // Build column index mapping
        std::vector<int> col_map; // col_map[value_idx] = table_col_idx
        if (!is.columns.empty()) {
            for (auto &cname : is.columns) {
                int ci = find_col_index(schema, cname);
                if (ci < 0) return {false, "Column '" + cname + "' not found in table '" + is.table + "'", {}, {}, 0};
                col_map.push_back(ci);
            }
        }

        // INSERT ... SELECT (atomic: buffer all rows, commit only on success)
        if (is.select) {
            auto sel_result = exec_select(*is.select);
            if (!sel_result.success) return sel_result;
            std::vector<sql::Tuple> staged;
            staged.reserve(sel_result.rows.size());
            int64_t auto_inc_saved = table->auto_increment_counter;
            for (auto &sel_row : sel_result.rows) {
                sql::Tuple row = build_insert_row(table, schema, sel_row, col_map);
                auto err = check_constraints(table, schema, row);
                if (!err.empty()) {
                    table->auto_increment_counter = auto_inc_saved;
                    return {false, err, {}, {}, 0};
                }
                staged.push_back(std::move(row));
            }
            for (auto &r : staged) insert_row_into_table(table, std::move(r));
            return {true, "", {}, {}, (int64_t)staged.size()};
        }

        // INSERT ... VALUES (atomic)
        std::vector<sql::Tuple> staged;
        staged.reserve(is.values.size());
        int64_t auto_inc_saved = table->auto_increment_counter;
        for (auto &val_row : is.values) {
            sql::Tuple vals;
            for (auto &expr : val_row) vals.push_back(eval_expr(*expr, {}, {}));

            sql::Tuple row = build_insert_row(table, schema, vals, col_map);
            auto err = check_constraints(table, schema, row);
            if (!err.empty()) {
                // ON CONFLICT handling
                if (!is.on_conflict.empty()) {
                    if (is.on_conflict == "DO NOTHING") {
                        continue; // skip this row
                    } else if (is.on_conflict == "DO UPDATE") {
                        // Find the conflicting row and update it
                        auto &all_rows = table->rows;
                        for (auto &existing : all_rows) {
                            // Check if PK matches
                            bool pk_match = true;
                            for (auto &pk_col : table->primary_key_cols) {
                                int pci = find_col_index(schema, pk_col);
                                if (pci >= 0 && pci < (int)row.size() && pci < (int)existing.size()) {
                                    if (row[pci].to_string() != existing[pci].to_string()) { pk_match = false; break; }
                                } else { pk_match = false; break; }
                            }
                            if (pk_match) {
                                for (auto &[col, expr] : is.on_conflict_updates) {
                                    int ci = find_col_index(schema, col);
                                    if (ci >= 0 && ci < (int)existing.size())
                                        existing[ci] = eval_expr_with_row(*expr, row, schema);
                                }
                                break;
                            }
                        }
                        continue; // don't insert, we updated
                    }
                }
                table->auto_increment_counter = auto_inc_saved;
                return {false, err, {}, {}, 0};
            }
            // BEFORE INSERT triggers
            sql::Tuple empty;
            fire_triggers(is.table, catalog::TriggerEvent::INSERT, catalog::TriggerTiming::BEFORE, row, empty, schema);
            staged.push_back(std::move(row));
        }
        for (auto &r : staged) {
            insert_row_into_table(table, sql::Tuple(r)); // copy for trigger
            // AFTER INSERT triggers
            sql::Tuple empty;
            fire_triggers(is.table, catalog::TriggerEvent::INSERT, catalog::TriggerTiming::AFTER, r, empty, schema);
        }
        // RETURNING clause
        if (!is.returning.empty()) {
            sql::ResultSet ret_result;
            ret_result.success = true;
            for (auto &expr : is.returning) {
                ret_result.columns.push_back({expr->alias.empty() ? get_expr_name(*expr) : expr->alias, ""});
            }
            for (auto &r : staged) {
                sql::Tuple out;
                for (auto &expr : is.returning) out.push_back(eval_expr_with_row(*expr, r, schema));
                ret_result.rows.push_back(std::move(out));
            }
            ret_result.rows_affected = static_cast<int64_t>(staged.size());
            return ret_result;
        }
        return {true, "", {}, {}, (int64_t)staged.size()};
    }

    sql::Tuple build_insert_row(catalog::TableInfo *table, const sql::Schema &schema,
                                 const sql::Tuple &vals, const std::vector<int> &col_map) {
        sql::Tuple row(table->columns.size(), sql::Value::make_null());

        // Apply defaults first
        for (size_t i = 0; i < table->columns.size(); i++) {
            if (table->columns[i].generated) continue;
            // Default values would be applied here if stored
        }

        // Map values to columns
        if (col_map.empty()) {
            for (size_t i = 0; i < vals.size() && i < row.size(); i++) {
                if (!table->columns[i].generated) row[i] = vals[i];
            }
        } else {
            for (size_t i = 0; i < vals.size() && i < col_map.size(); i++) {
                row[col_map[i]] = vals[i];
            }
        }

        // Compute generated columns
        for (size_t i = 0; i < table->columns.size(); i++) {
            if (table->columns[i].generated && table->columns[i].generated_expr) {
                row[i] = eval_expr_with_row(*table->columns[i].generated_expr, row, schema);
            }
        }
        return row;
    }

    std::string check_constraints(catalog::TableInfo *table, const sql::Schema &schema,
                                   sql::Tuple &row, int skip_row_idx = -1) {
        // Apply defaults and auto-increment (INSERT path; UPDATE path passes skip_row_idx)
        if (skip_row_idx < 0) {
            for (size_t i = 0; i < table->columns.size() && i < row.size(); i++) {
                auto &col = table->columns[i];
                if (row[i].is_null() && !col.generated) {
                    if (col.auto_increment) {
                        table->auto_increment_counter++;
                        row[i] = sql::Value::make_int(table->auto_increment_counter);
                    } else if (col.default_value) {
                        row[i] = eval_expr(*col.default_value, row, schema);
                    }
                }
            }
        }

        for (size_t i = 0; i < table->columns.size() && i < row.size(); i++) {
            auto &col = table->columns[i];
            // NOT NULL
            if (!col.nullable && row[i].is_null())
                return "NOT NULL constraint violated for column '" + col.name + "'";
            // CHECK constraint
            if (col.check_expr) {
                auto v = eval_expr_with_row(*col.check_expr, row, schema);
                if (v.type == sql::Value::Type::BOOL && !v.bool_val)
                    return "CHECK constraint violated for column '" + col.name + "'";
            }
            // DOMAIN CHECK enforcement. If the column's declared type name
            // matches a registered DOMAIN with a CHECK expression, the
            // constraint must pass for any non-NULL value (SQL standard:
            // NULLs pass DOMAIN CHECKs unconditionally — column NOT NULL is
            // the separate gate).
            if (!row[i].is_null()) {
                auto *dom = catalog_.find_domain_type(col.type.name);
                if (dom && dom->check_expr) {
                    sql::Schema dom_schema = { sql::Column{"VALUE", ""} };
                    sql::Tuple dom_row = { row[i] };
                    auto v = eval_expr_with_row(*dom->check_expr, dom_row, dom_schema);
                    if (v.type == sql::Value::Type::BOOL && !v.bool_val)
                        return "DOMAIN '" + col.type.name +
                               "' CHECK violated for column '" + col.name + "'";
                }
            }
        }

        // Collect all rows for constraint checking (includes partition rows)
        auto all_existing = table->is_partitioned() ? table->all_rows() : table->rows;

        // PRIMARY KEY uniqueness — skip the row at skip_row_idx (UPDATE path).
        if (!table->primary_key_cols.empty()) {
            for (size_t ri = 0; ri < all_existing.size(); ++ri) {
                if ((int)ri == skip_row_idx) continue;
                auto &existing = all_existing[ri];
                bool match = true;
                for (auto &pk_col : table->primary_key_cols) {
                    int ci = find_col_index(schema, pk_col);
                    if (ci < 0 || ci >= (int)row.size() || ci >= (int)existing.size()) { match = false; break; }
                    if (row[ci].is_null() || existing[ci].is_null()) { match = false; break; }
                    if (row[ci].to_string() != existing[ci].to_string()) { match = false; break; }
                }
                if (match)
                    return "PRIMARY KEY constraint violated: duplicate key";
            }
        }

        // UNIQUE constraints — skip the row at skip_row_idx (UPDATE path).
        for (size_t i = 0; i < table->columns.size() && i < row.size(); i++) {
            if (!table->columns[i].unique || table->columns[i].primary_key) continue;
            if (row[i].is_null()) continue; // NULL is allowed in UNIQUE
            for (size_t ri = 0; ri < all_existing.size(); ++ri) {
                if ((int)ri == skip_row_idx) continue;
                auto &existing = all_existing[ri];
                if (i < existing.size() && !existing[i].is_null() &&
                    row[i].to_string() == existing[i].to_string())
                    return "UNIQUE constraint violated for column '" + table->columns[i].name + "'";
            }
        }

        // FOREIGN KEY validation
        for (auto &fk : table->foreign_keys) {
            auto *ref_table = catalog_.find_table(fk.ref_table);
            if (!ref_table) continue;
            auto ref_schema = catalog_.get_table_schema(fk.ref_table);
            // Check that referenced row exists (search all rows including partitions)
            bool found = false;
            auto ref_all_rows = ref_table->is_partitioned() ? ref_table->all_rows() : ref_table->rows;
            for (auto &ref_row : ref_all_rows) {
                bool match = true;
                for (size_t j = 0; j < fk.columns.size() && j < fk.ref_columns.size(); j++) {
                    int src_ci = find_col_index(schema, fk.columns[j]);
                    int ref_ci = find_col_index(ref_schema, fk.ref_columns[j]);
                    if (src_ci < 0 || ref_ci < 0) { match = false; break; }
                    if (row[src_ci].is_null()) { match = false; break; } // NULL FK is OK
                    if (src_ci < (int)row.size() && ref_ci < (int)ref_row.size() &&
                        row[src_ci].to_string() == ref_row[ref_ci].to_string()) continue;
                    match = false; break;
                }
                if (match) { found = true; break; }
            }
            // Allow NULL foreign keys
            bool all_null = true;
            for (auto &fk_col : fk.columns) {
                int ci = find_col_index(schema, fk_col);
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) { all_null = false; break; }
            }
            if (!found && !all_null)
                return "FOREIGN KEY constraint violated: referenced row not found in '" + fk.ref_table + "'";
        }

        return "";
    }

    // ─── Partition routing ───

    // Find the target partition index for a row. Returns -1 if no partition matches.
    int partition_route(const catalog::TablePartitionSpec &spec, const sql::Tuple &row) const {
        // Extract partition key values
        std::vector<const sql::Value *> key_vals;
        for (int ci : spec.column_indices) {
            if (ci >= 0 && ci < (int)row.size())
                key_vals.push_back(&row[ci]);
            else
                return -1;
        }

        switch (spec.type) {
            case sql::ast::PartitionType::RANGE: {
                // RANGE: find first partition whose upper bound > key
                for (size_t i = 0; i < spec.partitions.size(); i++) {
                    auto &p = spec.partitions[i];
                    if (p.bound.is_maxvalue || p.bound.is_default) return (int)i;
                    if (p.bound.values.empty()) continue;
                    // Compare first key column to first bound value
                    int cmp = key_vals[0]->compare(p.bound.values[0]);
                    if (cmp < 0) return (int)i;
                }
                return -1;
            }
            case sql::ast::PartitionType::LIST: {
                // LIST: find partition whose value list contains the key
                for (size_t i = 0; i < spec.partitions.size(); i++) {
                    auto &p = spec.partitions[i];
                    if (p.bound.is_default) return (int)i;
                    for (auto &bv : p.bound.values) {
                        if (key_vals[0]->compare(bv) == 0) return (int)i;
                    }
                }
                return -1;
            }
            case sql::ast::PartitionType::HASH: {
                // HASH: compute hash of key values, match modulus/remainder
                uint64_t hash = 0;
                for (auto *kv : key_vals) {
                    std::string s = kv->to_string();
                    // Simple hash: FNV-1a
                    uint64_t h = 14695981039346656037ULL;
                    for (char c : s) {
                        h ^= (uint8_t)c;
                        h *= 1099511628211ULL;
                    }
                    hash ^= h;
                }
                for (size_t i = 0; i < spec.partitions.size(); i++) {
                    auto &p = spec.partitions[i];
                    if (p.bound.modulus > 0 &&
                        (int)(hash % (uint64_t)p.bound.modulus) == p.bound.remainder_val)
                        return (int)i;
                }
                return -1;
            }
        }
        return -1;
    }

    // Insert a row, routing to the correct partition if the table is partitioned
    void insert_row_into_table(catalog::TableInfo *table, sql::Tuple row) {
        // Columnar storage: append each value to its column vector
        if (table->columnar) {
            if (table->column_data.size() < row.size())
                table->column_data.resize(row.size());
            for (size_t i = 0; i < row.size(); i++)
                table->column_data[i].data.push_back(std::move(row[i]));
            return;
        }
        if (!table->is_partitioned()) {
            table->rows.push_back(std::move(row));
            return;
        }
        int pidx = partition_route(table->partition_spec.value(), row);
        if (pidx >= 0 && pidx < (int)table->partition_spec->partitions.size()) {
            table->partition_spec->partitions[pidx].rows.push_back(std::move(row));
        }
    }

    // ─── Partition pruning ───
    // Returns rows from only the partitions that could match the WHERE clause.
    std::vector<sql::Tuple> pruned_partition_rows(
            const catalog::TableInfo *table,
            const sql::ast::Expr *where_clause,
            const sql::Schema &/*schema*/) {
        if (!table->is_partitioned() || !where_clause)
            return table->all_rows();

        auto &spec = table->partition_spec.value();
        if (spec.column_indices.empty()) return table->all_rows();

        // Try to extract a constant comparison against the partition key column.
        // We look for patterns like: partition_col = literal, partition_col < literal, etc.
        std::string part_col = spec.columns[0];

        // Extract a constant value and operator from the WHERE clause
        struct Constraint {
            enum Op { EQ, LT, LTE, GT, GTE, IN_LIST, NONE } op = NONE;
            sql::Value val;
            std::vector<sql::Value> in_vals;
        };

        std::function<Constraint(const sql::ast::Expr &)> extract = [&](const sql::ast::Expr &expr) -> Constraint {
            if (expr.type == sql::ast::ExprType::BINARY_OP) {
                auto &bin = std::get<sql::ast::BinaryOp>(expr.data);
                // For AND, try both sides and return the first match
                if (bin.op == sql::TokenType::KW_AND) {
                    if (bin.left) { auto c = extract(*bin.left); if (c.op != Constraint::NONE) return c; }
                    if (bin.right) { auto c = extract(*bin.right); if (c.op != Constraint::NONE) return c; }
                    return {};
                }
                // Check for partition_col OP literal
                bool left_is_col = false, right_is_col = false;
                if (bin.left && bin.left->type == sql::ast::ExprType::COLUMN_REF) {
                    auto &ref = std::get<sql::ast::ColumnRef>(bin.left->data);
                    std::string col = ref.column;
                    for (auto &ch : col) ch = (char)std::tolower((unsigned char)ch);
                    std::string pc = part_col;
                    for (auto &ch : pc) ch = (char)std::tolower((unsigned char)ch);
                    if (col == pc) left_is_col = true;
                }
                if (bin.right && bin.right->type == sql::ast::ExprType::COLUMN_REF) {
                    auto &ref = std::get<sql::ast::ColumnRef>(bin.right->data);
                    std::string col = ref.column;
                    for (auto &ch : col) ch = (char)std::tolower((unsigned char)ch);
                    std::string pc = part_col;
                    for (auto &ch : pc) ch = (char)std::tolower((unsigned char)ch);
                    if (col == pc) right_is_col = true;
                }
                auto eval_lit = [&](const sql::ast::Expr &e) -> sql::Value {
                    if (e.type == sql::ast::ExprType::LITERAL) {
                        return eval_expr(e, {}, {});
                    }
                    return sql::Value::make_null();
                };
                if (left_is_col && bin.right) {
                    sql::Value v = eval_lit(*bin.right);
                    if (!v.is_null()) {
                        Constraint c; c.val = v;
                        if (bin.op == sql::TokenType::EQ) c.op = Constraint::EQ;
                        else if (bin.op == sql::TokenType::LT) c.op = Constraint::LT;
                        else if (bin.op == sql::TokenType::LTE) c.op = Constraint::LTE;
                        else if (bin.op == sql::TokenType::GT) c.op = Constraint::GT;
                        else if (bin.op == sql::TokenType::GTE) c.op = Constraint::GTE;
                        return c;
                    }
                }
                if (right_is_col && bin.left) {
                    sql::Value v = eval_lit(*bin.left);
                    if (!v.is_null()) {
                        Constraint c; c.val = v;
                        // Flip operator since column is on right
                        if (bin.op == sql::TokenType::EQ) c.op = Constraint::EQ;
                        else if (bin.op == sql::TokenType::LT) c.op = Constraint::GT;
                        else if (bin.op == sql::TokenType::LTE) c.op = Constraint::GTE;
                        else if (bin.op == sql::TokenType::GT) c.op = Constraint::LT;
                        else if (bin.op == sql::TokenType::GTE) c.op = Constraint::LTE;
                        return c;
                    }
                }
            }
            if (expr.type == sql::ast::ExprType::IN_EXPR) {
                auto &in = std::get<sql::ast::InExpr>(expr.data);
                if (in.operand && in.operand->type == sql::ast::ExprType::COLUMN_REF && !in.negated) {
                    auto &ref = std::get<sql::ast::ColumnRef>(in.operand->data);
                    std::string col = ref.column;
                    for (auto &ch : col) ch = (char)std::tolower((unsigned char)ch);
                    std::string pc = part_col;
                    for (auto &ch : pc) ch = (char)std::tolower((unsigned char)ch);
                    if (col == pc) {
                        if (auto *vals = std::get_if<std::vector<sql::ast::ExprPtr>>(&in.values)) {
                            Constraint c; c.op = Constraint::IN_LIST;
                            for (auto &e : *vals) {
                                if (e) c.in_vals.push_back(eval_expr(*e, {}, {}));
                            }
                            return c;
                        }
                    }
                }
            }
            return {};
        };

        Constraint constraint = extract(*where_clause);
        if (constraint.op == Constraint::NONE) return table->all_rows();

        // Now determine which partitions to include
        std::vector<bool> include(spec.partitions.size(), false);

        for (size_t pi = 0; pi < spec.partitions.size(); pi++) {
            auto &p = spec.partitions[pi];

            if (spec.type == sql::ast::PartitionType::RANGE) {
                // RANGE: partition holds rows where prev_bound <= key < this_bound
                if (p.bound.is_maxvalue || p.bound.is_default) {
                    // Could contain anything >= previous partition's bound
                    if (constraint.op == Constraint::EQ || constraint.op == Constraint::GTE ||
                        constraint.op == Constraint::GT) {
                        include[pi] = true;
                    } else {
                        include[pi] = true; // conservative: include MAXVALUE partition
                    }
                    continue;
                }
                if (p.bound.values.empty()) { include[pi] = true; continue; }
                auto &upper = p.bound.values[0];
                // Get lower bound from previous partition (or -inf)
                bool has_lower = (pi > 0 && !spec.partitions[pi-1].bound.values.empty());
                auto *lower = has_lower ? &spec.partitions[pi-1].bound.values[0] : nullptr;

                switch (constraint.op) {
                    case Constraint::EQ: {
                        bool above_lower = !lower || constraint.val.compare(*lower) >= 0;
                        bool below_upper = constraint.val.compare(upper) < 0;
                        include[pi] = above_lower && below_upper;
                        break;
                    }
                    case Constraint::LT:
                    case Constraint::LTE: {
                        bool above_lower = !lower || constraint.val.compare(*lower) > 0;
                        include[pi] = above_lower;
                        break;
                    }
                    case Constraint::GT:
                    case Constraint::GTE: {
                        bool below_upper = constraint.val.compare(upper) < 0;
                        include[pi] = below_upper || p.bound.is_maxvalue;
                        break;
                    }
                    case Constraint::IN_LIST: {
                        for (auto &iv : constraint.in_vals) {
                            bool above_lower = !lower || iv.compare(*lower) >= 0;
                            bool below_upper = iv.compare(upper) < 0;
                            if (above_lower && below_upper) { include[pi] = true; break; }
                        }
                        break;
                    }
                    default: include[pi] = true; break;
                }
            } else if (spec.type == sql::ast::PartitionType::LIST) {
                if (p.bound.is_default) { include[pi] = true; continue; }
                switch (constraint.op) {
                    case Constraint::EQ:
                        for (auto &bv : p.bound.values) {
                            if (constraint.val.compare(bv) == 0) { include[pi] = true; break; }
                        }
                        break;
                    case Constraint::IN_LIST:
                        for (auto &iv : constraint.in_vals) {
                            for (auto &bv : p.bound.values) {
                                if (iv.compare(bv) == 0) { include[pi] = true; break; }
                            }
                            if (include[pi]) break;
                        }
                        break;
                    default:
                        include[pi] = true; // Can't prune with range ops on LIST
                        break;
                }
            } else {
                // HASH: can only prune on EQ
                if (constraint.op == Constraint::EQ && p.bound.modulus > 0) {
                    std::string s = constraint.val.to_string();
                    uint64_t h = 14695981039346656037ULL;
                    for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ULL; }
                    include[pi] = ((int)(h % (uint64_t)p.bound.modulus) == p.bound.remainder_val);
                } else {
                    include[pi] = true;
                }
            }
        }

        std::vector<sql::Tuple> result;
        for (size_t pi = 0; pi < spec.partitions.size(); pi++) {
            if (include[pi]) {
                auto &pr = spec.partitions[pi].rows;
                result.insert(result.end(), pr.begin(), pr.end());
            }
        }
        return result;
    }

    // ===============================================
    // SELECT -- Complete Implementation
    // ===============================================

    sql::ResultSet exec_select(const sql::ast::SelectStmt &sel) {
        // Privilege check for each table in FROM clause
        for (auto &tref : sel.from) {
            if (tref && tref->type == sql::ast::TableRefType::TABLE) {
                if (!check_privilege(tref->name, "SELECT"))
                    return {false, "Permission denied: SELECT on '" + tref->name + "' for user '" + current_user_ + "'", {}, {}, 0};
            }
        }
        sql::ResultSet result;
        result.success = true;

        // Handle CTEs (including WITH RECURSIVE)
        auto old_ctes = cte_tables_;
        if (!sel.ctes.empty()) {
            for (auto &cte : sel.ctes) {
                if (!cte.query) continue;
                if (cte.recursive && cte.query->set_op == sql::ast::SelectStmt::SetOp::UNION_ALL
                    && cte.query->set_right) {
                    // Recursive CTE: anchor UNION ALL recursive
                    // Execute anchor by temporarily clearing the set operation
                    auto saved_op = cte.query->set_op;
                    auto saved_right = std::move(cte.query->set_right);
                    cte.query->set_op = sql::ast::SelectStmt::SetOp::NONE;
                    auto anchor = exec_select(*cte.query);
                    cte.query->set_op = saved_op;
                    cte.query->set_right = std::move(saved_right);
                    if (!anchor.success) continue;

                    auto all_rows = anchor.rows;
                    auto prev_rows = anchor.rows;
                    int max_depth = 1000;
                    while (max_depth-- > 0 && !prev_rows.empty()) {
                        cte_tables_[cte.name] = {anchor.columns, prev_rows};
                        auto iter_result = exec_select(*cte.query->set_right);
                        if (!iter_result.success || iter_result.rows.empty()) break;
                        prev_rows = iter_result.rows;
                        for (auto &row : iter_result.rows)
                            all_rows.push_back(std::move(row));
                    }
                    cte_tables_[cte.name] = {anchor.columns, std::move(all_rows)};
                } else {
                    // Non-recursive CTE
                    auto cte_result = exec_select(*cte.query);
                    if (cte_result.success) {
                        cte_tables_[cte.name] = {cte_result.columns, std::move(cte_result.rows)};
                    }
                }
            }
        }

        // Handle set operations (UNION, INTERSECT, EXCEPT)
        if (sel.set_op != sql::ast::SelectStmt::SetOp::NONE && sel.set_right) {
            result = exec_select_set_op(sel);
            cte_tables_ = old_ctes;
            return result;
        }

        // No FROM clause
        if (sel.from.empty()) {
            sql::Tuple row;
            for (auto &expr : sel.select_list) row.push_back(eval_expr(*expr, {}, {}));
            result.rows.push_back(std::move(row));
            for (auto &expr : sel.select_list) {
                result.columns.push_back({expr->alias.empty() ? "?column?" : expr->alias, ""});
            }
            cte_tables_ = old_ctes;
            return result;
        }

        // Resolve FROM: get combined data + schema
        sql::Schema combined_schema;
        std::vector<sql::Tuple> combined_rows;
        if (!resolve_from(sel.from, combined_schema, combined_rows, sel.where_clause.get())) {
            cte_tables_ = old_ctes;
            return {false, "Table not found in FROM clause", {}, {}, 0};
        }

        // Apply WHERE filter. Spatial pushdown: when the WHERE clause has an
        // `ST_Intersects(col, literal)` or `ST_DWithin(col, literal, d)` form
        // we precompute the literal's bounding box once and use it as a fast
        // prefilter against each row's column-geometry bbox before invoking
        // the (potentially expensive) full predicate. This is the same
        // selectivity win an R-Tree index gives; the heavy lifting is one
        // bbox extraction per row, not per WHERE clause evaluation.
        std::vector<sql::Tuple> filtered;
        if (sel.where_clause) {
            // Detect a spatial pushdown opportunity. We only handle a single
            // top-level ST_* call; complex predicates fall back to the slow
            // path.
            auto pushdown = analyze_spatial_pushdown(*sel.where_clause, combined_schema);
            if (pushdown.eligible) {
                pushed_down_spatial_count_++;
                for (auto &row : combined_rows) {
                    // Fast bbox reject.
                    if (!bbox_overlap_row(row, combined_schema, pushdown)) continue;
                    if (eval_predicate(*sel.where_clause, row, combined_schema))
                        filtered.push_back(row);
                }
            } else {
                // Index-accelerated filter: check if WHERE has col = literal
                // on an indexed column. If so, build a hash index and prefilter.
                bool index_used = false;
                if (sel.where_clause->type == sql::ast::ExprType::BINARY_OP) {
                    auto &op = std::get<sql::ast::BinaryOp>(sel.where_clause->data);
                    if (op.op == sql::TokenType::EQ &&
                        op.left->type == sql::ast::ExprType::COLUMN_REF &&
                        op.right->type == sql::ast::ExprType::LITERAL) {
                        auto &col_ref = std::get<sql::ast::ColumnRef>(op.left->data);
                        int ci = find_col_index(combined_schema, col_ref.column);
                        if (ci >= 0) {
                            // Check if there's an index on this column
                            for (auto &idx_name : catalog_.list_indexes()) {
                                auto *idx = catalog_.find_index(idx_name);
                                if (idx && !idx->columns.empty() && idx->columns[0] == col_ref.column) {
                                    // Index found! Use hash-based prefilter.
                                    auto target = eval_expr(*op.right, {}, {});
                                    std::string target_str = target.to_string();
                                    for (auto &row : combined_rows) {
                                        if (ci < (int)row.size() && row[ci].to_string() == target_str) {
                                            if (eval_predicate(*sel.where_clause, row, combined_schema))
                                                filtered.push_back(row);
                                        }
                                    }
                                    index_used = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!index_used) {
                    for (auto &row : combined_rows) {
                        if (eval_predicate(*sel.where_clause, row, combined_schema))
                            filtered.push_back(row);
                    }
                }
            }
        } else {
            filtered = std::move(combined_rows);
        }

        // Check for aggregates in select list
        bool has_agg = !sel.group_by.empty() || has_aggregate(sel.select_list);

        if (has_agg) {
            result = exec_aggregate(sel, filtered, combined_schema);
        } else {
            // Determine output
            bool select_star = is_select_star(sel.select_list);

            // Build output schema
            if (select_star) {
                result.columns = combined_schema;
            } else {
                for (auto &expr : sel.select_list) {
                    std::string name = get_expr_name(*expr);
                    result.columns.push_back({name, ""});
                }
            }

            // Project rows
            for (auto &row : filtered) {
                if (select_star) {
                    result.rows.push_back(row);
                } else {
                    sql::Tuple out;
                    for (auto &expr : sel.select_list)
                        out.push_back(eval_expr_with_row(*expr, row, combined_schema));
                    result.rows.push_back(std::move(out));
                }
            }
        }

        // DISTINCT
        if (sel.distinct) {
            std::set<std::string> seen;
            std::vector<sql::Tuple> unique;
            for (auto &row : result.rows) {
                std::string key;
                for (auto &v : row) key += v.to_string() + "|";
                if (seen.insert(key).second) unique.push_back(std::move(row));
            }
            result.rows = std::move(unique);
        }

        // WINDOW FUNCTIONS — post-process any WINDOW_CALL in select list
        {
            // Find which output columns contain window calls
            std::vector<std::pair<size_t, const sql::ast::WindowCall *>> win_cols;
            for (size_t ci = 0; ci < sel.select_list.size(); ci++) {
                if (sel.select_list[ci]->type == sql::ast::ExprType::WINDOW_CALL) {
                    win_cols.push_back({ci, &std::get<sql::ast::WindowCall>(sel.select_list[ci]->data)});
                }
            }
            if (!win_cols.empty()) {
                for (auto &[col_idx, wc] : win_cols) {
                    std::string fname = wc->name;
                    for (auto &c : fname) c = (char)std::toupper((unsigned char)c);

                    // Build partition groups: key -> list of row indices
                    std::vector<std::pair<std::string, std::vector<size_t>>> partitions;
                    std::unordered_map<std::string, size_t> pmap;
                    for (size_t ri = 0; ri < result.rows.size(); ri++) {
                        std::string pk;
                        for (auto &pe : wc->window.partition_by) {
                            pk += eval_expr_with_row(*pe, result.rows[ri], result.columns).to_string() + "|";
                        }
                        auto it = pmap.find(pk);
                        if (it == pmap.end()) {
                            pmap[pk] = partitions.size();
                            partitions.push_back({pk, {ri}});
                        } else {
                            partitions[it->second].second.push_back(ri);
                        }
                    }

                    // Sort within each partition by ORDER BY
                    for (auto &[pk, indices] : partitions) {
                        if (!wc->window.order_by.empty()) {
                            std::stable_sort(indices.begin(), indices.end(),
                                [&](size_t a, size_t b) {
                                    for (auto &[oe, is_asc] : wc->window.order_by) {
                                        auto va = eval_expr_with_row(*oe, result.rows[a], result.columns);
                                        auto vb = eval_expr_with_row(*oe, result.rows[b], result.columns);
                                        int cmp = va.compare(vb);
                                        if (cmp != 0) return is_asc ? cmp < 0 : cmp > 0;
                                    }
                                    return false;
                                });
                        }

                        size_t psize = indices.size();
                        for (size_t pos = 0; pos < psize; pos++) {
                            size_t ri = indices[pos];
                            sql::Value val = sql::Value::make_null();

                            if (fname == "ROW_NUMBER") {
                                val = sql::Value::make_int(static_cast<int64_t>(pos + 1));
                            } else if (fname == "RANK") {
                                if (pos == 0) {
                                    val = sql::Value::make_int(1);
                                } else {
                                    // Check if ORDER BY values match previous
                                    bool same = true;
                                    for (auto &[oe, is_asc] : wc->window.order_by) {
                                        auto va = eval_expr_with_row(*oe, result.rows[indices[pos]], result.columns);
                                        auto vb = eval_expr_with_row(*oe, result.rows[indices[pos-1]], result.columns);
                                        if (va.compare(vb) != 0) { same = false; break; }
                                    }
                                    val = same ? result.rows[indices[pos-1]][col_idx]
                                               : sql::Value::make_int(static_cast<int64_t>(pos + 1));
                                }
                            } else if (fname == "DENSE_RANK") {
                                if (pos == 0) {
                                    val = sql::Value::make_int(1);
                                } else {
                                    bool same = true;
                                    for (auto &[oe, is_asc] : wc->window.order_by) {
                                        auto va = eval_expr_with_row(*oe, result.rows[indices[pos]], result.columns);
                                        auto vb = eval_expr_with_row(*oe, result.rows[indices[pos-1]], result.columns);
                                        if (va.compare(vb) != 0) { same = false; break; }
                                    }
                                    int64_t prev = result.rows[indices[pos-1]][col_idx].int_val;
                                    val = same ? sql::Value::make_int(prev) : sql::Value::make_int(prev + 1);
                                }
                            } else if (fname == "NTILE") {
                                int64_t n = 1;
                                if (!wc->args.empty())
                                    n = eval_expr_with_row(*wc->args[0], result.rows[ri], result.columns).int_val;
                                if (n <= 0) n = 1;
                                val = sql::Value::make_int(static_cast<int64_t>(pos * static_cast<size_t>(n) / psize) + 1);
                            } else if (fname == "LAG" || fname == "LEAD") {
                                int64_t offset = 1;
                                if (wc->args.size() >= 2)
                                    offset = eval_expr_with_row(*wc->args[1], result.rows[ri], result.columns).int_val;
                                int64_t target_pos = fname == "LAG"
                                    ? static_cast<int64_t>(pos) - offset
                                    : static_cast<int64_t>(pos) + offset;
                                if (target_pos >= 0 && target_pos < static_cast<int64_t>(psize) && !wc->args.empty()) {
                                    val = eval_expr_with_row(*wc->args[0], result.rows[indices[static_cast<size_t>(target_pos)]], result.columns);
                                } else if (wc->args.size() >= 3) {
                                    val = eval_expr_with_row(*wc->args[2], result.rows[ri], result.columns);
                                }
                            } else if (fname == "PERCENT_RANK") {
                                double pr = psize <= 1 ? 0.0 : static_cast<double>(pos) / static_cast<double>(psize - 1);
                                val = sql::Value::make_float(pr);
                            } else if (fname == "CUME_DIST") {
                                // Count rows <= current in the ORDER BY
                                size_t le_count = pos + 1;
                                for (size_t k = pos + 1; k < psize; k++) {
                                    bool same = true;
                                    for (auto &[oe, is_asc] : wc->window.order_by) {
                                        auto va = eval_expr_with_row(*oe, result.rows[indices[pos]], result.columns);
                                        auto vb = eval_expr_with_row(*oe, result.rows[indices[k]], result.columns);
                                        if (va.compare(vb) != 0) { same = false; break; }
                                    }
                                    if (same) le_count++; else break;
                                }
                                val = sql::Value::make_float(static_cast<double>(le_count) / static_cast<double>(psize));
                            } else if (fname == "FIRST_VALUE" && !wc->args.empty()) {
                                val = eval_expr_with_row(*wc->args[0], result.rows[indices[0]], result.columns);
                            } else if (fname == "LAST_VALUE" && !wc->args.empty()) {
                                val = eval_expr_with_row(*wc->args[0], result.rows[indices[psize-1]], result.columns);
                            } else if (fname == "SUM" || fname == "COUNT" || fname == "AVG" ||
                                       fname == "MIN" || fname == "MAX") {
                                // Aggregate window functions — running aggregate up to current row
                                double sum = 0; int64_t cnt = 0;
                                sql::Value minv, maxv;
                                for (size_t k = 0; k <= pos; k++) {
                                    if (!wc->args.empty()) {
                                        auto av = eval_expr_with_row(*wc->args[0], result.rows[indices[k]], result.columns);
                                        if (!av.is_null()) {
                                            if (av.type == sql::Value::Type::INT64) sum += static_cast<double>(av.int_val);
                                            else if (av.type == sql::Value::Type::FLOAT64) sum += av.float_val;
                                            if (cnt == 0 || av.compare(minv) < 0) minv = av;
                                            if (cnt == 0 || av.compare(maxv) > 0) maxv = av;
                                            cnt++;
                                        }
                                    } else {
                                        cnt++;
                                    }
                                }
                                if (fname == "COUNT") val = sql::Value::make_int(cnt);
                                else if (fname == "SUM") val = sql::Value::make_float(sum);
                                else if (fname == "AVG") val = cnt > 0 ? sql::Value::make_float(sum / cnt) : sql::Value::make_null();
                                else if (fname == "MIN") val = cnt > 0 ? minv : sql::Value::make_null();
                                else if (fname == "MAX") val = cnt > 0 ? maxv : sql::Value::make_null();
                            }

                            result.rows[ri][col_idx] = val;
                        }
                    }
                }
            }
        }

        // ORDER BY
        if (!sel.order_by.empty()) {
            sort_results(result.rows, sel.order_by, result.columns);
        }

        // OFFSET
        if (sel.offset) {
            int64_t off = eval_expr(*sel.offset, {}, {}).int_val;
            if (off > 0) {
                if (off >= (int64_t)result.rows.size()) result.rows.clear();
                else result.rows.erase(result.rows.begin(), result.rows.begin() + off);
            }
        }

        // LIMIT
        if (sel.limit) {
            int64_t lim = eval_expr(*sel.limit, {}, {}).int_val;
            if (lim >= 0 && (size_t)lim < result.rows.size())
                result.rows.resize((size_t)lim);
        }

        cte_tables_ = old_ctes;
        return result;
    }

    // Resolve FROM clause: tables, views, matviews, CTEs, JOINs
    bool resolve_from(const std::vector<sql::ast::TableRefPtr> &refs,
                      sql::Schema &schema, std::vector<sql::Tuple> &rows,
                      const sql::ast::Expr *where_hint = nullptr) {
        for (size_t i = 0; i < refs.size(); i++) {
            sql::Schema ref_schema;
            std::vector<sql::Tuple> ref_rows;

            if (!resolve_table_ref(*refs[i], ref_schema, ref_rows, where_hint))
                return false;

            if (i == 0) {
                schema = ref_schema;
                rows = std::move(ref_rows);
            } else {
                // Cross join with accumulated results
                schema = cross_join_schemas(schema, ref_schema);
                rows = cross_join_rows(rows, ref_rows);
            }
        }
        return true;
    }

    bool resolve_table_ref(const sql::ast::TableRef &ref, sql::Schema &schema,
                           std::vector<sql::Tuple> &rows,
                           const sql::ast::Expr *where_hint = nullptr) {
        if (ref.type == sql::ast::TableRefType::TABLE) {
            std::string name = ref.name;
            std::string alias = ref.alias.empty() ? name : ref.alias;

            // Check INFORMATION_SCHEMA
            if (ref.schema.has_value()) {
                std::string schema_name = ref.schema.value();
                for (auto &c : schema_name) c = (char)std::tolower((unsigned char)c);
                if (schema_name == "information_schema") {
                    auto [is_schema, is_rows] = catalog_.get_information_schema(name);
                    if (!is_schema.empty()) {
                        schema = is_schema;
                        rows = std::move(is_rows);
                        return true;
                    }
                }
            }

            // Check CTE first
            auto cte_it = cte_tables_.find(name);
            if (cte_it != cte_tables_.end()) {
                schema = cte_it->second.first;
                rows = cte_it->second.second;
                return true;
            }

            // Check table
            auto *table = catalog_.find_table(name);
            if (table) {
                schema = catalog_.get_table_schema(name);
                // Alias the schema
                for (auto &col : schema) col.table = alias;
                if (table->columnar) {
                    rows = table->materialize_rows();
                } else if (table->is_partitioned() && where_hint) {
                    rows = pruned_partition_rows(table, where_hint, schema);
                } else {
                    rows = table->is_partitioned() ? table->all_rows() : table->rows;
                }
                return true;
            }

            // Check materialized view
            auto *mv = catalog_.find_materialized_view(name);
            if (mv) {
                schema = catalog_.get_matview_schema(name);
                for (auto &col : schema) col.table = alias;
                rows = mv->rows;
                return true;
            }

            // Check view — re-execute the view's stored query
            auto *view = catalog_.find_view(name);
            if (view && view->query) {
                auto view_result = exec_select(*view->query);
                if (!view_result.success) return false;
                schema = view_result.columns;
                for (auto &col : schema) col.table = alias;
                rows = std::move(view_result.rows);
                return true;
            }

            return false;
        }

        if (ref.type == sql::ast::TableRefType::SUBQUERY && ref.subquery) {
            auto sub_result = exec_select(*ref.subquery);
            if (!sub_result.success) return false;
            schema = sub_result.columns;
            rows = std::move(sub_result.rows);
            return true;
        }

        if (ref.type == sql::ast::TableRefType::JOIN) {
            // Resolve left side (stored in name/schema fields of the join ref)
            sql::Schema left_schema, right_schema;
            std::vector<sql::Tuple> left_rows, right_rows;

            // The left table info is in the ref itself
            auto *ltable = catalog_.find_table(ref.name);
            if (ltable) {
                left_schema = catalog_.get_table_schema(ref.name);
                std::string lalias = ref.alias.empty() ? ref.name : ref.alias;
                for (auto &c : left_schema) c.table = lalias;
                left_rows = ltable->is_partitioned() ? ltable->all_rows() : ltable->rows;
            }

            // Right side is in ref.join.right
            if (ref.join.right) {
                if (!resolve_table_ref(*ref.join.right, right_schema, right_rows))
                    return false;
            }

            schema = cross_join_schemas(left_schema, right_schema);

            // Apply join condition
            auto jtype = ref.join.type;
            if (jtype == sql::ast::JoinType::CROSS) {
                rows = cross_join_rows(left_rows, right_rows);
            } else {
                // NLJ with ON condition
                for (auto &l : left_rows) {
                    bool matched = false;
                    for (auto &r : right_rows) {
                        sql::Tuple combined = l;
                        combined.insert(combined.end(), r.begin(), r.end());
                        bool pass = true;
                        if (ref.join.on_condition) {
                            pass = eval_predicate(*ref.join.on_condition, combined, schema);
                        }
                        if (pass) { rows.push_back(combined); matched = true; }
                    }
                    // LEFT JOIN: emit NULLs for unmatched
                    if (!matched && (jtype == sql::ast::JoinType::LEFT || jtype == sql::ast::JoinType::FULL)) {
                        sql::Tuple combined = l;
                        for (size_t k = 0; k < right_schema.size(); k++)
                            combined.push_back(sql::Value::make_null());
                        rows.push_back(combined);
                    }
                }
                // RIGHT/FULL JOIN: check for unmatched right rows
                if (jtype == sql::ast::JoinType::RIGHT || jtype == sql::ast::JoinType::FULL) {
                    for (auto &r : right_rows) {
                        bool matched = false;
                        for (auto &l : left_rows) {
                            sql::Tuple combined = l;
                            combined.insert(combined.end(), r.begin(), r.end());
                            if (ref.join.on_condition && eval_predicate(*ref.join.on_condition, combined, schema)) {
                                matched = true; break;
                            }
                        }
                        if (!matched) {
                            sql::Tuple combined;
                            for (size_t k = 0; k < left_schema.size(); k++)
                                combined.push_back(sql::Value::make_null());
                            combined.insert(combined.end(), r.begin(), r.end());
                            rows.push_back(combined);
                        }
                    }
                }
            }
            return true;
        }

        return false;
    }

    sql::Schema cross_join_schemas(const sql::Schema &a, const sql::Schema &b) {
        sql::Schema out = a;
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }

    std::vector<sql::Tuple> cross_join_rows(const std::vector<sql::Tuple> &a,
                                             const std::vector<sql::Tuple> &b) {
        std::vector<sql::Tuple> out;
        for (auto &l : a) for (auto &r : b) {
            sql::Tuple combined = l;
            combined.insert(combined.end(), r.begin(), r.end());
            out.push_back(std::move(combined));
        }
        return out;
    }

    // Set operations -- execute the body portion of the left SELECT, then combine with right
    sql::ResultSet exec_select_set_op(const sql::ast::SelectStmt &sel) {
        // Execute right side
        if (!sel.set_right) return {false, "Missing right side of set operation", {}, {}, 0};
        auto right = exec_select(*sel.set_right);
        if (!right.success) return right;

        // Execute left body: we build a temporary SelectStmt without the set_op
        // Since we can't copy unique_ptrs, we execute the left body inline
        sql::ResultSet left;
        left.success = true;
        if (sel.from.empty()) {
            sql::Tuple row;
            for (auto &expr : sel.select_list) row.push_back(eval_expr(*expr, {}, {}));
            left.rows.push_back(std::move(row));
            for (auto &expr : sel.select_list)
                left.columns.push_back({expr->alias.empty() ? "?column?" : expr->alias, ""});
        } else {
            sql::Schema combined_schema;
            std::vector<sql::Tuple> combined_rows;
            if (!resolve_from(sel.from, combined_schema, combined_rows, sel.where_clause.get()))
                return {false, "Table not found in FROM clause", {}, {}, 0};
            if (sel.where_clause) {
                for (auto &row : combined_rows) {
                    if (eval_predicate(*sel.where_clause, row, combined_schema))
                        left.rows.push_back(row);
                }
            } else {
                left.rows = std::move(combined_rows);
            }
            bool star = is_select_star(sel.select_list);
            if (star) { left.columns = combined_schema; }
            else {
                for (auto &expr : sel.select_list) left.columns.push_back({get_expr_name(*expr), ""});
                std::vector<sql::Tuple> projected;
                for (auto &row : left.rows) {
                    sql::Tuple out;
                    for (auto &expr : sel.select_list) out.push_back(eval_expr_with_row(*expr, row, combined_schema));
                    projected.push_back(std::move(out));
                }
                left.rows = std::move(projected);
            }
        }

        sql::ResultSet result;
        result.success = true;
        result.columns = left.columns;

        using SO = sql::ast::SelectStmt::SetOp;
        auto make_key = [](const sql::Tuple &row) {
            std::string key;
            for (auto &v : row) key += v.to_string() + "|";
            return key;
        };

        switch (sel.set_op) {
            case SO::UNION_ALL:
                result.rows = std::move(left.rows);
                result.rows.insert(result.rows.end(), right.rows.begin(), right.rows.end());
                break;
            case SO::UNION: {
                result.rows = std::move(left.rows);
                result.rows.insert(result.rows.end(), right.rows.begin(), right.rows.end());
                std::set<std::string> seen;
                std::vector<sql::Tuple> unique;
                for (auto &row : result.rows) {
                    if (seen.insert(make_key(row)).second) unique.push_back(std::move(row));
                }
                result.rows = std::move(unique);
                break;
            }
            case SO::INTERSECT: case SO::INTERSECT_ALL: {
                std::set<std::string> right_keys;
                for (auto &row : right.rows) right_keys.insert(make_key(row));
                for (auto &row : left.rows) {
                    if (right_keys.count(make_key(row))) result.rows.push_back(std::move(row));
                }
                break;
            }
            case SO::EXCEPT: case SO::EXCEPT_ALL: {
                std::set<std::string> right_keys;
                for (auto &row : right.rows) right_keys.insert(make_key(row));
                for (auto &row : left.rows) {
                    if (!right_keys.count(make_key(row))) result.rows.push_back(std::move(row));
                }
                break;
            }
            default: break;
        }
        return result;
    }

    // Aggregate execution
    bool has_aggregate(const std::vector<sql::ast::ExprPtr> &exprs) {
        for (auto &e : exprs) {
            if (e->type == sql::ast::ExprType::AGGREGATE_CALL) return true;
            if (e->type == sql::ast::ExprType::FUNCTION_CALL) {
                auto &fc = std::get<sql::ast::FunctionCall>(e->data);
                std::string upper_name = fc.name;
                for (auto &c : upper_name) c = (char)std::toupper((unsigned char)c);
                if (upper_name == "COUNT" || upper_name == "SUM" || upper_name == "AVG" ||
                    upper_name == "MIN" || upper_name == "MAX") return true;
            }
        }
        return false;
    }

    sql::ResultSet exec_aggregate(const sql::ast::SelectStmt &sel,
                                   const std::vector<sql::Tuple> &data,
                                   const sql::Schema &schema) {
        sql::ResultSet result;
        result.success = true;

        // Resolve all group_by expressions to column indices in the source schema.
        // For function expressions (UPPER(city), SUBSTRING(date,1,4), etc.) we store
        // -1 and keep a pointer to the expression for runtime evaluation.
        std::vector<int> all_group_indices;
        std::vector<std::string> all_group_colnames;
        std::vector<const sql::ast::Expr *> all_group_exprs; // non-null when index is -1
        for (auto &gexpr : sel.group_by) {
            if (gexpr->type == sql::ast::ExprType::COLUMN_REF) {
                auto &ref = std::get<sql::ast::ColumnRef>(gexpr->data);
                int ci = find_col_index(schema, ref.column);
                all_group_indices.push_back(ci >= 0 ? ci : -1);
                all_group_colnames.push_back(ref.column);
                all_group_exprs.push_back(nullptr);
            } else {
                all_group_indices.push_back(-1);
                all_group_colnames.push_back(get_expr_name(*gexpr));
                all_group_exprs.push_back(gexpr.get());
            }
        }

        // Build output schema
        for (auto &expr : sel.select_list) {
            result.columns.push_back({get_expr_name(*expr), ""});
        }

        // Helper: group data by a subset of column indices, evaluate, append
        // active_orig_indices maps each entry in active_indices back to its position in all_group_indices
        auto run_one_grouping_set = [&](const std::vector<int> &active_indices,
                                         const std::vector<size_t> &active_orig_indices = {}) {
            struct GroupState {
                sql::Tuple group_key;
                std::vector<sql::Tuple> rows;
            };
            std::vector<GroupState> groups;
            std::unordered_map<std::string, size_t> group_map;

            for (auto &row : data) {
                std::string key;
                sql::Tuple gk;
                for (size_t ai = 0; ai < active_indices.size(); ai++) {
                    int gi = active_indices[ai];
                    if (gi >= 0 && gi < (int)row.size()) {
                        key += row[gi].to_string() + "|";
                        gk.push_back(row[gi]);
                    } else {
                        // Function expression in GROUP BY — evaluate it
                        // Find the original group expr for this active index
                        // active_indices maps from grouping set index -> all_group_indices value
                        // We need the corresponding expression
                        size_t orig_idx = ai < active_orig_indices.size() ? active_orig_indices[ai] : ai;
                        if (orig_idx < all_group_exprs.size() && all_group_exprs[orig_idx]) {
                            auto v = eval_expr_with_row(*all_group_exprs[orig_idx], row, schema);
                            key += v.to_string() + "|";
                            gk.push_back(v);
                        }
                    }
                }
                auto it = group_map.find(key);
                if (it == group_map.end()) {
                    group_map[key] = groups.size();
                    groups.push_back({gk, {row}});
                } else {
                    groups[it->second].rows.push_back(row);
                }
            }

            if (groups.empty() && active_indices.empty()) {
                groups.push_back({{}, data});
            }

            for (auto &grp : groups) {
                sql::Tuple out_row;
                for (auto &expr : sel.select_list) {
                    // For columns not in this grouping set, output NULL
                    if (expr->type == sql::ast::ExprType::COLUMN_REF) {
                        auto &ref = std::get<sql::ast::ColumnRef>(expr->data);
                        if (grouping_null_columns_.count(ref.column)) {
                            out_row.push_back(sql::Value::make_null());
                            continue;
                        }
                    }
                    out_row.push_back(eval_aggregate_expr(*expr, grp.rows, schema));
                }
                result.rows.push_back(std::move(out_row));
                // Stash group rows for HAVING evaluation
                having_groups_.push_back(std::move(grp.rows));
            }
        };

        having_groups_.clear();
        bool is_grouping_sets = (sel.group_by_mode != sql::ast::SelectStmt::GroupByMode::SIMPLE);

        if (!is_grouping_sets) {
            // SIMPLE mode: build identity orig-index mapping
            grouping_null_columns_.clear();
            std::vector<size_t> identity_orig;
            for (size_t i = 0; i < all_group_indices.size(); i++) identity_orig.push_back(i);
            run_one_grouping_set(all_group_indices, identity_orig);
        } else {
            // GROUPING SETS / CUBE / ROLLUP mode
            for (auto &gset : sel.grouping_sets) {
                std::vector<int> active_indices;
                std::vector<size_t> active_orig;
                std::set<std::string> active_colnames;
                for (int idx : gset) {
                    if (idx >= 0 && idx < (int)all_group_indices.size()) {
                        active_indices.push_back(all_group_indices[idx]);
                        active_orig.push_back(static_cast<size_t>(idx));
                        active_colnames.insert(all_group_colnames[idx]);
                    }
                }

                // Set which columns are aggregated away for GROUPING() function
                grouping_null_columns_.clear();
                for (size_t i = 0; i < all_group_colnames.size(); i++) {
                    if (active_colnames.find(all_group_colnames[i]) == active_colnames.end()) {
                        grouping_null_columns_.insert(all_group_colnames[i]);
                    }
                }

                run_one_grouping_set(active_indices, active_orig);
            }
            grouping_null_columns_.clear();
        }

        // HAVING — evaluate against group rows using eval_aggregate_expr
        if (sel.having) {
            std::vector<sql::Tuple> filtered;
            for (size_t ri = 0; ri < result.rows.size(); ri++) {
                // Evaluate HAVING predicate with aggregate access to group rows
                const auto &grp_rows = (ri < having_groups_.size()) ? having_groups_[ri] : data;
                auto hval = eval_aggregate_expr(*sel.having, grp_rows, schema);
                bool keep = false;
                if (hval.type == sql::Value::Type::BOOL) keep = hval.bool_val;
                else if (hval.type == sql::Value::Type::INT64) keep = (hval.int_val != 0);
                else if (!hval.is_null()) keep = true;
                if (keep) filtered.push_back(std::move(result.rows[ri]));
            }
            result.rows = std::move(filtered);
            having_groups_.clear();
        }

        return result;
    }

    sql::Value eval_aggregate_expr(const sql::ast::Expr &expr,
                                    const std::vector<sql::Tuple> &group_rows,
                                    const sql::Schema &schema) {
        if (expr.type == sql::ast::ExprType::AGGREGATE_CALL) {
            auto &agg = std::get<sql::ast::AggregateCall>(expr.data);
            return compute_aggregate(agg.name, agg.args, group_rows, schema);
        }
        // GROUPING() function — check before column ref / general eval
        if (expr.type == sql::ast::ExprType::FUNCTION_CALL) {
            auto &fc = std::get<sql::ast::FunctionCall>(expr.data);
            std::string fn_upper = fc.name;
            for (auto &c : fn_upper) c = (char)std::toupper((unsigned char)c);
            if (fn_upper == "GROUPING" && !fc.args.empty()) {
                if (fc.args[0]->type == sql::ast::ExprType::COLUMN_REF) {
                    auto &ref = std::get<sql::ast::ColumnRef>(fc.args[0]->data);
                    bool is_null = grouping_null_columns_.count(ref.column) > 0;
                    return sql::Value::make_int(is_null ? 1 : 0);
                }
                return sql::Value::make_int(0);
            }
        }
        if (expr.type == sql::ast::ExprType::COLUMN_REF) {
            if (!group_rows.empty()) {
                return eval_expr_with_row(expr, group_rows[0], schema);
            }
        }
        // BINARY_OP — recurse into children so aggregates in HAVING work
        // e.g., COUNT(*) >= 2 is BINARY_OP(AGGREGATE_CALL, LITERAL)
        if (expr.type == sql::ast::ExprType::BINARY_OP) {
            auto &op = std::get<sql::ast::BinaryOp>(expr.data);
            auto lv = eval_aggregate_expr(*op.left, group_rows, schema);
            auto rv = eval_aggregate_expr(*op.right, group_rows, schema);
            return eval_binary_op(op.op, lv, rv);
        }
        // UNARY_OP — recurse (for NOT, minus, etc.)
        if (expr.type == sql::ast::ExprType::UNARY_OP) {
            auto &op = std::get<sql::ast::UnaryOp>(expr.data);
            auto v = eval_aggregate_expr(*op.operand, group_rows, schema);
            if (op.op == sql::TokenType::KW_NOT) {
                if (v.is_null()) return sql::Value::make_null();
                bool truthy = (v.type == sql::Value::Type::BOOL) ? v.bool_val : (v.int_val != 0);
                return sql::Value::make_bool(!truthy);
            }
            if (op.op == sql::TokenType::MINUS) {
                if (v.type == sql::Value::Type::INT64) return sql::Value::make_int(-v.int_val);
                if (v.type == sql::Value::Type::FLOAT64) return sql::Value::make_float(-v.float_val);
            }
            return v;
        }
        // For non-aggregate expressions in an aggregate context, eval with first row
        if (!group_rows.empty()) {
            return eval_expr_with_row(expr, group_rows[0], schema);
        }
        return sql::Value::make_null();
    }

    sql::Value compute_aggregate(const std::string &name,
                                  const std::vector<sql::ast::ExprPtr> &args,
                                  const std::vector<sql::Tuple> &rows,
                                  const sql::Schema &schema) {
        std::string upper = name;
        for (auto &c : upper) c = (char)std::toupper((unsigned char)c);

        if (upper == "COUNT") {
            if (!args.empty() && args[0]->type == sql::ast::ExprType::COLUMN_REF) {
                auto &ref = std::get<sql::ast::ColumnRef>(args[0]->data);
                if (ref.column == "*") return sql::Value::make_int((int64_t)rows.size());
                int ci = find_col_index(schema, ref.column);
                int64_t count = 0;
                for (auto &row : rows) {
                    if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) count++;
                }
                return sql::Value::make_int(count);
            }
            return sql::Value::make_int((int64_t)rows.size());
        }

        // For SUM/AVG/MIN/MAX, get the column
        int ci = -1;
        if (!args.empty() && args[0]->type == sql::ast::ExprType::COLUMN_REF) {
            auto &ref = std::get<sql::ast::ColumnRef>(args[0]->data);
            ci = find_col_index(schema, ref.column);
        }

        if (upper == "SUM") {
            double sum = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                }
            }
            return sql::Value::make_float(sum);
        }
        if (upper == "AVG") {
            double sum = 0; int64_t cnt = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                    cnt++;
                }
            }
            return cnt > 0 ? sql::Value::make_float(sum / cnt) : sql::Value::make_null();
        }
        if (upper == "STDDEV" || upper == "STDDEV_POP" || upper == "STDEV") {
            double sum = 0; int64_t cnt = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                    cnt++;
                }
            }
            if (cnt < 1) return sql::Value::make_null();
            double mean = sum / cnt;
            double var_sum = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    double x = 0;
                    if (row[ci].type == sql::Value::Type::INT64) x = (double)row[ci].int_val;
                    else if (row[ci].type == sql::Value::Type::FLOAT64) x = row[ci].float_val;
                    var_sum += (x - mean) * (x - mean);
                }
            }
            return sql::Value::make_float(std::sqrt(var_sum / cnt));
        }
        if (upper == "STDDEV_SAMP") {
            double sum = 0; int64_t cnt = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                    cnt++;
                }
            }
            if (cnt < 2) return sql::Value::make_null();
            double mean = sum / cnt;
            double var_sum = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    double x = 0;
                    if (row[ci].type == sql::Value::Type::INT64) x = (double)row[ci].int_val;
                    else if (row[ci].type == sql::Value::Type::FLOAT64) x = row[ci].float_val;
                    var_sum += (x - mean) * (x - mean);
                }
            }
            return sql::Value::make_float(std::sqrt(var_sum / (cnt - 1)));
        }
        if (upper == "VARIANCE" || upper == "VAR_POP" || upper == "VARP") {
            double sum = 0; int64_t cnt = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                    cnt++;
                }
            }
            if (cnt < 1) return sql::Value::make_null();
            double mean = sum / cnt;
            double var_sum = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    double x = 0;
                    if (row[ci].type == sql::Value::Type::INT64) x = (double)row[ci].int_val;
                    else if (row[ci].type == sql::Value::Type::FLOAT64) x = row[ci].float_val;
                    var_sum += (x - mean) * (x - mean);
                }
            }
            return sql::Value::make_float(var_sum / cnt);
        }
        if (upper == "VAR_SAMP" || upper == "VAR") {
            double sum = 0; int64_t cnt = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    auto &v = row[ci];
                    if (v.type == sql::Value::Type::INT64) sum += (double)v.int_val;
                    else if (v.type == sql::Value::Type::FLOAT64) sum += v.float_val;
                    cnt++;
                }
            }
            if (cnt < 2) return sql::Value::make_null();
            double mean = sum / cnt;
            double var_sum = 0;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    double x = 0;
                    if (row[ci].type == sql::Value::Type::INT64) x = (double)row[ci].int_val;
                    else if (row[ci].type == sql::Value::Type::FLOAT64) x = row[ci].float_val;
                    var_sum += (x - mean) * (x - mean);
                }
            }
            return sql::Value::make_float(var_sum / (cnt - 1));
        }
        if (upper == "STRING_AGG" || upper == "GROUP_CONCAT") {
            // STRING_AGG(col, separator)
            std::string sep = ",";
            if (args.size() >= 2) {
                auto sv = eval_expr(*args[1], {}, {});
                if (!sv.is_null()) sep = sv.to_string();
            }
            std::string result;
            bool first = true;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    if (!first) result += sep;
                    result += row[ci].to_string();
                    first = false;
                }
            }
            return first ? sql::Value::make_null() : sql::Value::make_string(result);
        }
        if (upper == "MEDIAN") {
            std::vector<double> vals_vec;
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    if (row[ci].type == sql::Value::Type::INT64) vals_vec.push_back((double)row[ci].int_val);
                    else if (row[ci].type == sql::Value::Type::FLOAT64) vals_vec.push_back(row[ci].float_val);
                }
            }
            if (vals_vec.empty()) return sql::Value::make_null();
            std::sort(vals_vec.begin(), vals_vec.end());
            size_t n = vals_vec.size();
            double median = (n % 2 == 1) ? vals_vec[n/2] : (vals_vec[n/2-1] + vals_vec[n/2]) / 2.0;
            return sql::Value::make_float(median);
        }
        if (upper == "MIN" || upper == "MAX") {
            sql::Value best = sql::Value::make_null();
            for (auto &row : rows) {
                if (ci >= 0 && ci < (int)row.size() && !row[ci].is_null()) {
                    if (best.is_null()) { best = row[ci]; continue; }
                    bool replace = false;
                    if (row[ci].type == sql::Value::Type::INT64 && best.type == sql::Value::Type::INT64) {
                        replace = upper == "MIN" ? row[ci].int_val < best.int_val : row[ci].int_val > best.int_val;
                    } else {
                        replace = upper == "MIN" ? row[ci].to_string() < best.to_string()
                                                 : row[ci].to_string() > best.to_string();
                    }
                    if (replace) best = row[ci];
                }
            }
            return best;
        }
        return sql::Value::make_null();
    }

    // ===============================================
    // UPDATE
    // ===============================================

    sql::ResultSet exec_update(const sql::ast::UpdateStmt &us) {
        if (!check_privilege(us.table, "UPDATE"))
            return {false, "Permission denied: UPDATE on '" + us.table + "' for user '" + current_user_ + "'", {}, {}, 0};
        auto *table = catalog_.find_table(us.table);
        if (!table) return {false, "Table '" + us.table + "' not found", {}, {}, 0};
        auto schema = catalog_.get_table_schema(us.table);

        // Collect pointers to all row vectors (partitioned or not)
        std::vector<std::vector<sql::Tuple>*> row_stores;
        if (table->is_partitioned()) {
            for (auto &p : table->partition_spec->partitions)
                row_stores.push_back(&p.rows);
        } else {
            row_stores.push_back(&table->rows);
        }

        int64_t updated = 0;
        for (auto *rows_ptr : row_stores) {
            auto &rows = *rows_ptr;
            std::vector<std::pair<size_t, sql::Tuple>> staged;
            for (size_t ri = 0; ri < rows.size(); ++ri) {
                auto &row = rows[ri];
                if (us.where_clause && !eval_predicate(*us.where_clause, row, schema)) continue;
                sql::Tuple new_row = row;
                for (auto &[col_name, expr] : us.assignments) {
                    int ci = find_col_index(schema, col_name);
                    if (ci >= 0 && ci < (int)new_row.size())
                        new_row[ci] = eval_expr_with_row(*expr, new_row, schema);
                }
                for (size_t i = 0; i < table->columns.size(); i++) {
                    if (table->columns[i].generated && table->columns[i].generated_expr)
                        new_row[i] = eval_expr_with_row(*table->columns[i].generated_expr, new_row, schema);
                }
                auto err = check_constraints(table, schema, new_row, static_cast<int>(ri));
                if (!err.empty())
                    return {false, err, {}, {}, 0};
                // BEFORE UPDATE triggers
                fire_triggers(us.table, catalog::TriggerEvent::UPDATE, catalog::TriggerTiming::BEFORE, new_row, row, schema);
                staged.emplace_back(ri, std::move(new_row));
            }
            for (auto &[ri, nr] : staged) {
                sql::Tuple old_row = rows[ri]; // save old row for AFTER trigger
                rows[ri] = std::move(nr);
                // AFTER UPDATE triggers
                fire_triggers(us.table, catalog::TriggerEvent::UPDATE, catalog::TriggerTiming::AFTER, rows[ri], old_row, schema);
            }
            updated += (int64_t)staged.size();
        }
        // RETURNING clause for UPDATE
        if (!us.returning.empty() && updated > 0) {
            sql::ResultSet ret;
            ret.success = true;
            for (auto &expr : us.returning)
                ret.columns.push_back({expr->alias.empty() ? get_expr_name(*expr) : expr->alias, ""});
            // Collect updated rows from all stores
            for (auto *rows_ptr : row_stores) {
                for (auto &row : *rows_ptr) {
                    sql::Tuple out;
                    for (auto &expr : us.returning) out.push_back(eval_expr_with_row(*expr, row, schema));
                    ret.rows.push_back(std::move(out));
                }
            }
            ret.rows_affected = updated;
            return ret;
        }
        return {true, "", {}, {}, updated};
    }

    // ===============================================
    // DELETE
    // ===============================================

    sql::ResultSet exec_delete(const sql::ast::DeleteStmt &ds) {
        if (!check_privilege(ds.table, "DELETE"))
            return {false, "Permission denied: DELETE on '" + ds.table + "' for user '" + current_user_ + "'", {}, {}, 0};
        auto *table = catalog_.find_table(ds.table);
        if (!table) return {false, "Table '" + ds.table + "' not found", {}, {}, 0};
        auto schema = catalog_.get_table_schema(ds.table);

        // Collect pointers to all row vectors
        std::vector<std::vector<sql::Tuple>*> row_stores;
        if (table->is_partitioned()) {
            for (auto &p : table->partition_spec->partitions)
                row_stores.push_back(&p.rows);
        } else {
            row_stores.push_back(&table->rows);
        }

        // 1. Mark rows targeted for deletion in each row store.
        // Store (store_idx, row_indices) pairs
        struct StoreTargets {
            std::vector<sql::Tuple>* store;
            std::vector<size_t> indices;
        };
        std::vector<StoreTargets> all_targets;
        std::vector<sql::Tuple> deleted_rows; // for FK checking

        for (auto *rs : row_stores) {
            StoreTargets st; st.store = rs;
            for (size_t ri = 0; ri < rs->size(); ++ri) {
                if (!ds.where_clause || eval_predicate(*ds.where_clause, (*rs)[ri], schema)) {
                    st.indices.push_back(ri);
                    deleted_rows.push_back((*rs)[ri]);
                }
            }
            if (!st.indices.empty()) all_targets.push_back(std::move(st));
        }
        if (deleted_rows.empty()) return {true, "", {}, {}, 0};

        // 2. FK cascade/restrict using deleted_rows
        for (auto &child_name : catalog_.list_tables()) {
            if (child_name == ds.table) continue;
            auto *child = catalog_.find_table(child_name);
            if (!child) continue;
            auto child_schema = catalog_.get_table_schema(child_name);
            for (auto &fk : child->foreign_keys) {
                if (fk.ref_table != ds.table) continue;
                std::string action = fk.on_delete.empty() ? "RESTRICT" : fk.on_delete;
                for (auto &c : action) c = (char)std::toupper((unsigned char)c);

                auto fk_references = [&](const sql::Tuple &child_row,
                                         const sql::Tuple &parent_row) {
                    for (size_t j = 0; j < fk.columns.size() && j < fk.ref_columns.size(); ++j) {
                        int ci = find_col_index(child_schema, fk.columns[j]);
                        int pi = find_col_index(schema, fk.ref_columns[j]);
                        if (ci < 0 || pi < 0 ||
                            ci >= (int)child_row.size() || pi >= (int)parent_row.size())
                            return false;
                        if (child_row[ci].is_null() || parent_row[pi].is_null()) return false;
                        if (child_row[ci].to_string() != parent_row[pi].to_string())
                            return false;
                    }
                    return true;
                };

                std::vector<size_t> child_cascade_rows;
                for (size_t cri = 0; cri < child->rows.size(); ++cri) {
                    auto &child_row = child->rows[cri];
                    bool referenced = false;
                    for (auto &dr : deleted_rows) {
                        if (fk_references(child_row, dr)) { referenced = true; break; }
                    }
                    if (!referenced) continue;
                    if (action == "RESTRICT" || action == "NO ACTION") {
                        return {false,
                                "FOREIGN KEY constraint violated: row in '" + child_name +
                                "' references rows being deleted in '" + ds.table + "'",
                                {}, {}, 0};
                    } else if (action == "CASCADE") {
                        child_cascade_rows.push_back(cri);
                    } else if (action == "SET NULL") {
                        for (auto &fc : fk.columns) {
                            int ci = find_col_index(child_schema, fc);
                            if (ci >= 0 && ci < (int)child_row.size())
                                child_row[ci] = sql::Value::make_null();
                        }
                    } else if (action == "SET DEFAULT") {
                        for (auto &fc : fk.columns) {
                            int ci = find_col_index(child_schema, fc);
                            if (ci < 0 || ci >= (int)child_row.size()) continue;
                            auto &col_info = child->columns[(size_t)ci];
                            child_row[ci] = col_info.default_value
                                ? eval_expr(*col_info.default_value, child_row, child_schema)
                                : sql::Value::make_null();
                        }
                    }
                }
                std::sort(child_cascade_rows.begin(), child_cascade_rows.end());
                child_cascade_rows.erase(
                    std::unique(child_cascade_rows.begin(), child_cascade_rows.end()),
                    child_cascade_rows.end());
                for (auto it = child_cascade_rows.rbegin(); it != child_cascade_rows.rend(); ++it)
                    if (*it < child->rows.size()) child->rows.erase(child->rows.begin() + (long)*it);
            }
        }

        // 3. Fire BEFORE DELETE triggers, then erase targeted rows.
        int64_t total_deleted = 0;
        for (auto &st : all_targets) {
            // BEFORE DELETE triggers
            for (auto idx : st.indices) {
                if (idx < st.store->size()) {
                    sql::Tuple empty;
                    auto &old_row = (*st.store)[idx];
                    fire_triggers(ds.table, catalog::TriggerEvent::DELETE, catalog::TriggerTiming::BEFORE, empty, old_row, schema);
                }
            }
            std::sort(st.indices.begin(), st.indices.end());
            for (auto it = st.indices.rbegin(); it != st.indices.rend(); ++it) {
                if (*it < st.store->size()) {
                    st.store->erase(st.store->begin() + (long)*it);
                }
            }
            total_deleted += (int64_t)st.indices.size();
        }
        // AFTER DELETE triggers
        for (auto &del_row : deleted_rows) {
            sql::Tuple empty;
            fire_triggers(ds.table, catalog::TriggerEvent::DELETE, catalog::TriggerTiming::AFTER, empty, del_row, schema);
        }
        // RETURNING clause for DELETE
        if (!ds.returning.empty() && !deleted_rows.empty()) {
            sql::ResultSet ret;
            ret.success = true;
            for (auto &expr : ds.returning)
                ret.columns.push_back({expr->alias.empty() ? get_expr_name(*expr) : expr->alias, ""});
            for (auto &row : deleted_rows) {
                sql::Tuple out;
                for (auto &expr : ds.returning) out.push_back(eval_expr_with_row(*expr, row, schema));
                ret.rows.push_back(std::move(out));
            }
            ret.rows_affected = total_deleted;
            return ret;
        }
        return {true, "", {}, {}, total_deleted};
    }

    // ===============================================
    // MERGE
    // ===============================================

    sql::ResultSet exec_merge(const sql::ast::MergeStmt &ms) {
        auto *target = catalog_.find_table(ms.target_table);
        if (!target) return {false, "Target table '" + ms.target_table + "' not found", {}, {}, 0};

        // Resolve source
        sql::Schema src_schema;
        std::vector<sql::Tuple> src_rows;
        if (ms.source && !resolve_table_ref(*ms.source, src_schema, src_rows))
            return {false, "Source table not found", {}, {}, 0};

        auto tgt_schema = catalog_.get_table_schema(ms.target_table);
        auto combined_schema = cross_join_schemas(tgt_schema, src_schema);

        // Helper: pick first matching WHEN clause whose AND-condition is satisfied.
        // SQL MERGE semantics: each source row applies at most one WHEN clause
        // (the first whose predicate is satisfied). Subsequent clauses for the
        // same row are skipped, even if they would also match.
        auto find_clause = [&](bool matched,
                               const sql::Tuple &combined) -> const sql::ast::MergeWhenClause * {
            for (auto &clause : ms.when_clauses) {
                if (clause.matched != matched) continue;
                if (clause.condition &&
                    !eval_predicate(*clause.condition, combined, combined_schema)) continue;
                return &clause;
            }
            return nullptr;
        };

        int64_t affected = 0;
        // Collect target-row indices to delete; defer the actual erase to avoid
        // invalidating the iteration order while we're still scanning.
        std::vector<size_t> rows_to_delete;
        for (auto &src_row : src_rows) {
            bool matched = false;
            for (size_t ti = 0; ti < target->rows.size(); ++ti) {
                auto &tgt_row = target->rows[ti];
                sql::Tuple combined = tgt_row;
                combined.insert(combined.end(), src_row.begin(), src_row.end());
                if (ms.on_condition && eval_predicate(*ms.on_condition, combined, combined_schema)) {
                    matched = true;
                    const auto *clause = find_clause(/*matched=*/true, combined);
                    if (!clause) break;
                    using Action = sql::ast::MergeWhenClause::Action;
                    if (clause->action == Action::UPDATE) {
                        for (auto &[col, expr] : clause->assignments) {
                            std::string cname = col;
                            auto dot = cname.find('.');
                            if (dot != std::string::npos) cname = cname.substr(dot + 1);
                            int ci = find_col_index(tgt_schema, cname);
                            if (ci >= 0 && ci < (int)tgt_row.size())
                                tgt_row[ci] = eval_expr_with_row(*expr, combined, combined_schema);
                        }
                        affected++;
                    } else if (clause->action == Action::DELETE) {
                        rows_to_delete.push_back(ti);
                        affected++;
                    }
                    break;
                }
            }
            if (!matched) {
                // Empty combined row is target-side NULLs; source-side is the live src_row.
                sql::Tuple combined(tgt_schema.size(), sql::Value::make_null());
                combined.insert(combined.end(), src_row.begin(), src_row.end());
                const auto *clause = find_clause(/*matched=*/false, combined);
                if (clause && clause->action == sql::ast::MergeWhenClause::Action::INSERT) {
                    sql::Tuple new_row(target->columns.size(), sql::Value::make_null());
                    // If explicit columns were named, route values to those positions;
                    // otherwise fall back to positional INSERT semantics.
                    if (!clause->columns.empty()) {
                        for (size_t i = 0; i < clause->values.size() && i < clause->columns.size(); ++i) {
                            std::string cname = clause->columns[i];
                            auto dot = cname.find('.');
                            if (dot != std::string::npos) cname = cname.substr(dot + 1);
                            int ci = find_col_index(tgt_schema, cname);
                            if (ci >= 0 && ci < (int)new_row.size())
                                new_row[ci] = eval_expr_with_row(*clause->values[i], combined, combined_schema);
                        }
                    } else {
                        for (size_t i = 0; i < clause->values.size() && i < new_row.size(); ++i) {
                            new_row[i] = eval_expr_with_row(*clause->values[i], combined, combined_schema);
                        }
                    }
                    target->rows.push_back(std::move(new_row));
                    affected++;
                }
            }
        }
        // Apply deferred deletes in reverse index order so earlier offsets stay valid.
        std::sort(rows_to_delete.begin(), rows_to_delete.end());
        rows_to_delete.erase(std::unique(rows_to_delete.begin(), rows_to_delete.end()),
                             rows_to_delete.end());
        for (auto it = rows_to_delete.rbegin(); it != rows_to_delete.rend(); ++it)
            if (*it < target->rows.size()) target->rows.erase(target->rows.begin() + (long)*it);
        return {true, "", {}, {}, affected};
    }

    // ===============================================
    // Expression Evaluator -- COMPLETE
    // ===============================================

    sql::Value eval_expr(const sql::ast::Expr &expr,
                         const sql::Tuple &row, const sql::Schema &schema) {
        return eval_expr_with_row(expr, row, schema);
    }

    sql::Value eval_expr_with_row(const sql::ast::Expr &expr,
                                   const sql::Tuple &row, const sql::Schema &schema) {
        switch (expr.type) {
        case sql::ast::ExprType::LITERAL: {
            auto &lit = std::get<sql::ast::Literal>(expr.data);
            switch (lit.token_type) {
                case sql::TokenType::INTEGER_LITERAL: return sql::Value::make_int(std::stoll(lit.value));
                case sql::TokenType::FLOAT_LITERAL: return sql::Value::make_float(std::stod(lit.value));
                case sql::TokenType::STRING_LITERAL: return sql::Value::make_string(lit.value);
                case sql::TokenType::BOOLEAN_LITERAL: return sql::Value::make_bool(lit.value == "TRUE");
                case sql::TokenType::NULL_LITERAL: return sql::Value::make_null();
                default: return sql::Value::make_string(lit.value);
            }
        }
        case sql::ast::ExprType::COLUMN_REF: {
            auto &ref = std::get<sql::ast::ColumnRef>(expr.data);
            int ci = find_col_index_qualified(schema, ref);
            if (ci >= 0 && ci < (int)row.size()) return row[ci];
            // Fall back to outer scopes (correlated subquery) — innermost first.
            for (auto it = outer_scopes_.rbegin(); it != outer_scopes_.rend(); ++it) {
                int oci = find_col_index_qualified(*it->schema, ref);
                if (oci >= 0 && it->row && oci < (int)it->row->size())
                    return (*it->row)[oci];
            }
            return sql::Value::make_null();
        }
        case sql::ast::ExprType::BINARY_OP: {
            auto &op = std::get<sql::ast::BinaryOp>(expr.data);
            auto lv = eval_expr_with_row(*op.left, row, schema);
            auto rv = eval_expr_with_row(*op.right, row, schema);
            return eval_binary_op(op.op, lv, rv);
        }
        case sql::ast::ExprType::UNARY_OP: {
            auto &op = std::get<sql::ast::UnaryOp>(expr.data);
            auto v = eval_expr_with_row(*op.operand, row, schema);
            if (op.op == sql::TokenType::MINUS) {
                if (v.type == sql::Value::Type::INT64) return sql::Value::make_int(-v.int_val);
                if (v.type == sql::Value::Type::FLOAT64) return sql::Value::make_float(-v.float_val);
            }
            if (op.op == sql::TokenType::KW_NOT) {
                if (v.is_null()) return sql::Value::make_null();
                // Handle all truthy value types, not just BOOL
                bool truthy = false;
                if (v.type == sql::Value::Type::BOOL) truthy = v.bool_val;
                else if (v.type == sql::Value::Type::INT64) truthy = (v.int_val != 0);
                else if (v.type == sql::Value::Type::FLOAT64) truthy = (v.float_val != 0.0);
                else if (v.type == sql::Value::Type::STRING) truthy = !v.str_val.empty();
                else truthy = true; // non-null = truthy
                return sql::Value::make_bool(!truthy);
            }
            return v;
        }
        case sql::ast::ExprType::IS_NULL_EXPR: {
            auto &isn = std::get<sql::ast::IsNullExpr>(expr.data);
            auto v = eval_expr_with_row(*isn.operand, row, schema);
            bool r = v.is_null();
            return sql::Value::make_bool(isn.negated ? !r : r);
        }
        case sql::ast::ExprType::FUNCTION_CALL: {
            auto &fc = std::get<sql::ast::FunctionCall>(expr.data);
            return eval_function(fc.name, fc.args, row, schema);
        }
        case sql::ast::ExprType::CAST_EXPR: {
            auto &ce = std::get<sql::ast::CastExpr>(expr.data);
            auto v = eval_expr_with_row(*ce.operand, row, schema);
            return eval_cast(v, ce.target_type);
        }
        case sql::ast::ExprType::CASE_EXPR: {
            auto &ce = std::get<sql::ast::CaseExpr>(expr.data);
            if (ce.operand) {
                // Simple CASE
                auto base = eval_expr_with_row(*ce.operand, row, schema);
                for (auto &[when_e, then_e] : ce.when_clauses) {
                    auto wv = eval_expr_with_row(*when_e, row, schema);
                    if (!base.is_null() && !wv.is_null() && base.to_string() == wv.to_string())
                        return eval_expr_with_row(*then_e, row, schema);
                }
            } else {
                // Searched CASE
                for (auto &[when_e, then_e] : ce.when_clauses) {
                    if (eval_predicate(*when_e, row, schema))
                        return eval_expr_with_row(*then_e, row, schema);
                }
            }
            if (ce.else_clause) return eval_expr_with_row(*ce.else_clause, row, schema);
            return sql::Value::make_null();
        }
        case sql::ast::ExprType::IN_EXPR: {
            auto &ie = std::get<sql::ast::InExpr>(expr.data);
            auto val = eval_expr_with_row(*ie.operand, row, schema);
            if (val.is_null()) return sql::Value::make_null();
            bool found = false;
            bool saw_null = false;
            if (auto *vals = std::get_if<std::vector<sql::ast::ExprPtr>>(&ie.values)) {
                for (auto &e : *vals) {
                    auto v = eval_expr_with_row(*e, row, schema);
                    if (v.is_null()) { saw_null = true; continue; }
                    if (val.to_string() == v.to_string()) { found = true; break; }
                }
            } else if (auto *sub = std::get_if<sql::ast::SelectPtr>(&ie.values)) {
                // IN (subquery) — execute with outer scope so correlated refs resolve.
                if (*sub) {
                    ScopePush guard(this, row, schema);
                    auto r = exec_select(**sub);
                    if (r.success) {
                        for (auto &rr : r.rows) {
                            if (rr.empty()) continue;
                            if (rr[0].is_null()) { saw_null = true; continue; }
                            if (val.to_string() == rr[0].to_string()) { found = true; break; }
                        }
                    }
                }
            }
            // SQL three-valued logic: IN with no match but a NULL in the list returns NULL.
            // NOT IN with no match and a NULL in the list also returns NULL.
            if (!found && saw_null) return sql::Value::make_null();
            return sql::Value::make_bool(ie.negated ? !found : found);
        }
        case sql::ast::ExprType::BETWEEN_EXPR: {
            auto &be = std::get<sql::ast::BetweenExpr>(expr.data);
            auto val = eval_expr_with_row(*be.operand, row, schema);
            auto lo = eval_expr_with_row(*be.low, row, schema);
            auto hi = eval_expr_with_row(*be.high, row, schema);
            bool in_range = compare_values(val, lo) >= 0 && compare_values(val, hi) <= 0;
            return sql::Value::make_bool(be.negated ? !in_range : in_range);
        }
        case sql::ast::ExprType::LIKE_EXPR: {
            auto &le = std::get<sql::ast::LikeExpr>(expr.data);
            auto val = eval_expr_with_row(*le.operand, row, schema);
            auto pat = eval_expr_with_row(*le.pattern, row, schema);
            if (val.is_null() || pat.is_null()) return sql::Value::make_null();
            std::string s = val.to_string(), p = pat.to_string();
            if (le.ilike) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); for (auto &c : p) c = (char)std::tolower((unsigned char)c); }
            bool m = like_match(s, p);
            return sql::Value::make_bool(le.negated ? !m : m);
        }
        case sql::ast::ExprType::EXISTS_EXPR: {
            auto &ee = std::get<sql::ast::ExistsExpr>(expr.data);
            if (ee.subquery) {
                ScopePush guard(this, row, schema);
                auto r = exec_select(*ee.subquery);
                return sql::Value::make_bool(r.success && !r.rows.empty());
            }
            return sql::Value::make_bool(false);
        }
        case sql::ast::ExprType::SUBQUERY: {
            auto &sq = std::get<sql::ast::SubqueryExpr>(expr.data);
            if (sq.query) {
                ScopePush guard(this, row, schema);
                auto r = exec_select(*sq.query);
                if (r.success && !r.rows.empty() && !r.rows[0].empty())
                    return r.rows[0][0];
            }
            return sql::Value::make_null();
        }
        case sql::ast::ExprType::AGGREGATE_CALL:
            return sql::Value::make_null(); // handled by exec_aggregate
        case sql::ast::ExprType::WINDOW_CALL:
            return sql::Value::make_null(); // future
        case sql::ast::ExprType::PARAMETER:
            return sql::Value::make_null(); // future
        }
        return sql::Value::make_null();
    }

    // ===============================================
    // Built-in Functions
    // ===============================================

    sql::Value eval_function(const std::string &name,
                              const std::vector<sql::ast::ExprPtr> &args,
                              const sql::Tuple &row, const sql::Schema &schema) {
        std::string upper = name;
        for (auto &c : upper) c = (char)std::toupper((unsigned char)c);

        // GROUPING(col) — returns 1 if the column was aggregated away, 0 otherwise
        if (upper == "GROUPING" && !args.empty()) {
            if (args[0]->type == sql::ast::ExprType::COLUMN_REF) {
                auto &ref = std::get<sql::ast::ColumnRef>(args[0]->data);
                bool is_null = grouping_null_columns_.count(ref.column) > 0;
                return sql::Value::make_int(is_null ? 1 : 0);
            }
            return sql::Value::make_int(0);
        }

        // Evaluate args
        std::vector<sql::Value> vals;
        for (auto &a : args) vals.push_back(eval_expr_with_row(*a, row, schema));

        // String functions
        // UTF-8 aware UPPER/LOWER for the BMP. ASCII is fast-pathed; non-ASCII
        // covers Latin-1 supplement (U+00C0–U+00FF) and Latin Extended-A
        // (U+0100–U+017F). Cyrillic basic block U+0410–U+045F is also covered.
        // Other scripts pass through unchanged — ICU is the production option.
        auto utf8_decode_step = [](const std::string &s, size_t &i, uint32_t &cp) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) { cp = c; i++; return true; }
            int need;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; need = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; need = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; need = 3; }
            else { cp = c; i++; return false; }
            if (i + (size_t)need >= s.size()) { cp = c; i++; return false; }
            for (int k = 1; k <= need; k++) {
                unsigned char ck = (unsigned char)s[i+(size_t)k];
                if ((ck & 0xC0) != 0x80) { cp = c; i++; return false; }
                cp = (cp << 6) | (ck & 0x3F);
            }
            i += (size_t)need + 1; return true;
        };
        auto utf8_encode = [](uint32_t cp, std::string &out) {
            if (cp < 0x80) out.push_back((char)cp);
            else if (cp < 0x800) {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                out.push_back((char)(0xF0 | (cp >> 18)));
                out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        };
        auto case_fold = [&](uint32_t cp, bool to_upper) -> uint32_t {
            // ASCII
            if (cp >= 'A' && cp <= 'Z' && !to_upper) return cp + 32;
            if (cp >= 'a' && cp <= 'z' &&  to_upper) return cp - 32;
            // Latin-1 supplement letters (U+00C0–U+00DE / U+00E0–U+00FE except 0xD7/0xF7)
            if (to_upper) {
                if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) return cp - 32;
                if (cp == 0x00FF) return 0x0178; // ÿ -> Ÿ
            } else {
                if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 32;
                if (cp == 0x0178) return 0x00FF; // Ÿ -> ÿ
            }
            // Latin Extended-A: paired (Ā/ā, Ă/ă, ...) at even/odd boundaries
            if (cp >= 0x0100 && cp <= 0x0177) {
                if (cp % 2 == 0) { // capital
                    if (!to_upper) return cp + 1;
                } else {
                    if (to_upper) return cp - 1;
                }
                return cp;
            }
            // Cyrillic basic: U+0410 (А) .. U+042F (Я) upper / U+0430..0x44F lower
            if (to_upper) {
                if (cp >= 0x0430 && cp <= 0x044F) return cp - 32;
                if (cp >= 0x0450 && cp <= 0x045F) return cp - 80;
            } else {
                if (cp >= 0x0410 && cp <= 0x042F) return cp + 32;
                if (cp >= 0x0400 && cp <= 0x040F) return cp + 80;
            }
            return cp;
        };
        auto fold_str = [&](const std::string &s, bool to_upper) {
            std::string out; out.reserve(s.size());
            size_t i = 0;
            while (i < s.size()) {
                uint32_t cp = 0;
                size_t before = i;
                if (!utf8_decode_step(s, i, cp)) {
                    // Invalid sequence: pass byte through.
                    out.push_back(s[before]);
                    continue;
                }
                utf8_encode(case_fold(cp, to_upper), out);
            }
            return out;
        };
        if (upper == "UPPER" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
#ifdef TDB_WITH_ICU
            // Production path: ICU u_strToUpper handles every Unicode codepoint
            // including non-BMP code points and locale-sensitive folding.
            return sql::Value::make_string(icu_fold_to_upper(vals[0].to_string()));
#else
            return sql::Value::make_string(fold_str(vals[0].to_string(), true));
#endif
        }
        if (upper == "LOWER" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
#ifdef TDB_WITH_ICU
            return sql::Value::make_string(icu_fold_to_lower(vals[0].to_string()));
#else
            return sql::Value::make_string(fold_str(vals[0].to_string(), false));
#endif
        }
        if (upper == "LENGTH" && vals.size() >= 1) {
            // LENGTH returns bytes. CHAR_LENGTH / CHARACTER_LENGTH return
            // Unicode codepoints — handled later in the UTF-8 block.
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_int((int64_t)vals[0].to_string().size());
        }
        if (upper == "SUBSTRING" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            int64_t start = vals[1].int_val - 1; // SQL is 1-based
            if (start < 0) start = 0;
            int64_t len = (vals.size() >= 3) ? vals[2].int_val : (int64_t)s.size();
            if (start >= (int64_t)s.size()) return sql::Value::make_string("");
            return sql::Value::make_string(s.substr((size_t)start, (size_t)len));
        }
        if (upper == "TRIM" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            size_t start = s.find_first_not_of(" \t\n\r");
            size_t end = s.find_last_not_of(" \t\n\r");
            if (start == std::string::npos) return sql::Value::make_string("");
            return sql::Value::make_string(s.substr(start, end - start + 1));
        }
        if (upper == "REPLACE" && vals.size() >= 3) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string(), from = vals[1].to_string(), to = vals[2].to_string();
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return sql::Value::make_string(s);
        }
        if (upper == "CONCAT") {
            std::string result;
            for (auto &v : vals) {
                if (!v.is_null()) result += v.to_string();
            }
            return sql::Value::make_string(result);
        }
        if (upper == "LEFT" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_string(vals[0].to_string().substr(0, (size_t)vals[1].int_val));
        }
        if (upper == "RIGHT" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            size_t n = (size_t)vals[1].int_val;
            if (n >= s.size()) return sql::Value::make_string(s);
            return sql::Value::make_string(s.substr(s.size() - n));
        }
        if (upper == "REVERSE" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            std::reverse(s.begin(), s.end());
            return sql::Value::make_string(s);
        }
        if (upper == "LPAD" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            size_t target = (size_t)vals[1].int_val;
            std::string pad = vals.size() >= 3 ? vals[2].to_string() : " ";
            while (s.size() < target && !pad.empty()) s = pad + s;
            return sql::Value::make_string(s.substr(0, target));
        }
        if (upper == "RPAD" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            size_t target = (size_t)vals[1].int_val;
            std::string pad = vals.size() >= 3 ? vals[2].to_string() : " ";
            while (s.size() < target && !pad.empty()) s += pad;
            return sql::Value::make_string(s.substr(0, target));
        }
        if (upper == "REPEAT" && vals.size() >= 2) {
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string(), result;
            for (int64_t i = 0; i < vals[1].int_val; i++) result += s;
            return sql::Value::make_string(result);
        }
        if (upper == "POSITION" && vals.size() >= 2) {
            if (vals[0].is_null() || vals[1].is_null()) return sql::Value::make_null();
            auto pos = vals[1].to_string().find(vals[0].to_string());
            return sql::Value::make_int(pos == std::string::npos ? 0 : (int64_t)pos + 1);
        }

        // Null-handling functions
        if (upper == "COALESCE") {
            for (auto &v : vals) { if (!v.is_null()) return v; }
            return sql::Value::make_null();
        }
        if (upper == "NULLIF" && vals.size() >= 2) {
            if (!vals[0].is_null() && !vals[1].is_null() && vals[0].to_string() == vals[1].to_string())
                return sql::Value::make_null();
            return vals[0];
        }
        if (upper == "GREATEST") {
            sql::Value best = sql::Value::make_null();
            for (auto &v : vals) {
                if (v.is_null()) continue;
                if (best.is_null() || compare_values(v, best) > 0) best = v;
            }
            return best;
        }
        if (upper == "LEAST") {
            sql::Value best = sql::Value::make_null();
            for (auto &v : vals) {
                if (v.is_null()) continue;
                if (best.is_null() || compare_values(v, best) < 0) best = v;
            }
            return best;
        }

        // Math functions
        if (upper == "ABS" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::INT64) return sql::Value::make_int(std::abs(vals[0].int_val));
            return sql::Value::make_float(std::fabs(to_double(vals[0])));
        }
        if (upper == "ROUND" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            double v = to_double(vals[0]);
            int decimals = vals.size() >= 2 ? (int)vals[1].int_val : 0;
            double factor = std::pow(10.0, decimals);
            return sql::Value::make_float(std::round(v * factor) / factor);
        }
        if (upper == "CEIL" || upper == "CEILING") {
            if (vals.size() >= 1 && !vals[0].is_null()) return sql::Value::make_float(std::ceil(to_double(vals[0])));
            return sql::Value::make_null();
        }
        if (upper == "FLOOR") {
            if (vals.size() >= 1 && !vals[0].is_null()) return sql::Value::make_float(std::floor(to_double(vals[0])));
            return sql::Value::make_null();
        }
        if (upper == "MOD" && vals.size() >= 2) {
            if (vals[0].is_null() || vals[1].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::INT64 && vals[1].type == sql::Value::Type::INT64) {
                if (vals[1].int_val == 0) return sql::Value::make_null();
                return sql::Value::make_int(vals[0].int_val % vals[1].int_val);
            }
            return sql::Value::make_float(std::fmod(to_double(vals[0]), to_double(vals[1])));
        }
        if (upper == "POWER" && vals.size() >= 2) {
            return sql::Value::make_float(std::pow(to_double(vals[0]), to_double(vals[1])));
        }
        if (upper == "SQRT" && vals.size() >= 1) {
            return sql::Value::make_float(std::sqrt(to_double(vals[0])));
        }

        // Date/time
        if (upper == "NOW" || upper == "CURRENT_TIMESTAMP" || upper == "CURRENT_DATE" ||
            upper == "CURRENT_TIME" || upper == "LOCALTIME" || upper == "LOCALTIMESTAMP") {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            return sql::Value::make_string(buf);
        }
        // EXTRACT handled by the unified DATE_PART branch later — fall through.

        // Sequence functions
        if (upper == "NEXTVAL" && vals.size() >= 1) {
            return sql::Value::make_int(catalog_.nextval(vals[0].to_string()));
        }
        if (upper == "CURRVAL" && vals.size() >= 1) {
            return sql::Value::make_int(catalog_.currval(vals[0].to_string()));
        }

        // ─── Batch 5: composite type management ───────────────────────────
        // CREATE_TYPE('Address', 'street VARCHAR, city VARCHAR, zip VARCHAR')
        // registers a composite type. Field types can themselves be other
        // registered composite types — composition / nesting is fully supported.
        if (upper == "CREATE_TYPE" && vals.size() >= 2) {
            std::string tname = vals[0].to_string();
            std::string spec  = vals[1].to_string();
            catalog::CompositeTypeInfo info;
            info.name = tname;
            std::string cur; std::vector<std::string> field_specs;
            int depth = 0;
            for (char c : spec) {
                if (c == '(') depth++;
                if (c == ')') depth--;
                if (c == ',' && depth == 0) { field_specs.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            if (!cur.empty()) field_specs.push_back(cur);
            for (auto &fs : field_specs) {
                size_t i = 0;
                while (i < fs.size() && std::isspace((unsigned char)fs[i])) i++;
                size_t j = i;
                while (j < fs.size() && (std::isalnum((unsigned char)fs[j]) || fs[j] == '_')) j++;
                std::string fname = fs.substr(i, j - i);
                while (j < fs.size() && std::isspace((unsigned char)fs[j])) j++;
                std::string tspec = fs.substr(j);
                while (!tspec.empty() && std::isspace((unsigned char)tspec.back())) tspec.pop_back();
                catalog::CompositeFieldInfo fi;
                fi.name = fname;
                // Strip any "(p,s)" suffix and uppercase the bare type name.
                std::string bare = tspec;
                auto paren = bare.find('(');
                if (paren != std::string::npos) bare = bare.substr(0, paren);
                for (auto &c : bare) c = (char)std::toupper((unsigned char)c);
                fi.type.name = bare;
                info.fields.push_back(std::move(fi));
            }
            catalog_.add_composite_type(tname, std::move(info));
            return sql::Value::make_string("OK");
        }
        // MAKE_ROW(v1, v2, ...) — anonymous composite literal. Note the SQL
        // standard "ROW(...)" syntax is taken by the parser keyword KW_ROW;
        // expose this under a non-keyword name.
        if (upper == "MAKE_ROW") {
            std::vector<sql::Value> fs = vals;
            return sql::Value::make_composite("", std::move(fs));
        }
        // MAKE(typename, v1, v2, ...) — typed composite, validates arity against the catalog
        if (upper == "MAKE" && vals.size() >= 1) {
            std::string tname = vals[0].to_string();
            auto *info = catalog_.find_composite_type(tname);
            if (!info)
                return sql::Value::make_null();
            std::vector<sql::Value> fs(vals.begin() + 1, vals.end());
            // Pad with NULL or truncate to field count.
            if (fs.size() < info->fields.size())
                fs.resize(info->fields.size(), sql::Value::make_null());
            if (fs.size() > info->fields.size())
                fs.resize(info->fields.size());
            return sql::Value::make_composite(tname, std::move(fs));
        }
        // FIELD(composite, 'fieldname') — accessor by name. For ROW() literals
        // without registered type, accepts an integer 1-based position too.
        if (upper == "FIELD" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::COMPOSITE || !vals[0].composite_fields)
                return sql::Value::make_null();
            auto &fields = *vals[0].composite_fields;
            std::string key = vals[1].to_string();
            // Position-based for anonymous composites.
            if (vals[1].type == sql::Value::Type::INT64) {
                int64_t idx = vals[1].int_val - 1;
                if (idx >= 0 && (size_t)idx < fields.size()) return fields[(size_t)idx];
                return sql::Value::make_null();
            }
            auto *info = catalog_.find_composite_type(vals[0].str_val);
            if (info) {
                for (size_t i = 0; i < info->fields.size() && i < fields.size(); i++) {
                    if (info->fields[i].name == key) return fields[i];
                }
            }
            return sql::Value::make_null();
        }
        if (upper == "CREATE_ENUM_TYPE" && vals.size() >= 2) {
            // CREATE_ENUM_TYPE('Color', 'red,green,blue') — comma-separated labels.
            std::string tname = vals[0].to_string();
            std::string spec  = vals[1].to_string();
            catalog::EnumTypeInfo info;
            info.name = tname;
            std::string cur;
            for (char c : spec) {
                if (c == ',') { while (!cur.empty() && std::isspace((unsigned char)cur.front())) cur.erase(0,1);
                                while (!cur.empty() && std::isspace((unsigned char)cur.back())) cur.pop_back();
                                info.labels.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            if (!cur.empty()) {
                while (!cur.empty() && std::isspace((unsigned char)cur.front())) cur.erase(0,1);
                while (!cur.empty() && std::isspace((unsigned char)cur.back())) cur.pop_back();
                info.labels.push_back(cur);
            }
            catalog_.add_enum_type(tname, std::move(info));
            return sql::Value::make_string("OK");
        }
        if (upper == "MAKE_ENUM" && vals.size() >= 2) {
            std::string tname = vals[0].to_string();
            std::string lbl   = vals[1].to_string();
            auto *info = catalog_.find_enum_type(tname);
            if (!info) return sql::Value::make_null();
            for (size_t k = 0; k < info->labels.size(); k++)
                if (info->labels[k] == lbl) return sql::Value::make_enum(tname, (int64_t)k);
            return sql::Value::make_null(); // label not allowed
        }
        if (upper == "ENUM_LABEL" && vals.size() >= 1) {
            if (vals[0].type != sql::Value::Type::ENUM_VAL) return sql::Value::make_null();
            auto *info = catalog_.find_enum_type(vals[0].str_val);
            if (!info) return sql::Value::make_null();
            int64_t ord = vals[0].int_val;
            if (ord < 0 || (size_t)ord >= info->labels.size()) return sql::Value::make_null();
            return sql::Value::make_string(info->labels[(size_t)ord]);
        }

        // ─── ARRAY builtins ────────────────────────────────────────────────
        if (upper == "MAKE_ARRAY") {
            return sql::Value::make_array(std::move(vals));
        }
        if (upper == "ARRAY_LENGTH" && vals.size() >= 1) {
            if (vals[0].type != sql::Value::Type::ARRAY || !vals[0].composite_fields)
                return sql::Value::make_null();
            return sql::Value::make_int((int64_t)vals[0].composite_fields->size());
        }
        if (upper == "ARRAY_AT" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::ARRAY || !vals[0].composite_fields)
                return sql::Value::make_null();
            int64_t idx = vals[1].int_val - 1; // 1-based
            if (idx < 0 || (size_t)idx >= vals[0].composite_fields->size())
                return sql::Value::make_null();
            return (*vals[0].composite_fields)[(size_t)idx];
        }
        if (upper == "ARRAY_APPEND" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::ARRAY || !vals[0].composite_fields)
                return sql::Value::make_null();
            std::vector<sql::Value> out = *vals[0].composite_fields;
            for (size_t k = 1; k < vals.size(); k++) out.push_back(vals[k]);
            return sql::Value::make_array(std::move(out));
        }
        if (upper == "ARRAY_CONTAINS" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::ARRAY || !vals[0].composite_fields)
                return sql::Value::make_bool(false);
            std::string needle = vals[1].to_string();
            for (auto &e : *vals[0].composite_fields)
                if (e.to_string() == needle) return sql::Value::make_bool(true);
            return sql::Value::make_bool(false);
        }
        if (upper == "ARRAY_REMOVE_AT" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::ARRAY || !vals[0].composite_fields)
                return sql::Value::make_null();
            std::vector<sql::Value> out = *vals[0].composite_fields;
            int64_t idx = vals[1].int_val - 1;
            if (idx >= 0 && (size_t)idx < out.size()) out.erase(out.begin() + idx);
            return sql::Value::make_array(std::move(out));
        }
        if (upper == "ARRAY_CONCAT" && vals.size() >= 2) {
            std::vector<sql::Value> out;
            for (auto &v : vals) {
                if (v.type == sql::Value::Type::ARRAY && v.composite_fields)
                    for (auto &e : *v.composite_fields) out.push_back(e);
            }
            return sql::Value::make_array(std::move(out));
        }

        // ─── MULTISET builtins ─────────────────────────────────────────────
        if (upper == "MAKE_MULTISET") {
            return sql::Value::make_multiset(std::move(vals));
        }
        if (upper == "MULTISET_CARDINALITY" && vals.size() >= 1) {
            if (vals[0].type != sql::Value::Type::MULTISET || !vals[0].composite_fields)
                return sql::Value::make_null();
            return sql::Value::make_int((int64_t)vals[0].composite_fields->size());
        }
        if (upper == "MULTISET_COUNT" && vals.size() >= 2) {
            if (vals[0].type != sql::Value::Type::MULTISET || !vals[0].composite_fields)
                return sql::Value::make_int(0);
            std::string needle = vals[1].to_string();
            int64_t c = 0;
            for (auto &e : *vals[0].composite_fields) if (e.to_string() == needle) c++;
            return sql::Value::make_int(c);
        }
        if (upper == "MULTISET_UNION" && vals.size() >= 2) {
            std::vector<sql::Value> out;
            for (auto &v : vals) {
                if (v.type == sql::Value::Type::MULTISET && v.composite_fields)
                    for (auto &e : *v.composite_fields) out.push_back(e);
            }
            return sql::Value::make_multiset(std::move(out));
        }
        if (upper == "MULTISET_DISTINCT" && vals.size() >= 1) {
            if (vals[0].type != sql::Value::Type::MULTISET || !vals[0].composite_fields)
                return sql::Value::make_null();
            std::vector<sql::Value> out;
            std::set<std::string> seen;
            for (auto &e : *vals[0].composite_fields) {
                std::string k = e.to_string();
                if (seen.insert(k).second) out.push_back(e);
            }
            return sql::Value::make_multiset(std::move(out));
        }

        if (upper == "TYPEOF" && vals.size() >= 1) {
            using T = sql::Value::Type;
            switch (vals[0].type) {
                case T::NULL_VAL: return sql::Value::make_string("NULL");
                case T::INT64:    return sql::Value::make_string("INTEGER");
                case T::FLOAT64:  return sql::Value::make_string("FLOAT");
                case T::STRING:   return sql::Value::make_string("VARCHAR");
                case T::BOOL:     return sql::Value::make_string("BOOLEAN");
                case T::BLOB:     return sql::Value::make_string("BLOB");
                case T::VARBINARY:return sql::Value::make_string("VARBINARY");
                case T::DATE_VAL: return sql::Value::make_string("DATE");
                case T::TIME_VAL: return sql::Value::make_string("TIME");
                case T::TIMESTAMP_VAL: return sql::Value::make_string("TIMESTAMP");
                case T::TIMESTAMP_TZ:  return sql::Value::make_string("TIMESTAMP WITH TIME ZONE");
                case T::DECIMAL:  return sql::Value::make_string("DECIMAL");
                case T::UUID:     return sql::Value::make_string("UUID");
                case T::INTERVAL: return sql::Value::make_string("INTERVAL");
                case T::ENUM_VAL: return sql::Value::make_string("ENUM");
                case T::BIT_VAL:  return sql::Value::make_string("BIT");
                case T::JSON_VAL: return sql::Value::make_string("JSON");
                case T::XML_VAL:  return sql::Value::make_string("XML");
                case T::COMPOSITE: return sql::Value::make_string(vals[0].str_val.empty() ? "RECORD" : vals[0].str_val);
                case T::GEOMETRY: return sql::Value::make_string("GEOMETRY");
                case T::ARRAY:    return sql::Value::make_string("ARRAY");
                case T::MULTISET: return sql::Value::make_string("MULTISET");
            }
        }

        // ─── Batch 5: type constructors ────────────────────────────────────
        if (upper == "TO_DECIMAL" && vals.size() >= 1) {
            int scale = (vals.size() >= 2) ? (int)vals[1].int_val : 6;
            return sql::Value::make_decimal(vals[0].to_string(), scale);
        }
        if (upper == "TO_JSON" && vals.size() >= 1)   return sql::Value::make_json(vals[0].to_string());

        // ─── JSON path access (Batch 5 close-out) ────────────────────────
        // A minimal navigator: walks a JSON object/array tree following the
        // string keys / numeric indices passed in `path`. Sufficient for
        // dot-path lookup against any well-formed JSON value.
        auto json_skip_ws = [](const std::string &s, size_t &i){
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        };
        std::function<bool(const std::string &, size_t &, std::string &)> json_extract_value;
        std::function<bool(const std::string &, size_t &)> json_skip_value;
        auto json_parse_string_at = [&](const std::string &s, size_t &i, std::string &out) {
            json_skip_ws(s, i);
            if (i >= s.size() || s[i] != '"') return false;
            i++;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) { out.push_back(s[i+1]); i += 2; continue; }
                out.push_back(s[i++]);
            }
            if (i >= s.size()) return false;
            i++;
            return true;
        };
        json_skip_value = [&](const std::string &s, size_t &i) -> bool {
            json_skip_ws(s, i);
            if (i >= s.size()) return false;
            char c = s[i];
            if (c == '"') { std::string tmp; return json_parse_string_at(s, i, tmp); }
            if (c == '{' || c == '[') {
                char close_c = (c == '{') ? '}' : ']';
                int depth = 1; i++;
                while (i < s.size() && depth > 0) {
                    if (s[i] == '"') { std::string tmp; if (!json_parse_string_at(s, i, tmp)) return false; continue; }
                    if (s[i] == '{' || s[i] == '[') depth++;
                    else if (s[i] == '}' || s[i] == ']') { depth--; if (depth == 0 && s[i] == close_c) { i++; return true; } }
                    i++;
                }
                return depth == 0;
            }
            // primitive: scan until comma / closer / whitespace at top level
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
            return true;
        };
        // Extract the raw text of the JSON value at the given path. Returns
        // false if the path can't be resolved.
        json_extract_value = [&](const std::string &s, size_t &i, std::string &out) -> bool {
            json_skip_ws(s, i);
            if (i >= s.size()) return false;
            size_t start = i;
            if (!json_skip_value(s, i)) return false;
            out = s.substr(start, i - start);
            // Trim trailing whitespace.
            while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
            return true;
        };

        auto json_navigate = [&](const std::string &json,
                                 const std::vector<std::string> &path,
                                 std::string &result_text) -> bool {
            std::string cur = json;
            for (auto &step : path) {
                size_t i = 0;
                json_skip_ws(cur, i);
                if (i >= cur.size()) return false;
                if (cur[i] == '{') {
                    i++;
                    bool found = false;
                    while (i < cur.size()) {
                        json_skip_ws(cur, i);
                        if (i < cur.size() && cur[i] == '}') return false;
                        std::string key;
                        if (!json_parse_string_at(cur, i, key)) return false;
                        json_skip_ws(cur, i);
                        if (i >= cur.size() || cur[i] != ':') return false;
                        i++;
                        if (key == step) {
                            std::string raw;
                            if (!json_extract_value(cur, i, raw)) return false;
                            cur = raw;
                            found = true;
                            break;
                        } else {
                            if (!json_skip_value(cur, i)) return false;
                            json_skip_ws(cur, i);
                            if (i < cur.size() && cur[i] == ',') { i++; continue; }
                            if (i < cur.size() && cur[i] == '}') return false;
                        }
                    }
                    if (!found) return false;
                } else if (cur[i] == '[') {
                    int64_t idx = 0;
                    try { idx = std::stoll(step); } catch (...) { return false; }
                    i++;
                    int64_t k = 0;
                    while (i < cur.size()) {
                        json_skip_ws(cur, i);
                        if (i < cur.size() && cur[i] == ']') return false;
                        std::string raw;
                        if (!json_extract_value(cur, i, raw)) return false;
                        if (k == idx) { cur = raw; goto next_step; }
                        k++;
                        json_skip_ws(cur, i);
                        if (i < cur.size() && cur[i] == ',') { i++; continue; }
                        if (i < cur.size() && cur[i] == ']') return false;
                    }
                    return false;
                next_step:;
                } else {
                    return false;
                }
            }
            result_text = cur;
            return true;
        };

        if ((upper == "JSON_EXTRACT_PATH" || upper == "JSON_EXTRACT_PATH_TEXT")
            && vals.size() >= 1) {
            std::string json = vals[0].to_string();
            std::vector<std::string> path;
            for (size_t k = 1; k < vals.size(); k++) path.push_back(vals[k].to_string());
            std::string out;
            if (!json_navigate(json, path, out)) return sql::Value::make_null();
            // For JSON_EXTRACT_PATH_TEXT, strip surrounding quotes if the
            // result is a JSON string scalar.
            if (upper == "JSON_EXTRACT_PATH_TEXT" && out.size() >= 2 &&
                out.front() == '"' && out.back() == '"') {
                std::string s = out.substr(1, out.size() - 2);
                // Unescape backslash sequences.
                std::string u; u.reserve(s.size());
                for (size_t k = 0; k < s.size(); k++) {
                    if (s[k] == '\\' && k + 1 < s.size()) { u.push_back(s[k+1]); k++; }
                    else u.push_back(s[k]);
                }
                return sql::Value::make_string(u);
            }
            return upper == "JSON_EXTRACT_PATH_TEXT"
                ? sql::Value::make_string(out)
                : sql::Value::make_json(out);
        }
        if (upper == "TO_XML" && vals.size() >= 1)    return sql::Value::make_xml(vals[0].to_string());
        if (upper == "TO_UUID" && vals.size() >= 1)   return sql::Value::make_uuid(vals[0].to_string());
        if (upper == "TO_BIT" && vals.size() >= 1) {
            // Normalize: accept '0'/'1' string only.
            std::string s = vals[0].to_string();
            for (auto c : s) if (c != '0' && c != '1') return sql::Value::make_null();
            return sql::Value::make_bit(std::move(s));
        }
        if (upper == "MAKE_INTERVAL") {
            int64_t months = vals.size() > 0 ? vals[0].int_val : 0;
            int64_t days   = vals.size() > 1 ? vals[1].int_val : 0;
            int64_t secs   = vals.size() > 2 ? vals[2].int_val : 0;
            int64_t micros = days * 24LL * 3600 * 1000000LL + secs * 1000000LL;
            return sql::Value::make_interval(months, micros);
        }
        if (upper == "TS_PARSE" && vals.size() >= 1)
            return sql::parse_timestamp(vals[0].to_string());
        if (upper == "DATE_PARSE" && vals.size() >= 1)
            return sql::parse_date(vals[0].to_string());
        if (upper == "TIME_PARSE" && vals.size() >= 1)
            return sql::parse_time(vals[0].to_string());
        if (upper == "GEN_RANDOM_UUID") {
            // 16 random bytes, RFC 4122 v4 layout.
            unsigned char b[16];
            for (int i = 0; i < 16; i++) b[i] = (unsigned char)(std::rand() & 0xFF);
            b[6] = (b[6] & 0x0F) | 0x40; // version 4
            b[8] = (b[8] & 0x3F) | 0x80; // variant 1
            char out[37];
            std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
                b[10],b[11],b[12],b[13],b[14],b[15]);
            return sql::Value::make_uuid(std::string(out, 36));
        }

        // ─── Batch 4: cryptographic SQL functions ──────────────────────────
        auto to_hex = [](const uint8_t *p, size_t n) {
            static const char *hex = "0123456789abcdef";
            std::string s; s.reserve(n * 2);
            for (size_t i = 0; i < n; i++) {
                s.push_back(hex[(p[i] >> 4) & 0xF]);
                s.push_back(hex[p[i] & 0xF]);
            }
            return s;
        };
        if (upper == "SHA1" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            uint8_t h[20]; tdb_sha1(s.data(), s.size(), h);
            return sql::Value::make_string(to_hex(h, 20));
        }
        if (upper == "MD5" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            uint8_t h[16]; tdb_md5(s.data(), s.size(), h);
            return sql::Value::make_string(to_hex(h, 16));
        }
        if (upper == "HMAC_SHA1" && vals.size() >= 2) {
            std::string k = vals[0].to_string();
            std::string m = vals[1].to_string();
            uint8_t h[20];
            tdb_hmac_sha1((const uint8_t*)k.data(), k.size(),
                          (const uint8_t*)m.data(), m.size(), h);
            return sql::Value::make_string(to_hex(h, 20));
        }
        if (upper == "SHA256" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            uint8_t h[32];
            tdb_sha256(s.data(), s.size(), h);
            return sql::Value::make_string(to_hex(h, 32));
        }
        if (upper == "HMAC_SHA256" && vals.size() >= 2) {
            std::string k = vals[0].to_string();
            std::string m = vals[1].to_string();
            uint8_t h[32];
            tdb_hmac_sha256((const uint8_t*)k.data(), k.size(),
                            (const uint8_t*)m.data(), m.size(), h);
            return sql::Value::make_string(to_hex(h, 32));
        }
        if (upper == "PBKDF2_SHA256" && vals.size() >= 4) {
            std::string pw = vals[0].to_string();
            std::string salt = vals[1].to_string();
            int iters = (int)vals[2].int_val;
            int dklen = (int)vals[3].int_val;
            if (dklen <= 0 || dklen > 1024) return sql::Value::make_null();
            std::vector<uint8_t> dk((size_t)dklen);
            tdb_pbkdf2_sha256(pw.data(), pw.size(),
                              (const uint8_t*)salt.data(), salt.size(),
                              (uint32_t)iters, dk.data(), (size_t)dklen);
            return sql::Value::make_string(to_hex(dk.data(), (size_t)dklen));
        }
        if (upper == "RANDOM_BYTES" && vals.size() >= 1) {
            int n = (int)vals[0].int_val;
            if (n <= 0 || n > 4096) return sql::Value::make_null();
            std::vector<uint8_t> buf((size_t)n);
            tdb_crypto_random_bytes(buf.data(), (size_t)n);
            return sql::Value::make_varbinary(std::string((char*)buf.data(), (size_t)n));
        }

        // ─── Batch 6: timezone, full date functions ────────────────────────
        if (upper == "DATE_TRUNC" && vals.size() >= 2) {
            std::string unit = vals[0].to_string();
            for (auto &c : unit) c = (char)std::tolower((unsigned char)c);
            sql::Value v = vals[1];
            if (v.type != sql::Value::Type::TIMESTAMP_VAL && v.type != sql::Value::Type::DATE_VAL)
                return v;
            // Use Value's own component accessors (already proven by date tests).
            int y = v.date_year(), m = v.date_month(), d = v.date_day();
            int hh = 0, mi = 0, sec = 0;
            if (v.type == sql::Value::Type::TIMESTAMP_VAL) {
                hh = v.time_hour(); mi = v.time_minute(); sec = v.time_second();
            }
            if      (unit == "second" || unit == "seconds") {}
            else if (unit == "minute" || unit == "minutes") { sec = 0; }
            else if (unit == "hour"   || unit == "hours")   { sec = 0; mi = 0; }
            else if (unit == "day"    || unit == "days")    { sec = 0; mi = 0; hh = 0; }
            else if (unit == "month"  || unit == "months")  { sec = 0; mi = 0; hh = 0; d = 1; }
            else if (unit == "year"   || unit == "years")   { sec = 0; mi = 0; hh = 0; d = 1; m = 1; }
            else return v;
            return sql::Value::make_timestamp(y, m, d, hh, mi, sec);
        }
        if ((upper == "DATE_PART" || upper == "EXTRACT_PART" || upper == "EXTRACT") && vals.size() >= 2) {
            std::string field = vals[0].to_string();
            for (auto &c : field) c = (char)std::tolower((unsigned char)c);
            sql::Value v = vals[1];
            if (v.type != sql::Value::Type::DATE_VAL && v.type != sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_null();
            if (field == "year")  return sql::Value::make_int(v.date_year());
            if (field == "month") return sql::Value::make_int(v.date_month());
            if (field == "day")   return sql::Value::make_int(v.date_day());
            if (field == "hour" && v.type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(v.time_hour());
            if (field == "minute" && v.type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(v.time_minute());
            if (field == "second" && v.type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(v.time_second());
            return sql::Value::make_null();
        }
        if (upper == "AGE" && vals.size() >= 2) {
            // AGE(end, start) -> INTERVAL with both arguments as DATE/TIMESTAMP.
            if (vals[0].type != sql::Value::Type::DATE_VAL && vals[0].type != sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_null();
            if (vals[1].type != sql::Value::Type::DATE_VAL && vals[1].type != sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_null();
            constexpr int64_t kMpd = 86400LL * 1000000LL;
            int64_t end_us = vals[0].int_val * (vals[0].type == sql::Value::Type::DATE_VAL ? kMpd : 1);
            int64_t start_us = vals[1].int_val * (vals[1].type == sql::Value::Type::DATE_VAL ? kMpd : 1);
            int64_t diff = end_us - start_us;
            return sql::Value::make_interval(0, diff);
        }
        if (upper == "AT_TIME_ZONE" && vals.size() >= 2) {
            // AT_TIME_ZONE(timestamp, '+HH:MM' | '-HH:MM') -> TIMESTAMP_TZ
            if (vals[0].type != sql::Value::Type::TIMESTAMP_VAL) return sql::Value::make_null();
            std::string off = vals[1].to_string();
            int sign = 1; size_t i = 0;
            if (!off.empty() && (off[0] == '+' || off[0] == '-')) { if (off[0] == '-') sign = -1; i = 1; }
            int hh = 0, mm = 0;
            if (off.size() >= i + 2) hh = std::atoi(off.substr(i, 2).c_str());
            auto colon = off.find(':', i);
            if (colon != std::string::npos && colon + 2 < off.size())
                mm = std::atoi(off.substr(colon + 1, 2).c_str());
            int minutes = sign * (hh * 60 + mm);
            // Interpret the input as wall-clock in that zone — subtract the
            // offset to get UTC.
            int64_t utc_us = vals[0].int_val - (int64_t)minutes * 60 * 1000000LL;
            return sql::Value::make_timestamp_tz(utc_us, minutes);
        }

        // ─── Batch 6: UTF-8 helpers ────────────────────────────────────────
        if (upper == "OCTET_LENGTH" && vals.size() >= 1) {
            return sql::Value::make_int((int64_t)vals[0].to_string().size());
        }
        if ((upper == "CHAR_LENGTH" || upper == "CHARACTER_LENGTH") && vals.size() >= 1) {
            const std::string &s = vals[0].to_string();
            int64_t n = 0;
            for (size_t k = 0; k < s.size();) {
                unsigned char c = (unsigned char)s[k];
                size_t step = 1;
                if      ((c & 0x80) == 0x00) step = 1;
                else if ((c & 0xE0) == 0xC0) step = 2;
                else if ((c & 0xF0) == 0xE0) step = 3;
                else if ((c & 0xF8) == 0xF0) step = 4;
                k += step; n++;
            }
            return sql::Value::make_int(n);
        }
        if (upper == "IS_UTF8_VALID" && vals.size() >= 1) {
            const std::string &s = vals[0].to_string();
            bool ok = true;
            for (size_t k = 0; k < s.size();) {
                unsigned char c = (unsigned char)s[k];
                size_t need;
                if ((c & 0x80) == 0x00) need = 1;
                else if ((c & 0xE0) == 0xC0) need = 2;
                else if ((c & 0xF0) == 0xE0) need = 3;
                else if ((c & 0xF8) == 0xF0) need = 4;
                else { ok = false; break; }
                if (k + need > s.size()) { ok = false; break; }
                for (size_t j = 1; j < need; j++)
                    if (((unsigned char)s[k+j] & 0xC0) != 0x80) { ok = false; break; }
                if (!ok) break;
                k += need;
            }
            return sql::Value::make_bool(ok);
        }

        // ─── Batch 3: spatial functions (ST_*) ─────────────────────────────
        // Geometry value: canonical WKT in str_val; SRID in int_val; dim in int_val_2.
        // Supported WKT forms (Batch 3, fully closed):
        //   POINT(x y) / POINT Z(x y z) / POINT(x y z)
        //   LINESTRING(x y, x y, ...)
        //   POLYGON((outer ring), (hole 1), (hole 2), ...)
        //   MULTIPOINT((x y), (x y), ...) — or — MULTIPOINT(x y, x y, ...)
        //   MULTILINESTRING((x y, x y), (x y, x y), ...)
        //   MULTIPOLYGON(((outer),(hole)), ((outer)), ...)
        //   GEOMETRYCOLLECTION(POINT(...), LINESTRING(...), POLYGON(...))
        struct Pt { double x = 0, y = 0, z = 0; bool has_z = false; };
        struct Geom {
            enum Kind {
                POINT, LINESTRING, POLYGON,
                MULTIPOINT, MULTILINESTRING, MULTIPOLYGON,
                GEOMETRYCOLLECTION
            } kind = POINT;
            // POINT / LINESTRING use `verts`. POLYGON uses `rings` (rings[0]
            // = outer; rings[1..] = holes). MULTI* / COLLECTION use
            // `subgeoms` (a vector of full sub-Geom). `verts` may also be
            // populated for POLYGON as an alias of rings[0] so old code keeps
            // working.
            std::vector<Pt> verts;
            std::vector<std::vector<Pt>> rings;
            std::vector<Geom> subgeoms;
            int dim = 2;
        };
        auto skip_ws = [](const std::string &s, size_t &i){ while (i < s.size() && std::isspace((unsigned char)s[i])) i++; };
        auto parse_double = [&](const std::string &s, size_t &i, double &out) {
            skip_ws(s, i);
            size_t start = i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++;
            if (start == i) return false;
            try { out = std::stod(s.substr(start, i - start)); } catch (...) { return false; }
            return true;
        };
        // Lambda forward-declarations so the recursive parser can call itself.
        std::function<bool(const std::string &, size_t &, Geom &)> parse_geom_at;
        std::function<bool(const std::string &, size_t &, std::vector<Pt> &, int &)> parse_coord_seq;
        std::function<bool(const std::string &, size_t &, std::vector<std::vector<Pt>> &, int &)> parse_ring_list;

        auto match_kw_at = [&](const std::string &s, size_t &i, const char *kw) {
            size_t k = 0, save = i;
            while (kw[k] && i < s.size() &&
                   std::toupper((unsigned char)s[i]) == (unsigned char)kw[k]) { i++; k++; }
            if (kw[k] == 0) return true;
            i = save; return false;
        };

        // Parse "x y" or "x y z" — a single coordinate. Returns dim used.
        auto parse_one_coord = [&](const std::string &s, size_t &i, Pt &p, int &dim) {
            if (!parse_double(s, i, p.x)) return false;
            if (!parse_double(s, i, p.y)) return false;
            size_t save = i;
            double z;
            if (parse_double(s, i, z)) { p.z = z; p.has_z = true; if (dim < 3) dim = 3; }
            else i = save;
            return true;
        };

        // Parse "(x y, x y, x y)" — a single ring or linestring.
        parse_coord_seq = [&](const std::string &s, size_t &i, std::vector<Pt> &out, int &dim) {
            skip_ws(s, i);
            if (i >= s.size() || s[i] != '(') return false;
            i++;
            while (true) {
                Pt p;
                if (!parse_one_coord(s, i, p, dim)) return false;
                out.push_back(p);
                skip_ws(s, i);
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ')') { i++; return true; }
                return false;
            }
        };

        // Parse "((ring), (hole), (hole))" — a polygon ring list.
        parse_ring_list = [&](const std::string &s, size_t &i, std::vector<std::vector<Pt>> &rings, int &dim) {
            skip_ws(s, i);
            if (i >= s.size() || s[i] != '(') return false;
            i++;
            while (true) {
                std::vector<Pt> ring;
                if (!parse_coord_seq(s, i, ring, dim)) return false;
                rings.push_back(std::move(ring));
                skip_ws(s, i);
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ')') { i++; return true; }
                return false;
            }
        };

        parse_geom_at = [&](const std::string &wkt, size_t &i, Geom &g) -> bool {
            skip_ws(wkt, i);
            if (match_kw_at(wkt, i, "POINT")) {
                g.kind = Geom::POINT;
                skip_ws(wkt, i);
                bool zflag = match_kw_at(wkt, i, "Z");
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != '(') return false;
                i++;
                Pt p;
                if (!parse_one_coord(wkt, i, p, g.dim)) return false;
                if (zflag) { p.has_z = true; g.dim = 3; }
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != ')') return false;
                i++;
                g.verts.push_back(p);
                return true;
            }
            if (match_kw_at(wkt, i, "LINESTRING")) {
                g.kind = Geom::LINESTRING;
                return parse_coord_seq(wkt, i, g.verts, g.dim);
            }
            if (match_kw_at(wkt, i, "POLYGON")) {
                g.kind = Geom::POLYGON;
                if (!parse_ring_list(wkt, i, g.rings, g.dim)) return false;
                // Backward-compat alias: keep verts pointing at the outer ring.
                if (!g.rings.empty()) g.verts = g.rings[0];
                return true;
            }
            if (match_kw_at(wkt, i, "MULTIPOINT")) {
                g.kind = Geom::MULTIPOINT;
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != '(') return false;
                i++;
                while (true) {
                    Pt p;
                    Geom sub; sub.kind = Geom::POINT;
                    skip_ws(wkt, i);
                    // Accept both "(x y)" and bare "x y" inside the outer parens.
                    bool wrapped = (i < wkt.size() && wkt[i] == '(');
                    if (wrapped) i++;
                    if (!parse_one_coord(wkt, i, p, g.dim)) return false;
                    if (wrapped) {
                        skip_ws(wkt, i);
                        if (i >= wkt.size() || wkt[i] != ')') return false;
                        i++;
                    }
                    sub.verts.push_back(p);
                    g.subgeoms.push_back(std::move(sub));
                    skip_ws(wkt, i);
                    if (i < wkt.size() && wkt[i] == ',') { i++; continue; }
                    if (i < wkt.size() && wkt[i] == ')') { i++; return true; }
                    return false;
                }
            }
            if (match_kw_at(wkt, i, "MULTILINESTRING")) {
                g.kind = Geom::MULTILINESTRING;
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != '(') return false;
                i++;
                while (true) {
                    Geom sub; sub.kind = Geom::LINESTRING;
                    if (!parse_coord_seq(wkt, i, sub.verts, g.dim)) return false;
                    g.subgeoms.push_back(std::move(sub));
                    skip_ws(wkt, i);
                    if (i < wkt.size() && wkt[i] == ',') { i++; continue; }
                    if (i < wkt.size() && wkt[i] == ')') { i++; return true; }
                    return false;
                }
            }
            if (match_kw_at(wkt, i, "MULTIPOLYGON")) {
                g.kind = Geom::MULTIPOLYGON;
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != '(') return false;
                i++;
                while (true) {
                    Geom sub; sub.kind = Geom::POLYGON;
                    if (!parse_ring_list(wkt, i, sub.rings, g.dim)) return false;
                    if (!sub.rings.empty()) sub.verts = sub.rings[0];
                    g.subgeoms.push_back(std::move(sub));
                    skip_ws(wkt, i);
                    if (i < wkt.size() && wkt[i] == ',') { i++; continue; }
                    if (i < wkt.size() && wkt[i] == ')') { i++; return true; }
                    return false;
                }
            }
            if (match_kw_at(wkt, i, "GEOMETRYCOLLECTION")) {
                g.kind = Geom::GEOMETRYCOLLECTION;
                skip_ws(wkt, i);
                if (i >= wkt.size() || wkt[i] != '(') return false;
                i++;
                while (true) {
                    skip_ws(wkt, i);
                    Geom sub;
                    if (!parse_geom_at(wkt, i, sub)) return false;
                    if (sub.dim > g.dim) g.dim = sub.dim;
                    g.subgeoms.push_back(std::move(sub));
                    skip_ws(wkt, i);
                    if (i < wkt.size() && wkt[i] == ',') { i++; continue; }
                    if (i < wkt.size() && wkt[i] == ')') { i++; return true; }
                    return false;
                }
            }
            return false;
        };

        auto parse_wkt = [&](const std::string &wkt, Geom &g) -> bool {
            size_t i = 0;
            return parse_geom_at(wkt, i, g);
        };

        // Point-in-ring (ray casting).
        auto point_in_ring = [](const Pt &p, const std::vector<Pt> &ring) {
            bool inside = false;
            size_t n = ring.size();
            for (size_t a = 0, b = n - 1; a < n; b = a++) {
                if (((ring[a].y > p.y) != (ring[b].y > p.y)) &&
                    (p.x < (ring[b].x - ring[a].x) * (p.y - ring[a].y) / (ring[b].y - ring[a].y + 1e-30) + ring[a].x))
                    inside = !inside;
            }
            return inside;
        };

        // Point-in-polygon respecting holes: must be in outer ring AND in no
        // interior ring. Falls back to the legacy `verts` field when `rings`
        // isn't populated (which happens for old POLYGON values that pre-date
        // the holes-aware parser; verts == outer ring in that case).
        auto point_in_polygon = [&](const Pt &p, const Geom &poly) {
            if (poly.kind != Geom::POLYGON) return false;
            const std::vector<Pt> &outer = !poly.rings.empty() ? poly.rings[0] : poly.verts;
            if (outer.empty()) return false;
            if (!point_in_ring(p, outer)) return false;
            for (size_t r = 1; r < poly.rings.size(); r++) {
                if (point_in_ring(p, poly.rings[r])) return false; // inside a hole
            }
            return true;
        };

        // Legacy ring-only point-in-polygon, kept for code that passes a raw
        // outer ring (no holes). Forwards to point_in_ring.
        auto point_in_outer_ring = [&](const Pt &p, const std::vector<Pt> &ring) {
            return point_in_ring(p, ring);
        };
        (void)point_in_outer_ring;
        auto as_geom = [&](const sql::Value &v, Geom &g) {
            if (v.type == sql::Value::Type::GEOMETRY || v.type == sql::Value::Type::STRING)
                return parse_wkt(v.str_val.empty() ? v.to_string() : v.str_val, g);
            return false;
        };

        if (upper == "ST_GEOMFROMTEXT" && vals.size() >= 1) {
            Geom g;
            std::string s = vals[0].to_string();
            if (!parse_wkt(s, g)) return sql::Value::make_null();
            int srid = (vals.size() >= 2) ? (int)vals[1].int_val : 0;
            return sql::Value::make_geometry(s, srid, g.dim);
        }
        if (upper == "ST_ASTEXT" && vals.size() >= 1) {
            return sql::Value::make_string(vals[0].to_string());
        }
        if (upper == "ST_SRID" && vals.size() >= 1) {
            return sql::Value::make_int(vals[0].int_val);
        }
        if (upper == "ST_MAKEPOINT" && vals.size() >= 2) {
            double x = to_double(vals[0]), y = to_double(vals[1]);
            if (vals.size() >= 3) {
                double z = to_double(vals[2]);
                std::ostringstream oss; oss.precision(15);
                oss << "POINT Z(" << x << " " << y << " " << z << ")";
                return sql::Value::make_geometry(oss.str(), 0, 3);
            }
            std::ostringstream oss; oss.precision(15);
            oss << "POINT(" << x << " " << y << ")";
            return sql::Value::make_geometry(oss.str(), 0, 2);
        }
        if (upper == "ST_X" && vals.size() >= 1) {
            Geom g; if (!as_geom(vals[0], g) || g.kind != Geom::POINT) return sql::Value::make_null();
            return sql::Value::make_float(g.verts[0].x);
        }
        if (upper == "ST_Y" && vals.size() >= 1) {
            Geom g; if (!as_geom(vals[0], g) || g.kind != Geom::POINT) return sql::Value::make_null();
            return sql::Value::make_float(g.verts[0].y);
        }
        if (upper == "ST_Z" && vals.size() >= 1) {
            Geom g; if (!as_geom(vals[0], g) || g.kind != Geom::POINT || !g.verts[0].has_z) return sql::Value::make_null();
            return sql::Value::make_float(g.verts[0].z);
        }
        if (upper == "ST_DISTANCE" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind != Geom::POINT || b.kind != Geom::POINT) return sql::Value::make_null();
            double dx = a.verts[0].x - b.verts[0].x;
            double dy = a.verts[0].y - b.verts[0].y;
            return sql::Value::make_float(std::sqrt(dx*dx + dy*dy));
        }
        if (upper == "ST_3DDISTANCE" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind != Geom::POINT || b.kind != Geom::POINT) return sql::Value::make_null();
            double dx = a.verts[0].x - b.verts[0].x;
            double dy = a.verts[0].y - b.verts[0].y;
            double dz = a.verts[0].z - b.verts[0].z;
            return sql::Value::make_float(std::sqrt(dx*dx + dy*dy + dz*dz));
        }
        if (upper == "ST_DWITHIN" && vals.size() >= 3) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind != Geom::POINT || b.kind != Geom::POINT) return sql::Value::make_null();
            double dx = a.verts[0].x - b.verts[0].x;
            double dy = a.verts[0].y - b.verts[0].y;
            return sql::Value::make_bool(std::sqrt(dx*dx + dy*dy) <= to_double(vals[2]));
        }
        if (upper == "ST_EQUALS" && vals.size() >= 2) {
            return sql::Value::make_bool(vals[0].to_string() == vals[1].to_string());
        }
        // Standard 2D segment-intersection test using the orientation
        // (counter-clockwise) sign trick. Returns true when segment p1-p2
        // crosses segment p3-p4, including endpoint-on-segment touches.
        auto seg_intersects = [](const Pt &p1, const Pt &p2,
                                  const Pt &p3, const Pt &p4) {
            auto orient = [](const Pt &a, const Pt &b, const Pt &c) {
                double v = (b.y - a.y) * (c.x - b.x) - (b.x - a.x) * (c.y - b.y);
                if (v > 0) return 1;
                if (v < 0) return -1;
                return 0;
            };
            auto on_seg = [](const Pt &a, const Pt &b, const Pt &c) {
                return std::min(a.x, c.x) <= b.x && b.x <= std::max(a.x, c.x) &&
                       std::min(a.y, c.y) <= b.y && b.y <= std::max(a.y, c.y);
            };
            int o1 = orient(p1, p2, p3);
            int o2 = orient(p1, p2, p4);
            int o3 = orient(p3, p4, p1);
            int o4 = orient(p3, p4, p2);
            if (o1 != o2 && o3 != o4) return true;
            // Collinear touch cases.
            if (o1 == 0 && on_seg(p1, p3, p2)) return true;
            if (o2 == 0 && on_seg(p1, p4, p2)) return true;
            if (o3 == 0 && on_seg(p3, p1, p4)) return true;
            if (o4 == 0 && on_seg(p3, p2, p4)) return true;
            return false;
        };
        auto linestrings_intersect = [&](const std::vector<Pt> &a, const std::vector<Pt> &b) {
            if (a.size() < 2 || b.size() < 2) return false;
            for (size_t i = 0; i + 1 < a.size(); i++) {
                for (size_t j = 0; j + 1 < b.size(); j++) {
                    if (seg_intersects(a[i], a[i+1], b[j], b[j+1])) return true;
                }
            }
            return false;
        };
        auto linestring_intersects_polygon = [&](const std::vector<Pt> &line, const Geom &poly) {
            // True if any vertex is inside the polygon OR if any line segment
            // crosses any ring (outer or hole boundary).
            for (auto &p : line) if (point_in_polygon(p, poly)) return true;
            const auto rings_to_check = poly.rings.empty()
                ? std::vector<std::vector<Pt>>{poly.verts} : poly.rings;
            for (auto &ring : rings_to_check) {
                if (ring.size() < 2) continue;
                for (size_t i = 0; i + 1 < line.size(); i++) {
                    for (size_t j = 0; j + 1 < ring.size(); j++) {
                        if (seg_intersects(line[i], line[i+1], ring[j], ring[j+1])) return true;
                    }
                }
            }
            return false;
        };

        // Predicate dispatcher that handles MULTI*/COLLECTION uniformly: an
        // intersection of a multi-geom with X is the OR over each sub-geom
        // intersecting X. Same shape for contains/within with AND semantics.
        std::function<bool(const Geom &, const Geom &)> intersects_2geom;
        intersects_2geom = [&](const Geom &a, const Geom &b) -> bool {
            // Unwrap multis: if either side is a multi, recurse over its subs.
            if (a.kind == Geom::MULTIPOINT || a.kind == Geom::MULTILINESTRING ||
                a.kind == Geom::MULTIPOLYGON || a.kind == Geom::GEOMETRYCOLLECTION) {
                for (auto &s : a.subgeoms) if (intersects_2geom(s, b)) return true;
                return false;
            }
            if (b.kind == Geom::MULTIPOINT || b.kind == Geom::MULTILINESTRING ||
                b.kind == Geom::MULTIPOLYGON || b.kind == Geom::GEOMETRYCOLLECTION) {
                for (auto &s : b.subgeoms) if (intersects_2geom(a, s)) return true;
                return false;
            }
            // Single-geom cases.
            if (a.kind == Geom::POINT && b.kind == Geom::POINT)
                return a.verts[0].x == b.verts[0].x && a.verts[0].y == b.verts[0].y;
            if (a.kind == Geom::POINT && b.kind == Geom::POLYGON)
                return point_in_polygon(a.verts[0], b);
            if (a.kind == Geom::POLYGON && b.kind == Geom::POINT)
                return point_in_polygon(b.verts[0], a);
            if (a.kind == Geom::POLYGON && b.kind == Geom::POLYGON) {
                const auto &outA = a.rings.empty() ? a.verts : a.rings[0];
                const auto &outB = b.rings.empty() ? b.verts : b.rings[0];
                for (auto &p : outA) if (point_in_polygon(p, b)) return true;
                for (auto &p : outB) if (point_in_polygon(p, a)) return true;
                return false;
            }
            // LINESTRING-LINESTRING: proper segment-segment intersection.
            if (a.kind == Geom::LINESTRING && b.kind == Geom::LINESTRING)
                return linestrings_intersect(a.verts, b.verts);
            // LINESTRING-POLYGON / POLYGON-LINESTRING: a vertex inside OR a
            // segment crossing any ring counts.
            if (a.kind == Geom::LINESTRING && b.kind == Geom::POLYGON)
                return linestring_intersects_polygon(a.verts, b);
            if (a.kind == Geom::POLYGON && b.kind == Geom::LINESTRING)
                return linestring_intersects_polygon(b.verts, a);
            // POINT-LINESTRING: point lies on any segment.
            if (a.kind == Geom::POINT && b.kind == Geom::LINESTRING) {
                if (b.verts.size() < 2) return false;
                for (size_t j = 0; j + 1 < b.verts.size(); j++) {
                    // Degenerate "segment" with a == b: check exact-point equality.
                    if (seg_intersects(a.verts[0], a.verts[0], b.verts[j], b.verts[j+1]))
                        return true;
                }
                return false;
            }
            if (a.kind == Geom::LINESTRING && b.kind == Geom::POINT) {
                if (a.verts.size() < 2) return false;
                for (size_t j = 0; j + 1 < a.verts.size(); j++) {
                    if (seg_intersects(b.verts[0], b.verts[0], a.verts[j], a.verts[j+1]))
                        return true;
                }
                return false;
            }
            return false;
        };

        std::function<bool(const Geom &, const Geom &)> contains_2geom;
        contains_2geom = [&](const Geom &a, const Geom &b) -> bool {
            // For MULTI on the contained side: contained iff every sub is contained.
            if (b.kind == Geom::MULTIPOINT || b.kind == Geom::MULTILINESTRING ||
                b.kind == Geom::MULTIPOLYGON || b.kind == Geom::GEOMETRYCOLLECTION) {
                for (auto &s : b.subgeoms) if (!contains_2geom(a, s)) return false;
                return true;
            }
            // For MULTI on the container side: contained iff any sub contains.
            if (a.kind == Geom::MULTIPOINT || a.kind == Geom::MULTILINESTRING ||
                a.kind == Geom::MULTIPOLYGON || a.kind == Geom::GEOMETRYCOLLECTION) {
                for (auto &s : a.subgeoms) if (contains_2geom(s, b)) return true;
                return false;
            }
            if (a.kind == Geom::POLYGON && b.kind == Geom::POINT)
                return point_in_polygon(b.verts[0], a);
            if (a.kind == Geom::POLYGON && b.kind == Geom::POLYGON) {
                const auto &outB = b.rings.empty() ? b.verts : b.rings[0];
                for (auto &p : outB) if (!point_in_polygon(p, a)) return false;
                return true;
            }
            if (a.kind == Geom::POLYGON && b.kind == Geom::LINESTRING) {
                for (auto &p : b.verts) if (!point_in_polygon(p, a)) return false;
                return true;
            }
            return false;
        };

        if (upper == "ST_INTERSECTS" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            return sql::Value::make_bool(intersects_2geom(a, b));
        }
        if (upper == "ST_CONTAINS" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            return sql::Value::make_bool(contains_2geom(a, b));
        }
        if (upper == "ST_WITHIN" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            return sql::Value::make_bool(contains_2geom(b, a));
        }
        // ST_3DIntersects: only true if same point in 3D (or 2D match when no Z).
        if (upper == "ST_3DINTERSECTS" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind == Geom::POINT && b.kind == Geom::POINT) {
                return sql::Value::make_bool(
                    a.verts[0].x == b.verts[0].x &&
                    a.verts[0].y == b.verts[0].y &&
                    a.verts[0].z == b.verts[0].z);
            }
            return sql::Value::make_bool(false);
        }
        if (upper == "ST_3DDWITHIN" && vals.size() >= 3) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind != Geom::POINT || b.kind != Geom::POINT) return sql::Value::make_null();
            double dx = a.verts[0].x - b.verts[0].x;
            double dy = a.verts[0].y - b.verts[0].y;
            double dz = a.verts[0].z - b.verts[0].z;
            return sql::Value::make_bool(std::sqrt(dx*dx + dy*dy + dz*dz) <= to_double(vals[2]));
        }
        // ST_Touches / ST_Crosses / ST_Overlaps: simplified MVP semantics.
        // - ST_Touches: shared boundary point only (point-on-polygon-edge).
        // - ST_Crosses: cardinality reduction — point inside polygon = false.
        // - ST_Overlaps: same dimension, partial overlap (polygon-polygon).
        if (upper == "ST_TOUCHES" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind == Geom::POINT && b.kind == Geom::POINT)
                return sql::Value::make_bool(a.verts[0].x == b.verts[0].x &&
                                             a.verts[0].y == b.verts[0].y);
            return sql::Value::make_bool(false);
        }
        if (upper == "ST_CROSSES" && vals.size() >= 2) {
            // Two linestrings cross when at least one pair of segments
            // properly intersects. (Touching at a shared endpoint alone is
            // not a CROSS per OGC; for the MVP we keep the broader notion
            // since the distinction is rarely material.)
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind == Geom::LINESTRING && b.kind == Geom::LINESTRING)
                return sql::Value::make_bool(linestrings_intersect(a.verts, b.verts));
            return sql::Value::make_bool(false);
        }
        if (upper == "ST_OVERLAPS" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            // Same-dim overlap: polygons whose ring vertices straddle the other.
            if (a.kind == Geom::POLYGON && b.kind == Geom::POLYGON) {
                const auto &outA = a.rings.empty() ? a.verts : a.rings[0];
                const auto &outB = b.rings.empty() ? b.verts : b.rings[0];
                bool any_in = false, any_out = false;
                for (auto &p : outA) {
                    if (point_in_polygon(p, b)) any_in = true; else any_out = true;
                    if (any_in && any_out) return sql::Value::make_bool(true);
                }
                for (auto &p : outB) {
                    if (point_in_polygon(p, a)) any_in = true; else any_out = true;
                    if (any_in && any_out) return sql::Value::make_bool(true);
                }
            }
            return sql::Value::make_bool(false);
        }
        // Haversine great-circle distance for GEOGRAPHY-typed points.
        // ST_GEOGDISTANCE(lonlat_a, lonlat_b) — returns meters on a spherical Earth.
        if (upper == "ST_GEOGDISTANCE" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            if (a.kind != Geom::POINT || b.kind != Geom::POINT) return sql::Value::make_null();
            const double R = 6371000.0; // mean Earth radius in meters
            auto rad = [](double d) { return d * M_PI / 180.0; };
            double lat1 = rad(a.verts[0].y), lat2 = rad(b.verts[0].y);
            double dlat = lat2 - lat1;
            double dlon = rad(b.verts[0].x) - rad(a.verts[0].x);
            double h = std::sin(dlat/2)*std::sin(dlat/2)
                     + std::cos(lat1)*std::cos(lat2)*std::sin(dlon/2)*std::sin(dlon/2);
            double c = 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
            return sql::Value::make_float(R * c);
        }
        if (upper == "ST_DISJOINT" && vals.size() >= 2) {
            Geom a, b;
            if (!as_geom(vals[0], a) || !as_geom(vals[1], b)) return sql::Value::make_null();
            return sql::Value::make_bool(!intersects_2geom(a, b));
        }

        // ─── Inspection / accessor functions (OGC SF-SQL) ────────────────
        auto geom_kind_name = [](Geom::Kind k) -> const char * {
            switch (k) {
                case Geom::POINT:               return "POINT";
                case Geom::LINESTRING:          return "LINESTRING";
                case Geom::POLYGON:             return "POLYGON";
                case Geom::MULTIPOINT:          return "MULTIPOINT";
                case Geom::MULTILINESTRING:     return "MULTILINESTRING";
                case Geom::MULTIPOLYGON:        return "MULTIPOLYGON";
                case Geom::GEOMETRYCOLLECTION:  return "GEOMETRYCOLLECTION";
            }
            return "UNKNOWN";
        };
        if (upper == "ST_GEOMETRYTYPE" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g)) return sql::Value::make_null();
            return sql::Value::make_string(std::string("ST_") + geom_kind_name(g.kind));
        }
        if (upper == "ST_NUMGEOMETRIES" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g)) return sql::Value::make_null();
            if (g.kind == Geom::MULTIPOINT || g.kind == Geom::MULTILINESTRING ||
                g.kind == Geom::MULTIPOLYGON || g.kind == Geom::GEOMETRYCOLLECTION)
                return sql::Value::make_int((int64_t)g.subgeoms.size());
            return sql::Value::make_int(1);
        }
        if (upper == "ST_GEOMETRYN" && vals.size() >= 2) {
            Geom g;
            if (!as_geom(vals[0], g)) return sql::Value::make_null();
            int64_t n = vals[1].int_val - 1; // 1-based
            auto emit = [&](const Geom &sub) -> sql::Value {
                std::ostringstream oss; oss.precision(15);
                if (sub.kind == Geom::POINT) {
                    oss << "POINT(" << sub.verts[0].x << " " << sub.verts[0].y << ")";
                } else if (sub.kind == Geom::LINESTRING) {
                    oss << "LINESTRING(";
                    for (size_t k = 0; k < sub.verts.size(); k++) {
                        if (k) oss << ",";
                        oss << sub.verts[k].x << " " << sub.verts[k].y;
                    }
                    oss << ")";
                } else if (sub.kind == Geom::POLYGON) {
                    oss << "POLYGON(";
                    const auto &rs = sub.rings.empty()
                        ? std::vector<std::vector<Pt>>{sub.verts} : sub.rings;
                    for (size_t r = 0; r < rs.size(); r++) {
                        if (r) oss << ",";
                        oss << "(";
                        for (size_t k = 0; k < rs[r].size(); k++) {
                            if (k) oss << ",";
                            oss << rs[r][k].x << " " << rs[r][k].y;
                        }
                        oss << ")";
                    }
                    oss << ")";
                }
                return sql::Value::make_geometry(oss.str(), 0, sub.dim);
            };
            if (g.kind == Geom::MULTIPOINT || g.kind == Geom::MULTILINESTRING ||
                g.kind == Geom::MULTIPOLYGON || g.kind == Geom::GEOMETRYCOLLECTION) {
                if (n < 0 || (size_t)n >= g.subgeoms.size()) return sql::Value::make_null();
                return emit(g.subgeoms[(size_t)n]);
            }
            // Single geom: index 1 returns itself.
            if (n == 0) return emit(g);
            return sql::Value::make_null();
        }
        if (upper == "ST_NUMPOINTS" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::LINESTRING) return sql::Value::make_null();
            return sql::Value::make_int((int64_t)g.verts.size());
        }
        if (upper == "ST_STARTPOINT" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::LINESTRING || g.verts.empty())
                return sql::Value::make_null();
            std::ostringstream oss; oss.precision(15);
            oss << "POINT(" << g.verts.front().x << " " << g.verts.front().y << ")";
            return sql::Value::make_geometry(oss.str(), 0, g.dim);
        }
        if (upper == "ST_ENDPOINT" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::LINESTRING || g.verts.empty())
                return sql::Value::make_null();
            std::ostringstream oss; oss.precision(15);
            oss << "POINT(" << g.verts.back().x << " " << g.verts.back().y << ")";
            return sql::Value::make_geometry(oss.str(), 0, g.dim);
        }
        if (upper == "ST_NUMINTERIORRINGS" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::POLYGON) return sql::Value::make_null();
            if (g.rings.size() <= 1) return sql::Value::make_int(0);
            return sql::Value::make_int((int64_t)(g.rings.size() - 1));
        }
        if (upper == "ST_EXTERIORRING" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::POLYGON) return sql::Value::make_null();
            const auto &outer = g.rings.empty() ? g.verts : g.rings[0];
            if (outer.empty()) return sql::Value::make_null();
            std::ostringstream oss; oss.precision(15);
            oss << "LINESTRING(";
            for (size_t k = 0; k < outer.size(); k++) {
                if (k) oss << ",";
                oss << outer[k].x << " " << outer[k].y;
            }
            oss << ")";
            return sql::Value::make_geometry(oss.str(), 0, g.dim);
        }
        if (upper == "ST_INTERIORRINGN" && vals.size() >= 2) {
            Geom g;
            if (!as_geom(vals[0], g) || g.kind != Geom::POLYGON) return sql::Value::make_null();
            int64_t n = vals[1].int_val; // 1-based, ring 0 is exterior
            if (n < 1 || (size_t)n >= g.rings.size()) return sql::Value::make_null();
            const auto &ring = g.rings[(size_t)n];
            std::ostringstream oss; oss.precision(15);
            oss << "LINESTRING(";
            for (size_t k = 0; k < ring.size(); k++) {
                if (k) oss << ",";
                oss << ring[k].x << " " << ring[k].y;
            }
            oss << ")";
            return sql::Value::make_geometry(oss.str(), 0, g.dim);
        }
        if (upper == "ST_ISVALID" && vals.size() >= 1) {
            Geom g;
            return sql::Value::make_bool(as_geom(vals[0], g));
        }
        if (upper == "ST_ISEMPTY" && vals.size() >= 1) {
            Geom g;
            if (!as_geom(vals[0], g)) return sql::Value::make_null();
            if (g.kind == Geom::POINT)      return sql::Value::make_bool(g.verts.empty());
            if (g.kind == Geom::LINESTRING) return sql::Value::make_bool(g.verts.empty());
            if (g.kind == Geom::POLYGON)
                return sql::Value::make_bool(g.rings.empty() && g.verts.empty());
            return sql::Value::make_bool(g.subgeoms.empty());
        }

        // ═══════════════════════════════════════════════════════════════
        // Extended function library: Excel / Access / Tableau / Analytics
        // ═══════════════════════════════════════════════════════════════

        // Helper to extract numeric value from a Value
        auto as_double = [](const sql::Value &v) -> double {
            if (v.type == sql::Value::Type::INT64) return (double)v.int_val;
            if (v.type == sql::Value::Type::FLOAT64) return v.float_val;
            if (v.type == sql::Value::Type::BOOL) return v.bool_val ? 1.0 : 0.0;
            if (v.type == sql::Value::Type::STRING) {
                try { return std::stod(v.str_val); } catch (...) { return 0.0; }
            }
            return 0.0;
        };

        // ─── Trigonometric functions ────────────────────────────────
        if (upper == "SIN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::sin(as_double(vals[0])));
        }
        if (upper == "COS" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::cos(as_double(vals[0])));
        }
        if (upper == "TAN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::tan(as_double(vals[0])));
        }
        if (upper == "ASIN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::asin(as_double(vals[0])));
        }
        if (upper == "ACOS" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::acos(as_double(vals[0])));
        }
        if (upper == "ATAN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::atan(as_double(vals[0])));
        }
        if (upper == "ATAN2" && vals.size() >= 2) {
            if (vals[0].is_null() || vals[1].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::atan2(as_double(vals[0]), as_double(vals[1])));
        }
        if (upper == "SINH" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::sinh(as_double(vals[0])));
        }
        if (upper == "COSH" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::cosh(as_double(vals[0])));
        }
        if (upper == "TANH" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::tanh(as_double(vals[0])));
        }
        if (upper == "DEGREES" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(as_double(vals[0]) * 180.0 / M_PI);
        }
        if (upper == "RADIANS" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(as_double(vals[0]) * M_PI / 180.0);
        }
        if (upper == "PI") {
            return sql::Value::make_float(M_PI);
        }

        // ─── Math / Excel / Access numeric functions ────────────────
        if (upper == "LOG" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            double x = as_double(vals[0]);
            if (vals.size() >= 2) {
                double base = as_double(vals[1]);
                return sql::Value::make_float(std::log(x) / std::log(base));
            }
            return sql::Value::make_float(std::log(x)); // natural log
        }
        if (upper == "LOG2" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::log2(as_double(vals[0])));
        }
        if (upper == "LOG10" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::log10(as_double(vals[0])));
        }
        if (upper == "LN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::log(as_double(vals[0])));
        }
        if (upper == "EXP" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::exp(as_double(vals[0])));
        }
        if (upper == "SIGN" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            double x = as_double(vals[0]);
            return sql::Value::make_int(x > 0 ? 1 : (x < 0 ? -1 : 0));
        }
        if (upper == "TRUNC" || upper == "TRUNCATE") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_null();
            double x = as_double(vals[0]);
            int digits = (vals.size() >= 2) ? (int)as_double(vals[1]) : 0;
            double factor = std::pow(10.0, digits);
            return sql::Value::make_float(std::trunc(x * factor) / factor);
        }
        if ((upper == "RAND" || upper == "RANDOM") && vals.empty()) {
            return sql::Value::make_float((double)std::rand() / RAND_MAX);
        }
        if (upper == "RANDBETWEEN" && vals.size() >= 2) {
            int64_t lo = (int64_t)as_double(vals[0]);
            int64_t hi = (int64_t)as_double(vals[1]);
            if (hi < lo) std::swap(lo, hi);
            return sql::Value::make_int(lo + (std::rand() % (hi - lo + 1)));
        }
        if (upper == "GCD" && vals.size() >= 2) {
            auto gcd_fn = [](int64_t a, int64_t b) -> int64_t {
                a = std::abs(a); b = std::abs(b);
                while (b) { int64_t t = b; b = a % b; a = t; }
                return a;
            };
            return sql::Value::make_int(gcd_fn((int64_t)as_double(vals[0]), (int64_t)as_double(vals[1])));
        }
        if (upper == "LCM" && vals.size() >= 2) {
            auto gcd_fn = [](int64_t a, int64_t b) -> int64_t {
                a = std::abs(a); b = std::abs(b);
                while (b) { int64_t t = b; b = a % b; a = t; }
                return a;
            };
            int64_t a = (int64_t)as_double(vals[0]), b = (int64_t)as_double(vals[1]);
            int64_t g = gcd_fn(a, b);
            return sql::Value::make_int(g == 0 ? 0 : std::abs(a / g * b));
        }
        if (upper == "FACTORIAL" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            int64_t n = (int64_t)as_double(vals[0]);
            if (n < 0 || n > 20) return sql::Value::make_null();
            int64_t result = 1;
            for (int64_t i = 2; i <= n; i++) result *= i;
            return sql::Value::make_int(result);
        }
        if (upper == "COMBIN" && vals.size() >= 2) {
            int64_t n = (int64_t)as_double(vals[0]), k = (int64_t)as_double(vals[1]);
            if (k < 0 || k > n || n < 0) return sql::Value::make_int(0);
            if (k > n - k) k = n - k;
            int64_t result = 1;
            for (int64_t i = 0; i < k; i++) { result = result * (n - i) / (i + 1); }
            return sql::Value::make_int(result);
        }
        if (upper == "PERMUT" && vals.size() >= 2) {
            int64_t n = (int64_t)as_double(vals[0]), k = (int64_t)as_double(vals[1]);
            if (k < 0 || k > n || n < 0) return sql::Value::make_int(0);
            int64_t result = 1;
            for (int64_t i = 0; i < k; i++) result *= (n - i);
            return sql::Value::make_int(result);
        }
        if (upper == "EVEN" && vals.size() >= 1) {
            double x = as_double(vals[0]);
            int64_t c = (int64_t)std::ceil(std::abs(x));
            if (c % 2 != 0) c++;
            return sql::Value::make_int(x >= 0 ? c : -c);
        }
        if (upper == "ODD" && vals.size() >= 1) {
            double x = as_double(vals[0]);
            int64_t c = (int64_t)std::ceil(std::abs(x));
            if (c % 2 == 0) c++;
            return sql::Value::make_int(x >= 0 ? c : -c);
        }
        if (upper == "CBRT" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(std::cbrt(as_double(vals[0])));
        }
        if (upper == "HYPOT" && vals.size() >= 2) {
            return sql::Value::make_float(std::hypot(as_double(vals[0]), as_double(vals[1])));
        }

        // ─── Boolean / Logical functions (Excel/Tableau) ────────────
        if (upper == "IIF" && vals.size() >= 3) {
            // IIF(condition, true_val, false_val) — Access/Tableau
            bool cond = false;
            auto &cv = vals[0];
            if (cv.type == sql::Value::Type::BOOL) cond = cv.bool_val;
            else if (cv.type == sql::Value::Type::INT64) cond = (cv.int_val != 0);
            else if (!cv.is_null()) cond = true;
            return cond ? vals[1] : vals[2];
        }
        if (upper == "IF" && vals.size() >= 3) {
            // Same as IIF
            bool cond = false;
            auto &cv = vals[0];
            if (cv.type == sql::Value::Type::BOOL) cond = cv.bool_val;
            else if (cv.type == sql::Value::Type::INT64) cond = (cv.int_val != 0);
            else if (!cv.is_null()) cond = true;
            return cond ? vals[1] : vals[2];
        }
        if (upper == "IFNULL" && vals.size() >= 2) {
            return vals[0].is_null() ? vals[1] : vals[0];
        }
        if (upper == "NVL" && vals.size() >= 2) {
            return vals[0].is_null() ? vals[1] : vals[0];
        }
        if (upper == "NVL2" && vals.size() >= 3) {
            return vals[0].is_null() ? vals[2] : vals[1];
        }
        if (upper == "ISNULL" && vals.size() >= 1) {
            return sql::Value::make_bool(vals[0].is_null());
        }
        if (upper == "ISBLANK" && vals.size() >= 1) {
            return sql::Value::make_bool(vals[0].is_null() || vals[0].to_string().empty());
        }
        if (upper == "ISNUMBER" || upper == "ISNUMERIC") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_bool(false);
            auto &v = vals[0];
            if (v.type == sql::Value::Type::INT64 || v.type == sql::Value::Type::FLOAT64)
                return sql::Value::make_bool(true);
            if (v.type == sql::Value::Type::STRING) {
                try { std::stod(v.str_val); return sql::Value::make_bool(true); }
                catch (...) { return sql::Value::make_bool(false); }
            }
            return sql::Value::make_bool(false);
        }
        if (upper == "ISTEXT" && vals.size() >= 1) {
            return sql::Value::make_bool(!vals[0].is_null() && vals[0].type == sql::Value::Type::STRING);
        }
        if (upper == "ISLOGICAL" && vals.size() >= 1) {
            return sql::Value::make_bool(vals[0].type == sql::Value::Type::BOOL);
        }
        if (upper == "ISERROR" && vals.size() >= 1) {
            return sql::Value::make_bool(false); // No error values in TDB
        }
        if (upper == "CHOOSE" && vals.size() >= 2) {
            int idx = (int)as_double(vals[0]);
            if (idx >= 1 && idx < (int)vals.size()) return vals[idx];
            return sql::Value::make_null();
        }
        if (upper == "SWITCH" && vals.size() >= 3) {
            // SWITCH(expr, val1, result1, val2, result2, ..., [default])
            for (size_t i = 1; i + 1 < vals.size(); i += 2) {
                if (vals[0].compare(vals[i]) == 0) return vals[i + 1];
            }
            if (vals.size() % 2 == 0) return vals.back(); // default
            return sql::Value::make_null();
        }

        // ─── Text / String functions (Excel/Access/Tableau) ─────────
        if (upper == "MID" && vals.size() >= 3) {
            // MID(text, start, length) — Excel/Access (1-based)
            if (vals[0].is_null()) return sql::Value::make_null();
            std::string s = vals[0].to_string();
            int start = (int)as_double(vals[1]) - 1;
            int len = (int)as_double(vals[2]);
            if (start < 0) start = 0;
            if (start >= (int)s.size()) return sql::Value::make_string("");
            return sql::Value::make_string(s.substr(start, len));
        }
        if (upper == "FIND" || upper == "SEARCH") {
            // FIND(find_text, within_text[, start_pos]) — 1-based
            if (vals.size() < 2) return sql::Value::make_null();
            std::string needle = vals[0].to_string();
            std::string haystack = vals[1].to_string();
            if (upper == "SEARCH") {
                for (auto &c : needle) c = (char)std::tolower((unsigned char)c);
                for (auto &c : haystack) c = (char)std::tolower((unsigned char)c);
            }
            int start = (vals.size() >= 3) ? (int)as_double(vals[2]) - 1 : 0;
            if (start < 0) start = 0;
            auto pos = haystack.find(needle, start);
            if (pos == std::string::npos) return sql::Value::make_int(0);
            return sql::Value::make_int((int64_t)pos + 1);
        }
        if (upper == "SUBSTITUTE" && vals.size() >= 3) {
            std::string text = vals[0].to_string();
            std::string old_text = vals[1].to_string();
            std::string new_text = vals[2].to_string();
            if (old_text.empty()) return sql::Value::make_string(text);
            std::string result;
            size_t pos = 0;
            while ((pos = text.find(old_text)) != std::string::npos) {
                result += text.substr(0, pos) + new_text;
                text = text.substr(pos + old_text.size());
            }
            result += text;
            return sql::Value::make_string(result);
        }
        if (upper == "EXACT" && vals.size() >= 2) {
            return sql::Value::make_bool(vals[0].to_string() == vals[1].to_string());
        }
        if (upper == "REPT" && vals.size() >= 2) {
            std::string s = vals[0].to_string();
            int n = (int)as_double(vals[1]);
            std::string result;
            for (int i = 0; i < n && result.size() < 32768; i++) result += s;
            return sql::Value::make_string(result);
        }
        if (upper == "PROPER" && vals.size() >= 1) {
            // Capitalize first letter of each word
            std::string s = vals[0].to_string();
            bool cap_next = true;
            for (auto &c : s) {
                if (std::isspace((unsigned char)c) || c == '-' || c == '\'') { cap_next = true; }
                else if (cap_next) { c = (char)std::toupper((unsigned char)c); cap_next = false; }
                else { c = (char)std::tolower((unsigned char)c); }
            }
            return sql::Value::make_string(s);
        }
        if (upper == "INITCAP" && vals.size() >= 1) {
            // Same as PROPER — PostgreSQL name
            std::string s = vals[0].to_string();
            bool cap_next = true;
            for (auto &c : s) {
                if (!std::isalnum((unsigned char)c)) { cap_next = true; }
                else if (cap_next) { c = (char)std::toupper((unsigned char)c); cap_next = false; }
                else { c = (char)std::tolower((unsigned char)c); }
            }
            return sql::Value::make_string(s);
        }
        if (upper == "LTRIM" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            s.erase(0, s.find_first_not_of(" \t\n\r"));
            return sql::Value::make_string(s);
        }
        if (upper == "RTRIM" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            auto pos = s.find_last_not_of(" \t\n\r");
            if (pos != std::string::npos) s.erase(pos + 1);
            else s.clear();
            return sql::Value::make_string(s);
        }
        if (upper == "SPLIT_PART" && vals.size() >= 3) {
            std::string s = vals[0].to_string();
            std::string delim = vals[1].to_string();
            int part = (int)as_double(vals[2]);
            if (delim.empty() || part < 1) return sql::Value::make_null();
            int cur = 1;
            size_t pos = 0;
            while (cur < part) {
                pos = s.find(delim, pos);
                if (pos == std::string::npos) return sql::Value::make_string("");
                pos += delim.size();
                cur++;
            }
            auto end = s.find(delim, pos);
            return sql::Value::make_string(s.substr(pos, end == std::string::npos ? end : end - pos));
        }
        if (upper == "TRANSLATE" && vals.size() >= 3) {
            std::string s = vals[0].to_string();
            std::string from = vals[1].to_string();
            std::string to = vals[2].to_string();
            std::string result;
            for (char c : s) {
                auto pos = from.find(c);
                if (pos == std::string::npos) result += c;
                else if (pos < to.size()) result += to[pos];
                // else: character removed (no replacement)
            }
            return sql::Value::make_string(result);
        }
        if (upper == "ASCII" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            return sql::Value::make_int(s.empty() ? 0 : (int64_t)(unsigned char)s[0]);
        }
        if (upper == "CHR" || upper == "CHAR") {
            if (vals.empty()) return sql::Value::make_null();
            int code = (int)as_double(vals[0]);
            if (code < 0 || code > 127) return sql::Value::make_null();
            return sql::Value::make_string(std::string(1, (char)code));
        }
        if (upper == "SPACE" && vals.size() >= 1) {
            int n = (int)as_double(vals[0]);
            return sql::Value::make_string(std::string(std::max(0, n), ' '));
        }
        if (upper == "CONTAINS" && vals.size() >= 2) {
            // Tableau CONTAINS(string, substring)
            std::string s = vals[0].to_string();
            std::string sub = vals[1].to_string();
            return sql::Value::make_bool(s.find(sub) != std::string::npos);
        }
        if (upper == "STARTSWITH" && vals.size() >= 2) {
            std::string s = vals[0].to_string();
            std::string prefix = vals[1].to_string();
            return sql::Value::make_bool(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0);
        }
        if (upper == "ENDSWITH" && vals.size() >= 2) {
            std::string s = vals[0].to_string();
            std::string suffix = vals[1].to_string();
            return sql::Value::make_bool(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
        }
        if (upper == "FORMAT_NUMBER" && vals.size() >= 1) {
            // Format a number with optional decimal places
            double x = as_double(vals[0]);
            int decimals = (vals.size() >= 2) ? (int)as_double(vals[1]) : 2;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(decimals) << x;
            return sql::Value::make_string(oss.str());
        }
        if (upper == "TO_CHAR" && vals.size() >= 1) {
            return sql::Value::make_string(vals[0].to_string());
        }
        if (upper == "TO_NUMBER" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            try { return sql::Value::make_float(std::stod(vals[0].to_string())); }
            catch (...) { return sql::Value::make_null(); }
        }
        if (upper == "TO_INT" || upper == "INT" || upper == "CINT") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_int((int64_t)as_double(vals[0]));
        }

        // ─── Statistical / Prediction / Goal analysis ───────────────
        if (upper == "CORREL" && vals.size() >= 2) {
            // Expects two array values or paired numeric columns
            // For single-value mode: return null (needs array context)
            return sql::Value::make_null();
        }
        if (upper == "NORM_DIST" && vals.size() >= 3) {
            // Normal distribution CDF/PDF: NORM_DIST(x, mean, stddev [, cumulative])
            double x = as_double(vals[0]);
            double mu = as_double(vals[1]);
            double sigma = as_double(vals[2]);
            bool cumulative = (vals.size() >= 4) ? (as_double(vals[3]) != 0) : true;
            if (sigma <= 0) return sql::Value::make_null();
            double z = (x - mu) / sigma;
            if (cumulative) {
                return sql::Value::make_float(0.5 * (1.0 + std::erf(z / std::sqrt(2.0))));
            } else {
                return sql::Value::make_float(std::exp(-0.5 * z * z) / (sigma * std::sqrt(2.0 * M_PI)));
            }
        }
        if (upper == "NORM_INV" && vals.size() >= 3) {
            // Inverse normal CDF (Abramowitz & Stegun approximation)
            double p = as_double(vals[0]);
            double mu = as_double(vals[1]);
            double sigma = as_double(vals[2]);
            if (p <= 0 || p >= 1 || sigma <= 0) return sql::Value::make_null();
            // Rational approximation for inverse normal
            double t = (p < 0.5) ? std::sqrt(-2.0 * std::log(p)) : std::sqrt(-2.0 * std::log(1.0 - p));
            double c0 = 2.515517, c1 = 0.802853, c2 = 0.010328;
            double d1 = 1.432788, d2 = 0.189269, d3 = 0.001308;
            double z = t - (c0 + c1*t + c2*t*t) / (1.0 + d1*t + d2*t*t + d3*t*t*t);
            if (p < 0.5) z = -z;
            return sql::Value::make_float(mu + sigma * z);
        }
        if (upper == "T_DIST" && vals.size() >= 2) {
            // Student's t-distribution CDF approximation
            double x = as_double(vals[0]);
            double df = as_double(vals[1]);
            if (df <= 0) return sql::Value::make_null();
            // Beta-regularized incomplete approximation using normal for large df
            double z = x / std::sqrt(df);
            double cdf = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
            return sql::Value::make_float(cdf);
        }
        if (upper == "PERCENTILE" && vals.size() >= 2) {
            // PERCENTILE(array_val, k) — returns k-th percentile
            // Works on array values
            if (vals[0].type == sql::Value::Type::ARRAY && vals[0].composite_fields) {
                double k = as_double(vals[1]);
                if (k < 0 || k > 1) return sql::Value::make_null();
                std::vector<double> sorted;
                for (auto &v : *vals[0].composite_fields) {
                    if (!v.is_null()) sorted.push_back(as_double(v));
                }
                if (sorted.empty()) return sql::Value::make_null();
                std::sort(sorted.begin(), sorted.end());
                double idx = k * (sorted.size() - 1);
                size_t lo = (size_t)idx;
                double frac = idx - lo;
                if (lo + 1 >= sorted.size()) return sql::Value::make_float(sorted.back());
                return sql::Value::make_float(sorted[lo] * (1 - frac) + sorted[lo + 1] * frac);
            }
            return sql::Value::make_null();
        }
        if (upper == "FORECAST" || upper == "FORECAST_LINEAR") {
            // FORECAST(x, known_y_array, known_x_array) — linear regression prediction
            if (vals.size() >= 3) {
                double target_x = as_double(vals[0]);
                // If y and x are arrays
                if (vals[1].type == sql::Value::Type::ARRAY && vals[2].type == sql::Value::Type::ARRAY &&
                    vals[1].composite_fields && vals[2].composite_fields) {
                    auto &ys = *vals[1].composite_fields;
                    auto &xs = *vals[2].composite_fields;
                    size_t n = std::min(ys.size(), xs.size());
                    if (n < 2) return sql::Value::make_null();
                    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
                    for (size_t i = 0; i < n; i++) {
                        double x = as_double(xs[i]), y = as_double(ys[i]);
                        sum_x += x; sum_y += y; sum_xy += x*y; sum_xx += x*x;
                    }
                    double denom = n * sum_xx - sum_x * sum_x;
                    if (std::abs(denom) < 1e-15) return sql::Value::make_null();
                    double slope = (n * sum_xy - sum_x * sum_y) / denom;
                    double intercept = (sum_y - slope * sum_x) / n;
                    return sql::Value::make_float(intercept + slope * target_x);
                }
            }
            return sql::Value::make_null();
        }
        if (upper == "SLOPE" && vals.size() >= 2) {
            if (vals[0].type == sql::Value::Type::ARRAY && vals[1].type == sql::Value::Type::ARRAY &&
                vals[0].composite_fields && vals[1].composite_fields) {
                auto &ys = *vals[0].composite_fields;
                auto &xs = *vals[1].composite_fields;
                size_t n = std::min(ys.size(), xs.size());
                if (n < 2) return sql::Value::make_null();
                double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
                for (size_t i = 0; i < n; i++) {
                    double x = as_double(xs[i]), y = as_double(ys[i]);
                    sum_x += x; sum_y += y; sum_xy += x*y; sum_xx += x*x;
                }
                double denom = n * sum_xx - sum_x * sum_x;
                if (std::abs(denom) < 1e-15) return sql::Value::make_null();
                return sql::Value::make_float((n * sum_xy - sum_x * sum_y) / denom);
            }
            return sql::Value::make_null();
        }
        if (upper == "INTERCEPT" && vals.size() >= 2) {
            if (vals[0].type == sql::Value::Type::ARRAY && vals[1].type == sql::Value::Type::ARRAY &&
                vals[0].composite_fields && vals[1].composite_fields) {
                auto &ys = *vals[0].composite_fields;
                auto &xs = *vals[1].composite_fields;
                size_t n = std::min(ys.size(), xs.size());
                if (n < 2) return sql::Value::make_null();
                double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
                for (size_t i = 0; i < n; i++) {
                    double x = as_double(xs[i]), y = as_double(ys[i]);
                    sum_x += x; sum_y += y; sum_xy += x*y; sum_xx += x*x;
                }
                double denom = n * sum_xx - sum_x * sum_x;
                if (std::abs(denom) < 1e-15) return sql::Value::make_null();
                double slope = (n * sum_xy - sum_x * sum_y) / denom;
                return sql::Value::make_float((sum_y - slope * sum_x) / n);
            }
            return sql::Value::make_null();
        }
        if (upper == "RSQ" || upper == "R_SQUARED") {
            // R-squared (coefficient of determination)
            if (vals.size() >= 2 && vals[0].type == sql::Value::Type::ARRAY &&
                vals[1].type == sql::Value::Type::ARRAY &&
                vals[0].composite_fields && vals[1].composite_fields) {
                auto &ys = *vals[0].composite_fields;
                auto &xs = *vals[1].composite_fields;
                size_t n = std::min(ys.size(), xs.size());
                if (n < 2) return sql::Value::make_null();
                double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0, sum_yy = 0;
                for (size_t i = 0; i < n; i++) {
                    double x = as_double(xs[i]), y = as_double(ys[i]);
                    sum_x += x; sum_y += y; sum_xy += x*y; sum_xx += x*x; sum_yy += y*y;
                }
                double num = n * sum_xy - sum_x * sum_y;
                double denom = std::sqrt((n*sum_xx - sum_x*sum_x) * (n*sum_yy - sum_y*sum_y));
                if (std::abs(denom) < 1e-15) return sql::Value::make_null();
                double r = num / denom;
                return sql::Value::make_float(r * r);
            }
            return sql::Value::make_null();
        }
        if (upper == "GROWTH" && vals.size() >= 1) {
            // Simple growth rate: (current - previous) / previous
            // In scalar context: returns % growth between two values
            if (vals.size() >= 2) {
                double current = as_double(vals[0]);
                double previous = as_double(vals[1]);
                if (std::abs(previous) < 1e-15) return sql::Value::make_null();
                return sql::Value::make_float((current - previous) / previous);
            }
            return sql::Value::make_null();
        }

        // ─── Date convenience functions ─────────────────────────────
        if (upper == "TODAY") {
            // Equivalent to CURRENT_DATE
            auto now = std::chrono::system_clock::now();
            auto tp = std::chrono::system_clock::to_time_t(now);
            struct tm t; localtime_r(&tp, &t);
            return sql::Value::make_date(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        }
        if (upper == "YEAR" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::DATE_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].date_year());
            // Fallback: extract from "YYYY-MM-DD" string format
            if (vals[0].type == sql::Value::Type::STRING && vals[0].str_val.size() >= 4) {
                try { return sql::Value::make_int(std::stoi(vals[0].str_val.substr(0, 4))); } catch (...) {}
            }
            return sql::Value::make_null();
        }
        if (upper == "MONTH" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::DATE_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].date_month());
            if (vals[0].type == sql::Value::Type::STRING && vals[0].str_val.size() >= 7) {
                try { return sql::Value::make_int(std::stoi(vals[0].str_val.substr(5, 2))); } catch (...) {}
            }
            return sql::Value::make_null();
        }
        if (upper == "DAY" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::DATE_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].date_day());
            if (vals[0].type == sql::Value::Type::STRING && vals[0].str_val.size() >= 10) {
                try { return sql::Value::make_int(std::stoi(vals[0].str_val.substr(8, 2))); } catch (...) {}
            }
            return sql::Value::make_null();
        }
        if (upper == "HOUR" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::TIME_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].time_hour());
            return sql::Value::make_null();
        }
        if (upper == "MINUTE" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::TIME_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].time_minute());
            return sql::Value::make_null();
        }
        if (upper == "SECOND" && vals.size() >= 1) {
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::TIME_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL)
                return sql::Value::make_int(vals[0].time_second());
            return sql::Value::make_null();
        }
        if (upper == "DATEDIFF" && vals.size() >= 2) {
            // DATEDIFF(date1, date2) -> days between
            if (vals[0].is_null() || vals[1].is_null()) return sql::Value::make_null();
            if ((vals[0].type == sql::Value::Type::DATE_VAL || vals[0].type == sql::Value::Type::TIMESTAMP_VAL) &&
                (vals[1].type == sql::Value::Type::DATE_VAL || vals[1].type == sql::Value::Type::TIMESTAMP_VAL)) {
                return sql::Value::make_int(vals[0].int_val - vals[1].int_val);
            }
            return sql::Value::make_null();
        }
        if (upper == "DATEADD" && vals.size() >= 2) {
            // DATEADD(date, days_to_add)
            if (vals[0].is_null()) return sql::Value::make_null();
            if (vals[0].type == sql::Value::Type::DATE_VAL) {
                sql::Value result = vals[0];
                result.int_val += (int64_t)as_double(vals[1]);
                return result;
            }
            return sql::Value::make_null();
        }

        // ─── Type conversion helpers ────────────────────────────────
        if (upper == "FLOAT" || upper == "CDBL" || upper == "DOUBLE") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_float(as_double(vals[0]));
        }
        if (upper == "STR" || upper == "CSTR" || upper == "TEXT") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_null();
            return sql::Value::make_string(vals[0].to_string());
        }
        if (upper == "BOOL" || upper == "CBOOL") {
            if (vals.empty() || vals[0].is_null()) return sql::Value::make_null();
            auto &v = vals[0];
            if (v.type == sql::Value::Type::BOOL) return v;
            if (v.type == sql::Value::Type::INT64) return sql::Value::make_bool(v.int_val != 0);
            if (v.type == sql::Value::Type::FLOAT64) return sql::Value::make_bool(v.float_val != 0);
            if (v.type == sql::Value::Type::STRING) {
                std::string s = v.str_val;
                for (auto &c : s) c = (char)std::tolower((unsigned char)c);
                return sql::Value::make_bool(s == "true" || s == "1" || s == "yes");
            }
            return sql::Value::make_bool(false);
        }

        // ─── Bitwise operations ─────────────────────────────────────
        if (upper == "BIT_AND" && vals.size() >= 2) {
            return sql::Value::make_int((int64_t)as_double(vals[0]) & (int64_t)as_double(vals[1]));
        }
        if (upper == "BIT_OR" && vals.size() >= 2) {
            return sql::Value::make_int((int64_t)as_double(vals[0]) | (int64_t)as_double(vals[1]));
        }
        if (upper == "BIT_XOR" && vals.size() >= 2) {
            return sql::Value::make_int((int64_t)as_double(vals[0]) ^ (int64_t)as_double(vals[1]));
        }
        if (upper == "BIT_NOT" && vals.size() >= 1) {
            return sql::Value::make_int(~(int64_t)as_double(vals[0]));
        }
        if (upper == "BIT_SHIFT_LEFT" && vals.size() >= 2) {
            return sql::Value::make_int((int64_t)as_double(vals[0]) << (int)as_double(vals[1]));
        }
        if (upper == "BIT_SHIFT_RIGHT" && vals.size() >= 2) {
            return sql::Value::make_int((int64_t)as_double(vals[0]) >> (int)as_double(vals[1]));
        }

        // ─── Hash / encoding ────────────────────────────────────────
        if (upper == "CRC32" && vals.size() >= 1) {
            // Simple CRC32 implementation
            std::string s = vals[0].to_string();
            uint32_t crc = 0xFFFFFFFF;
            for (unsigned char c : s) {
                crc ^= c;
                for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
            return sql::Value::make_int((int64_t)(crc ^ 0xFFFFFFFF));
        }
        if (upper == "HEX" && vals.size() >= 1) {
            int64_t n = (int64_t)as_double(vals[0]);
            char buf[32]; std::snprintf(buf, sizeof(buf), "%llX", (unsigned long long)n);
            return sql::Value::make_string(std::string(buf));
        }
        if (upper == "UNHEX" && vals.size() >= 1) {
            std::string s = vals[0].to_string();
            try { return sql::Value::make_int((int64_t)std::stoull(s, nullptr, 16)); }
            catch (...) { return sql::Value::make_null(); }
        }
        if (upper == "BIN" && vals.size() >= 1) {
            int64_t n = (int64_t)as_double(vals[0]);
            if (n == 0) return sql::Value::make_string("0");
            std::string result;
            uint64_t u = (uint64_t)n;
            while (u) { result = (char)('0' + (u & 1)) + result; u >>= 1; }
            return sql::Value::make_string(result);
        }
        if (upper == "OCT" && vals.size() >= 1) {
            int64_t n = (int64_t)as_double(vals[0]);
            char buf[32]; std::snprintf(buf, sizeof(buf), "%llo", (unsigned long long)n);
            return sql::Value::make_string(std::string(buf));
        }

        // ─── Misc utility ───────────────────────────────────────────
        if (upper == "GENERATE_SERIES") {
            // GENERATE_SERIES(start, stop[, step]) -> ARRAY
            if (vals.size() >= 2) {
                int64_t start = (int64_t)as_double(vals[0]);
                int64_t stop = (int64_t)as_double(vals[1]);
                int64_t step = (vals.size() >= 3) ? (int64_t)as_double(vals[2]) : 1;
                if (step == 0) return sql::Value::make_null();
                std::vector<sql::Value> elements;
                if (step > 0) {
                    for (int64_t i = start; i <= stop && elements.size() < 10000; i += step)
                        elements.push_back(sql::Value::make_int(i));
                } else {
                    for (int64_t i = start; i >= stop && elements.size() < 10000; i += step)
                        elements.push_back(sql::Value::make_int(i));
                }
                return sql::Value::make_array(std::move(elements));
            }
            return sql::Value::make_null();
        }
        if (upper == "HASH" && vals.size() >= 1) {
            // Generic hash function for any value
            std::string s = vals[0].to_string();
            uint64_t h = 14695981039346656037ULL;
            for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ULL; }
            return sql::Value::make_int((int64_t)h);
        }
        if (upper == "ENCODE" && vals.size() >= 2) {
            // ENCODE(data, format) — currently just hex
            std::string fmt = vals[1].to_string();
            for (auto &c : fmt) c = (char)std::tolower((unsigned char)c);
            if (fmt == "hex") {
                std::string s = vals[0].to_string();
                std::string result;
                for (unsigned char c : s) {
                    char buf[4]; std::snprintf(buf, sizeof(buf), "%02x", c);
                    result += buf;
                }
                return sql::Value::make_string(result);
            }
            return vals[0];
        }
        if (upper == "DECODE" && vals.size() >= 2) {
            std::string fmt = vals[1].to_string();
            for (auto &c : fmt) c = (char)std::tolower((unsigned char)c);
            if (fmt == "hex") {
                std::string s = vals[0].to_string();
                std::string result;
                for (size_t i = 0; i + 1 < s.size(); i += 2) {
                    int byte = 0;
                    try { byte = std::stoi(s.substr(i, 2), nullptr, 16); } catch (...) {}
                    result += (char)byte;
                }
                return sql::Value::make_string(result);
            }
            return vals[0];
        }

        // Fallback: treat unknown functions as returning their first arg or NULL
        return vals.empty() ? sql::Value::make_null() : vals[0];
    }

    // ===============================================
    // Spatial pushdown (R-Tree-style bbox prefilter)
    // ===============================================
    struct SpatialPushdown {
        bool eligible = false;
        // Column reference on one side of the predicate (the "indexed" side).
        sql::ast::ColumnRef col;
        // Bounding box of the literal geometry on the other side.
        double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
        // For ST_DWithin: distance to inflate the literal bbox by.
        double inflate = 0;
    };

    // Walk a WHERE expression looking for a single top-level spatial call
    // whose RHS is a literal expression (no row state needed to evaluate).
    SpatialPushdown analyze_spatial_pushdown(const sql::ast::Expr &where,
                                              const sql::Schema &schema) {
        SpatialPushdown out;
        if (where.type != sql::ast::ExprType::FUNCTION_CALL) return out;
        auto &fc = std::get<sql::ast::FunctionCall>(where.data);
        std::string name = fc.name;
        for (auto &c : name) c = (char)std::toupper((unsigned char)c);
        bool is_intersects = (name == "ST_INTERSECTS" || name == "ST_CONTAINS" || name == "ST_WITHIN");
        bool is_dwithin    = (name == "ST_DWITHIN");
        if (!is_intersects && !is_dwithin) return out;
        if (fc.args.size() < 2) return out;
        // Identify (col, literal_geom) — column is whichever side is a
        // ColumnRef, literal must be a function call producing a Value with
        // no row dependency. We test by attempting to evaluate against an
        // empty row; failures fall through to the slow path.
        const sql::ast::Expr *col_e = nullptr, *lit_e = nullptr;
        if (fc.args[0]->type == sql::ast::ExprType::COLUMN_REF) { col_e = fc.args[0].get(); lit_e = fc.args[1].get(); }
        else if (fc.args[1]->type == sql::ast::ExprType::COLUMN_REF) { col_e = fc.args[1].get(); lit_e = fc.args[0].get(); }
        else return out;
        sql::Value lit_val;
        try { lit_val = eval_expr_with_row(*lit_e, {}, {}); } catch (...) { return out; }
        if (lit_val.type != sql::Value::Type::GEOMETRY) return out;
        double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
        if (!geometry_bbox(lit_val.str_val, xmin, ymin, xmax, ymax)) return out;
        out.col = std::get<sql::ast::ColumnRef>(col_e->data);
        out.xmin = xmin; out.ymin = ymin; out.xmax = xmax; out.ymax = ymax;
        // Verify the column resolves in the current schema.
        if (find_col_index_qualified(schema, out.col) < 0) return out;
        if (is_dwithin && fc.args.size() >= 3) {
            try {
                sql::Value d = eval_expr_with_row(*fc.args[2], {}, {});
                out.inflate = to_double(d);
            } catch (...) { return out; }
        }
        out.eligible = true;
        return out;
    }

    // Extract the planar bbox from a WKT string. Returns true on success.
    bool geometry_bbox(const std::string &wkt, double &xmin, double &ymin,
                       double &xmax, double &ymax) {
        size_t i = 0;
        auto skip_ws = [&]() { while (i < wkt.size() && std::isspace((unsigned char)wkt[i])) i++; };
        auto match_kw = [&](const char *kw) {
            size_t save = i; size_t k = 0;
            while (kw[k] && i < wkt.size() && std::toupper((unsigned char)wkt[i]) == (unsigned char)kw[k]) { i++; k++; }
            if (kw[k] == 0) return true;
            i = save; return false;
        };
        auto parse_double = [&](double &out) {
            skip_ws();
            size_t s = i;
            if (i < wkt.size() && (wkt[i] == '+' || wkt[i] == '-')) i++;
            while (i < wkt.size() && (std::isdigit((unsigned char)wkt[i]) || wkt[i] == '.' || wkt[i] == 'e' || wkt[i] == 'E' || wkt[i] == '+' || wkt[i] == '-')) i++;
            if (s == i) return false;
            try { out = std::stod(wkt.substr(s, i - s)); } catch (...) { return false; }
            return true;
        };
        skip_ws();
        bool first = true;
        auto see = [&](double x, double y) {
            if (first) { xmin = xmax = x; ymin = ymax = y; first = false; }
            else {
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
            }
        };
        if (match_kw("POINT")) {
            skip_ws(); match_kw("Z"); skip_ws();
            if (i >= wkt.size() || wkt[i] != '(') return false;
            i++;
            double x = 0, y = 0;
            if (!parse_double(x) || !parse_double(y)) return false;
            see(x, y);
            return true;
        }
        if (match_kw("LINESTRING") || match_kw("POLYGON")) {
            // We accept either since for bbox purposes all that matters is
            // the vertex stream. Strip parens loosely.
            while (i < wkt.size() && wkt[i] != ')') {
                if (wkt[i] == '(' || wkt[i] == ',' || std::isspace((unsigned char)wkt[i])) { i++; continue; }
                double x = 0, y = 0;
                if (!parse_double(x)) { i++; continue; }
                if (!parse_double(y)) return false;
                see(x, y);
            }
            return !first;
        }
        return false;
    }

    bool bbox_overlap_row(const sql::Tuple &row, const sql::Schema &schema,
                          const SpatialPushdown &pd) {
        int ci = find_col_index_qualified(schema, pd.col);
        if (ci < 0 || ci >= (int)row.size()) return true; // safety: defer to slow path
        const sql::Value &v = row[(size_t)ci];
        if (v.type != sql::Value::Type::GEOMETRY) return true;
        double rxmin = 0, rymin = 0, rxmax = 0, rymax = 0;
        if (!geometry_bbox(v.str_val, rxmin, rymin, rxmax, rymax)) return true;
        // Inflate the literal bbox by pd.inflate (ST_DWithin radius).
        double lxmin = pd.xmin - pd.inflate, lymin = pd.ymin - pd.inflate;
        double lxmax = pd.xmax + pd.inflate, lymax = pd.ymax + pd.inflate;
        // Standard 2D bbox-overlap test.
        return !(rxmax < lxmin || rxmin > lxmax || rymax < lymin || rymin > lymax);
    }

    // ===============================================
    // CAST
    // ===============================================

    sql::Value eval_cast(const sql::Value &v, const sql::ast::DataType &target) {
        if (v.is_null()) return sql::Value::make_null();
        std::string tname = target.name;
        for (auto &c : tname) c = (char)std::toupper((unsigned char)c);

        if (tname == "INT" || tname == "INTEGER" || tname == "BIGINT" || tname == "SMALLINT") {
            if (v.type == sql::Value::Type::INT64) return v;
            if (v.type == sql::Value::Type::FLOAT64) return sql::Value::make_int((int64_t)v.float_val);
            if (v.type == sql::Value::Type::STRING) {
                try { return sql::Value::make_int(std::stoll(v.str_val)); } catch (...) {}
            }
            if (v.type == sql::Value::Type::BOOL) return sql::Value::make_int(v.bool_val ? 1 : 0);
        }
        if (tname == "FLOAT" || tname == "DOUBLE" || tname == "REAL" || tname == "DECIMAL" || tname == "NUMERIC") {
            if (v.type == sql::Value::Type::FLOAT64) return v;
            if (v.type == sql::Value::Type::INT64) return sql::Value::make_float((double)v.int_val);
            if (v.type == sql::Value::Type::STRING) {
                try { return sql::Value::make_float(std::stod(v.str_val)); } catch (...) {}
            }
        }
        if (tname == "VARCHAR" || tname == "TEXT" || tname == "CHAR") {
            return sql::Value::make_string(v.to_string());
        }
        if (tname == "BOOLEAN" || tname == "BOOL") {
            if (v.type == sql::Value::Type::BOOL) return v;
            if (v.type == sql::Value::Type::INT64) return sql::Value::make_bool(v.int_val != 0);
            if (v.type == sql::Value::Type::STRING) {
                std::string s = v.str_val;
                for (auto &c : s) c = (char)std::toupper((unsigned char)c);
                return sql::Value::make_bool(s == "TRUE" || s == "1" || s == "T" || s == "YES");
            }
        }
        return sql::Value::make_string(v.to_string());
    }

    // ===============================================
    // Binary Operators -- with proper NULL semantics
    // ===============================================

    sql::Value eval_binary_op(sql::TokenType op, const sql::Value &lv, const sql::Value &rv) {
        using TT = sql::TokenType;
        using VT = sql::Value::Type;

        // NULL propagation for comparisons
        if (op == TT::EQ || op == TT::NEQ || op == TT::LT || op == TT::GT || op == TT::LTE || op == TT::GTE) {
            if (lv.is_null() || rv.is_null()) return sql::Value::make_null();
            int cmp = compare_values(lv, rv);
            switch (op) {
                case TT::EQ:  return sql::Value::make_bool(cmp == 0);
                case TT::NEQ: return sql::Value::make_bool(cmp != 0);
                case TT::LT:  return sql::Value::make_bool(cmp < 0);
                case TT::GT:  return sql::Value::make_bool(cmp > 0);
                case TT::LTE: return sql::Value::make_bool(cmp <= 0);
                case TT::GTE: return sql::Value::make_bool(cmp >= 0);
                default: break;
            }
        }

        // Logical with NULL propagation
        if (op == TT::KW_AND) {
            if (lv.type == VT::BOOL && !lv.bool_val) return sql::Value::make_bool(false); // FALSE AND anything = FALSE
            if (rv.type == VT::BOOL && !rv.bool_val) return sql::Value::make_bool(false);
            if (lv.is_null() || rv.is_null()) return sql::Value::make_null();
            return sql::Value::make_bool(lv.bool_val && rv.bool_val);
        }
        if (op == TT::KW_OR) {
            if (lv.type == VT::BOOL && lv.bool_val) return sql::Value::make_bool(true); // TRUE OR anything = TRUE
            if (rv.type == VT::BOOL && rv.bool_val) return sql::Value::make_bool(true);
            if (lv.is_null() || rv.is_null()) return sql::Value::make_null();
            return sql::Value::make_bool(lv.bool_val || rv.bool_val);
        }

        // Arithmetic -- NULL propagation
        if (lv.is_null() || rv.is_null()) return sql::Value::make_null();

        // DECIMAL arithmetic — lossless via __int128 of unscaled magnitude.
        if ((op == TT::PLUS || op == TT::MINUS || op == TT::STAR || op == TT::SLASH) &&
            (lv.type == VT::DECIMAL || rv.type == VT::DECIMAL)) {
#if defined(__SIZEOF_INT128__)
            // __int128 is a compiler extension; not strictly ISO. Wrap to avoid
            // -Wpedantic noise while keeping precise arithmetic on supported
            // toolchains (gcc, clang).
            _Pragma("GCC diagnostic push")
            _Pragma("GCC diagnostic ignored \"-Wpedantic\"")
            using i128 = __int128;
            _Pragma("GCC diagnostic pop")
            auto parse = [](const sql::Value &v, i128 &unscaled, int &scale, bool &neg) {
                std::string s;
                if (v.type == VT::DECIMAL) { s = v.str_val; scale = (int)v.int_val; }
                else if (v.type == VT::INT64) { s = std::to_string(v.int_val); scale = 0; }
                else if (v.type == VT::FLOAT64) { std::ostringstream o; o.precision(15); o << v.float_val; s = o.str(); scale = -1; }
                else { s = v.to_string(); scale = -1; }
                neg = false;
                size_t i = 0;
                if (!s.empty() && s[0] == '-') { neg = true; i = 1; }
                else if (!s.empty() && s[0] == '+') i = 1;
                auto dot = s.find('.', i);
                unscaled = 0;
                if (dot == std::string::npos) {
                    for (; i < s.size(); i++) {
                        if (!std::isdigit((unsigned char)s[i])) return false;
                        unscaled = unscaled * 10 + (s[i] - '0');
                    }
                    if (scale < 0) scale = 0;
                } else {
                    int observed_scale = (int)(s.size() - dot - 1);
                    for (; i < s.size(); i++) {
                        if (i == dot) continue;
                        if (!std::isdigit((unsigned char)s[i])) return false;
                        unscaled = unscaled * 10 + (s[i] - '0');
                    }
                    if (scale < 0) scale = observed_scale;
                }
                return true;
            };
            auto rescale_up = [](i128 v, int by) {
                while (by-- > 0) v *= 10;
                return v;
            };
            auto format = [](i128 v, int scale, bool neg) {
                if (v == 0) {
                    if (scale == 0) return std::string("0");
                    std::string s = "0.";
                    s.append((size_t)scale, '0');
                    return s;
                }
                std::string digits;
                while (v > 0) { digits.push_back('0' + (int)(v % 10)); v /= 10; }
                while ((int)digits.size() < scale + 1) digits.push_back('0');
                std::reverse(digits.begin(), digits.end());
                std::string out;
                if (neg) out.push_back('-');
                if (scale == 0) { out += digits; return out; }
                size_t dot_pos = digits.size() - (size_t)scale;
                out += digits.substr(0, dot_pos);
                out += '.';
                out += digits.substr(dot_pos);
                return out;
            };
            i128 lu = 0, ru = 0; int ls = 0, rs = 0; bool ln = false, rn = false;
            if (!parse(lv, lu, ls, ln) || !parse(rv, ru, rs, rn))
                return sql::Value::make_null();
            if (op == TT::PLUS || op == TT::MINUS) {
                int s = std::max(ls, rs);
                lu = rescale_up(lu, s - ls);
                ru = rescale_up(ru, s - rs);
                i128 a = ln ? -lu : lu;
                i128 b = rn ? -ru : ru;
                i128 r = (op == TT::PLUS) ? a + b : a - b;
                bool neg = r < 0; if (neg) r = -r;
                return sql::Value::make_decimal(format(r, s, neg), s);
            }
            if (op == TT::STAR) {
                int s = ls + rs;
                i128 r = lu * ru;
                bool neg = ln ^ rn;
                return sql::Value::make_decimal(format(r, s, neg), s);
            }
            if (op == TT::SLASH) {
                if (ru == 0) return sql::Value::make_null();
                int target_scale = std::max(ls, rs);
                int extra = target_scale + rs;     // shift dividend up by rs to retain divisor scale
                i128 lshift = rescale_up(lu, extra - ls);
                i128 q = lshift / ru;
                bool neg = ln ^ rn;
                return sql::Value::make_decimal(format(q, target_scale, neg), target_scale);
            }
#else
            // Fallback for toolchains without __int128: lose precision via double.
            double dl = to_double(lv), dr = to_double(rv);
            switch (op) {
                case TT::PLUS:  return sql::Value::make_decimal(std::to_string(dl + dr), 6);
                case TT::MINUS: return sql::Value::make_decimal(std::to_string(dl - dr), 6);
                case TT::STAR:  return sql::Value::make_decimal(std::to_string(dl * dr), 6);
                case TT::SLASH: return dr == 0 ? sql::Value::make_null() : sql::Value::make_decimal(std::to_string(dl / dr), 6);
                default: break;
            }
#endif
        }

        if (op == TT::PLUS || op == TT::MINUS || op == TT::STAR || op == TT::SLASH || op == TT::PERCENT) {
            if (lv.type == VT::INT64 && rv.type == VT::INT64) {
                switch (op) {
                    case TT::PLUS:  return sql::Value::make_int(lv.int_val + rv.int_val);
                    case TT::MINUS: return sql::Value::make_int(lv.int_val - rv.int_val);
                    case TT::STAR:  return sql::Value::make_int(lv.int_val * rv.int_val);
                    case TT::SLASH: return rv.int_val == 0 ? sql::Value::make_null() : sql::Value::make_int(lv.int_val / rv.int_val);
                    case TT::PERCENT: return rv.int_val == 0 ? sql::Value::make_null() : sql::Value::make_int(lv.int_val % rv.int_val);
                    default: break;
                }
            }
            double dl = to_double(lv), dr = to_double(rv);
            switch (op) {
                case TT::PLUS:  return sql::Value::make_float(dl + dr);
                case TT::MINUS: return sql::Value::make_float(dl - dr);
                case TT::STAR:  return sql::Value::make_float(dl * dr);
                case TT::SLASH: return dr == 0 ? sql::Value::make_null() : sql::Value::make_float(dl / dr);
                case TT::PERCENT: return dr == 0 ? sql::Value::make_null() : sql::Value::make_float(std::fmod(dl, dr));
                default: break;
            }
        }

        if (op == TT::CONCAT) {
            return sql::Value::make_string(lv.to_string() + rv.to_string());
        }

        return sql::Value::make_null();
    }

    // ===============================================
    // Helpers
    // ===============================================

    bool eval_predicate(const sql::ast::Expr &expr, const sql::Tuple &row, const sql::Schema &schema) {
        auto v = eval_expr_with_row(expr, row, schema);
        return v.type == sql::Value::Type::BOOL && v.bool_val;
    }

    int find_col_index(const sql::Schema &schema, const std::string &name) {
        for (size_t i = 0; i < schema.size(); i++) {
            if (schema[i].name == name) return (int)i;
        }
        return -1;
    }

    int find_col_index_qualified(const sql::Schema &schema, const sql::ast::ColumnRef &ref) {
        // If table qualifier is present, match on table.column
        if (ref.table.has_value()) {
            for (size_t i = 0; i < schema.size(); i++) {
                if (schema[i].table == ref.table.value() && schema[i].name == ref.column)
                    return (int)i;
            }
            // Table qualifier didn't match any column in this schema — do NOT fall
            // back to unqualified matching. This allows correlated subqueries to
            // resolve outer-scope references correctly (e.g., aa.id in a subquery
            // that has its own "id" column from table bb).
            return -1;
        }
        // No table qualifier: match by column name only
        return find_col_index(schema, ref.column);
    }

    double to_double(const sql::Value &v) {
        if (v.type == sql::Value::Type::INT64) return (double)v.int_val;
        if (v.type == sql::Value::Type::FLOAT64) return v.float_val;
        if (v.type == sql::Value::Type::STRING) { try { return std::stod(v.str_val); } catch (...) {} }
        return 0.0;
    }

    int compare_values(const sql::Value &a, const sql::Value &b) {
        if (a.is_null() && b.is_null()) return 0;
        if (a.is_null()) return -1;
        if (b.is_null()) return 1;
        // Numeric comparison
        if ((a.type == sql::Value::Type::INT64 || a.type == sql::Value::Type::FLOAT64) &&
            (b.type == sql::Value::Type::INT64 || b.type == sql::Value::Type::FLOAT64)) {
            double da = to_double(a), db = to_double(b);
            if (da < db) return -1;
            if (da > db) return 1;
            return 0;
        }
        // String comparison
        std::string sa = a.to_string(), sb = b.to_string();
        return sa.compare(sb);
    }

    bool like_match(const std::string &str, const std::string &pattern) {
        return like_match_impl(str, 0, pattern, 0);
    }

    bool like_match_impl(const std::string &s, size_t si, const std::string &p, size_t pi) {
        while (pi < p.size()) {
            char pc = p[pi];
            if (pc == '%') {
                pi++;
                if (pi == p.size()) return true;
                for (size_t k = si; k <= s.size(); k++) {
                    if (like_match_impl(s, k, p, pi)) return true;
                }
                return false;
            }
            if (pc == '_') {
                if (si >= s.size()) return false;
                si++; pi++;
            } else {
                if (si >= s.size() || s[si] != pc) return false;
                si++; pi++;
            }
        }
        return si == s.size();
    }

    bool is_select_star(const std::vector<sql::ast::ExprPtr> &list) {
        return list.size() == 1 && list[0]->type == sql::ast::ExprType::COLUMN_REF &&
               std::get<sql::ast::ColumnRef>(list[0]->data).column == "*";
    }

    std::string get_expr_name(const sql::ast::Expr &expr) {
        if (!expr.alias.empty()) return expr.alias;
        if (expr.type == sql::ast::ExprType::COLUMN_REF)
            return std::get<sql::ast::ColumnRef>(expr.data).column;
        if (expr.type == sql::ast::ExprType::AGGREGATE_CALL) {
            auto &agg = std::get<sql::ast::AggregateCall>(expr.data);
            return agg.name;
        }
        if (expr.type == sql::ast::ExprType::FUNCTION_CALL) {
            auto &fc = std::get<sql::ast::FunctionCall>(expr.data);
            return fc.name;
        }
        return "?column?";
    }

    void sort_results(std::vector<sql::Tuple> &rows,
                      const std::vector<sql::ast::OrderByItem> &order_by,
                      const sql::Schema &schema) {
        std::sort(rows.begin(), rows.end(),
            [&](const sql::Tuple &a, const sql::Tuple &b) -> bool {
                for (auto &item : order_by) {
                    int ci = -1;
                    if (item.expr->type == sql::ast::ExprType::COLUMN_REF) {
                        auto &ref = std::get<sql::ast::ColumnRef>(item.expr->data);
                        ci = find_col_index(schema, ref.column);
                    }
                    if (ci >= 0 && ci < (int)a.size() && ci < (int)b.size()) {
                        int cmp = compare_values(a[ci], b[ci]);
                        if (cmp != 0) return item.ascending ? cmp < 0 : cmp > 0;
                    }
                }
                return false;
            });
    }
};

} // namespace tdb

#endif // TDB_DATABASE_H
