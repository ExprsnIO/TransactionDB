#ifndef TDB_CATALOG_H
#define TDB_CATALOG_H

#include "tdb/sql/ast.h"
#include "tdb/sql/executor.h"
#include "tdb/version_object.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace tdb::catalog {

struct ColumnInfo {
    std::string name;
    sql::ast::DataType type;
    bool nullable = true;
    bool encrypted = false;
    uint16_t ordinal = 0;
    bool generated = false;
    sql::ast::ExprPtr generated_expr;
    bool generated_stored = true;
    // Constraints
    bool primary_key = false;
    bool unique = false;
    bool auto_increment = false;
    sql::ast::ExprPtr default_value;   // DEFAULT expr
    sql::ast::ExprPtr check_expr;      // CHECK(expr)
};

struct ForeignKeyInfo {
    std::string name;
    std::vector<std::string> columns;
    std::string ref_table;
    std::vector<std::string> ref_columns;
    std::string on_delete;  // CASCADE, RESTRICT, SET NULL, SET DEFAULT
    std::string on_update;
};

// ─── Partition metadata ───
struct PartitionBoundInfo {
    std::vector<sql::Value> values;  // bound values
    bool is_maxvalue = false;
    bool is_default = false;
    // HASH
    int modulus = 0;
    int remainder_val = 0;
};

struct PartitionInfo {
    std::string name;
    PartitionBoundInfo bound;
    std::vector<sql::Tuple> rows;    // partition-local row storage
};

struct TablePartitionSpec {
    sql::ast::PartitionType type;
    std::vector<std::string> columns;          // partition key column names
    std::vector<int> column_indices;           // resolved column ordinals
    std::vector<PartitionInfo> partitions;
};

struct TableInfo {
    std::string schema;
    std::string name;
    std::vector<ColumnInfo> columns;
    std::vector<sql::Tuple> rows;              // non-partitioned row storage
    bool encrypted = false;
    bool temporary = false;
    bool columnar = false;     // column-oriented storage (STORE AS COLUMN)
    std::string tablespace;
    // Columnar storage: data stored per-column instead of per-row
    struct ColumnStore {
        std::vector<sql::Value> data; // all values for one column, row-indexed
    };
    std::vector<ColumnStore> column_data; // column_data[col_idx].data[row_idx]

    // Materialize columnar data to row format (for queries)
    std::vector<sql::Tuple> materialize_rows() const {
        if (!columnar || column_data.empty()) return rows;
        size_t num_rows = column_data.empty() ? 0 : column_data[0].data.size();
        size_t num_cols = column_data.size();
        std::vector<sql::Tuple> result(num_rows);
        for (size_t r = 0; r < num_rows; r++) {
            result[r].resize(num_cols);
            for (size_t c = 0; c < num_cols; c++) {
                if (r < column_data[c].data.size())
                    result[r][c] = column_data[c].data[r];
            }
        }
        return result;
    }
    // Constraints
    std::vector<std::string> primary_key_cols;
    std::vector<ForeignKeyInfo> foreign_keys;
    int64_t auto_increment_counter = 0;
    ObjectVersion version = ObjectVersion::initial();
    // Partitioning
    std::optional<TablePartitionSpec> partition_spec;

    bool is_partitioned() const { return partition_spec.has_value(); }

    // Get all rows across all partitions (for scans that can't prune)
    std::vector<sql::Tuple> all_rows() const {
        if (!is_partitioned()) return rows;
        std::vector<sql::Tuple> result;
        for (auto &p : partition_spec->partitions)
            result.insert(result.end(), p.rows.begin(), p.rows.end());
        return result;
    }

    size_t total_row_count() const {
        if (!is_partitioned()) return rows.size();
        size_t count = 0;
        for (auto &p : partition_spec->partitions) count += p.rows.size();
        return count;
    }
};

struct IndexInfo {
    std::string name;
    std::string table;
    sql::ast::IndexMethod method;
    std::vector<std::string> columns;
    bool unique = false;
    ObjectVersion version = ObjectVersion::initial();
};

struct ViewInfo {
    std::string schema;
    std::string name;
    std::vector<std::string> columns;
    std::string query_text;
    sql::ast::SelectPtr query;
    bool or_replace = false;
    ObjectVersion version = ObjectVersion::initial();
};

struct MaterializedViewInfo {
    std::string schema;
    std::string name;
    std::vector<ColumnInfo> columns;
    std::vector<sql::Tuple> rows;
    std::string query_text;
    sql::ast::SelectPtr query;
    bool writable = false;
    bool with_data = true;
    std::string tablespace;
    uint64_t last_refresh_epoch = 0;
    ObjectVersion version = ObjectVersion::initial();
};

struct SavedQueryInfo {
    std::string schema;
    std::string name;
    std::string description;
    std::vector<std::pair<std::string, sql::ast::DataType>> parameters;
    std::string query_text;
    sql::ast::SelectPtr query;
    ObjectVersion version = ObjectVersion::initial();
};

struct TablespaceInfo {
    std::string name;
    std::string location;
    std::string owner;
    std::unordered_map<std::string, std::string> options;
    ObjectVersion version = ObjectVersion::initial();
};

struct SequenceInfo {
    std::string name;
    std::string schema;
    int64_t current_value = 0;
    int64_t start = 1;
    int64_t increment = 1;
    int64_t min_value = 1;
    int64_t max_value = INT64_MAX;
    bool cycle = false;
    ObjectVersion version = ObjectVersion::initial();
};

struct PreparedStmtInfo {
    std::string name;
    std::string sql_text;
    sql::ast::StmtPtr parsed;
};

// ─── Enum types ───
struct EnumTypeInfo {
    std::string schema;
    std::string name;
    std::vector<std::string> labels;
    ObjectVersion version = ObjectVersion::initial();
};

// ─── DOMAIN types ───
// A DOMAIN is a constrained alias: a base SQL type plus an optional CHECK
// predicate that every value assigned to a column of the domain must satisfy.
struct DomainTypeInfo {
    std::string schema;
    std::string name;
    sql::ast::DataType base_type;
    sql::ast::ExprPtr check_expr;  // optional; nullable
    ObjectVersion version = ObjectVersion::initial();
};

// ─── Composite (user-defined record) types ───
// Each field has a name and a type spec. Type spec may name another composite
// type, enabling nesting, or any built-in (including JSON/XML).
struct CompositeFieldInfo {
    std::string name;
    sql::ast::DataType type;
};

struct CompositeTypeInfo {
    std::string schema;
    std::string name;
    std::vector<CompositeFieldInfo> fields;
    ObjectVersion version = ObjectVersion::initial();
};

// ─── Document objects (Phase 2) ───
// Standalone JSON/XML documents stored as top-level catalog objects, queryable
// by name via XPath / XQuery / GraphQL. Distinct from JSON/XML column types,
// which carry document payloads inside table rows.
enum class DocumentFormat { JSON, XML };

struct DocumentInfo {
    std::string schema;
    std::string name;
    DocumentFormat format = DocumentFormat::JSON;
    std::string content;
    ObjectVersion version = ObjectVersion::initial();
    std::unordered_map<std::string, std::string> namespaces;  // XML namespace bindings
};

// ─── Scripts & Triggers (Phase 3) ───
// Lua scripts stored as catalog objects. is_udf=true means the script is
// callable in SELECT expressions; is_udf=false means CALL-style procedures.
struct ScriptParam {
    std::string name;
    std::string type_name;  // SQL type tag (e.g. "INT", "FLOAT", "VARCHAR(50)")
};

struct ScriptInfo {
    std::string schema;
    std::string name;
    std::string lua_source;
    std::vector<ScriptParam> params;
    bool has_return = false;
    std::string return_type;     // SQL type if has_return
    bool is_udf = false;
    ObjectVersion version = ObjectVersion::initial();
};

enum class TriggerEvent  { INSERT, UPDATE, DELETE };
enum class TriggerTiming { BEFORE, AFTER };

struct TriggerInfo {
    std::string name;
    std::string table;
    TriggerTiming timing = TriggerTiming::AFTER;
    TriggerEvent  event  = TriggerEvent::INSERT;
    std::string script_name;
    ObjectVersion version = ObjectVersion::initial();
};

// ─── Users & privileges (Phase 4) ───
// Database access modes, set at creation. ANONYMOUS = open read/write, no
// users; SINGLE_USER = exactly one credentialed user; MULTI_USER = many.
enum class DatabaseMode : uint32_t { ANONYMOUS = 0, SINGLE_USER = 1, MULTI_USER = 2 };

struct UserInfo {
    std::string username;
    uint8_t  salt[16] = {0};
    uint8_t  password_hash[32] = {0};   // PBKDF2-HMAC-SHA256 of password
    uint32_t kdf_iterations = 100000;
    bool     is_superuser = false;
    ObjectVersion version = ObjectVersion::initial();
};

struct Privilege {
    std::string grantee;        // username
    std::string privilege;      // SELECT, INSERT, UPDATE, DELETE, EXECUTE, ALL
    std::string object_kind;    // table | view | document | script | *
    std::string object_name;    // specific name or *
    bool with_grant_option = false;
};

class Catalog {
public:
    // ─── Tables ───
    void add_table(const std::string &name, TableInfo info);
    TableInfo *find_table(const std::string &name);
    const TableInfo *find_table(const std::string &name) const;
    bool drop_table(const std::string &name);
    std::vector<std::string> list_tables() const;

    // ─── Indexes ───
    void add_index(const std::string &name, IndexInfo info);
    const IndexInfo *find_index(const std::string &name) const;
    bool drop_index(const std::string &name);
    std::vector<std::string> list_indexes() const;

    // ─── Views ───
    void add_view(const std::string &name, ViewInfo info);
    ViewInfo *find_view(const std::string &name);
    const ViewInfo *find_view(const std::string &name) const;
    bool drop_view(const std::string &name);
    std::vector<std::string> list_views() const;

    // ─── Materialized Views ───
    void add_materialized_view(const std::string &name, MaterializedViewInfo info);
    MaterializedViewInfo *find_materialized_view(const std::string &name);
    const MaterializedViewInfo *find_materialized_view(const std::string &name) const;
    bool drop_materialized_view(const std::string &name);
    std::vector<std::string> list_materialized_views() const;

    // ─── Saved Queries ───
    void add_saved_query(const std::string &name, SavedQueryInfo info);
    SavedQueryInfo *find_saved_query(const std::string &name);
    const SavedQueryInfo *find_saved_query(const std::string &name) const;
    bool drop_saved_query(const std::string &name);
    std::vector<std::string> list_saved_queries() const;

    // ─── Tablespaces ───
    void add_tablespace(const std::string &name, TablespaceInfo info);
    const TablespaceInfo *find_tablespace(const std::string &name) const;
    bool drop_tablespace(const std::string &name);
    std::vector<std::string> list_tablespaces() const;

    // ─── Sequences ───
    void add_sequence(const std::string &name, SequenceInfo info);
    SequenceInfo *find_sequence(const std::string &name);
    const SequenceInfo *find_sequence(const std::string &name) const;
    bool drop_sequence(const std::string &name);
    int64_t nextval(const std::string &name);
    int64_t currval(const std::string &name) const;

    // ─── Prepared Statements ───
    void add_prepared(const std::string &name, PreparedStmtInfo info);
    PreparedStmtInfo *find_prepared(const std::string &name);
    bool drop_prepared(const std::string &name);

    // ─── Composite types (Batch 5) ───
    void add_composite_type(const std::string &name, CompositeTypeInfo info) {
        composite_types_[name] = std::move(info);
    }
    CompositeTypeInfo *find_composite_type(const std::string &name) {
        auto it = composite_types_.find(name);
        return it == composite_types_.end() ? nullptr : &it->second;
    }
    const CompositeTypeInfo *find_composite_type(const std::string &name) const {
        auto it = composite_types_.find(name);
        return it == composite_types_.end() ? nullptr : &it->second;
    }
    bool drop_composite_type(const std::string &name) {
        return composite_types_.erase(name) > 0;
    }
    std::vector<std::string> list_composite_types() const {
        std::vector<std::string> names;
        names.reserve(composite_types_.size());
        for (auto &kv : composite_types_) names.push_back(kv.first);
        return names;
    }

    // ─── Enum types ───
    void add_enum_type(const std::string &name, EnumTypeInfo info) { enum_types_[name] = std::move(info); }
    EnumTypeInfo *find_enum_type(const std::string &name) {
        auto it = enum_types_.find(name); return it == enum_types_.end() ? nullptr : &it->second;
    }
    const EnumTypeInfo *find_enum_type(const std::string &name) const {
        auto it = enum_types_.find(name); return it == enum_types_.end() ? nullptr : &it->second;
    }
    bool drop_enum_type(const std::string &name) { return enum_types_.erase(name) > 0; }

    // ─── DOMAIN types ───
    void add_domain_type(const std::string &name, DomainTypeInfo info) {
        domain_types_[name] = std::move(info);
    }
    DomainTypeInfo *find_domain_type(const std::string &name) {
        auto it = domain_types_.find(name); return it == domain_types_.end() ? nullptr : &it->second;
    }
    const DomainTypeInfo *find_domain_type(const std::string &name) const {
        auto it = domain_types_.find(name); return it == domain_types_.end() ? nullptr : &it->second;
    }
    bool drop_domain_type(const std::string &name) { return domain_types_.erase(name) > 0; }

    // ─── Documents (Phase 2) ───
    void add_document(const std::string &name, DocumentInfo info);
    DocumentInfo *find_document(const std::string &name);
    const DocumentInfo *find_document(const std::string &name) const;
    bool drop_document(const std::string &name);
    std::vector<std::string> list_documents() const;

    // ─── Scripts (Phase 3) ───
    void add_script(const std::string &name, ScriptInfo info);
    ScriptInfo *find_script(const std::string &name);
    const ScriptInfo *find_script(const std::string &name) const;
    bool drop_script(const std::string &name);
    std::vector<std::string> list_scripts() const;

    // ─── Triggers (Phase 3) ───
    void add_trigger(const std::string &name, TriggerInfo info);
    TriggerInfo *find_trigger(const std::string &name);
    const TriggerInfo *find_trigger(const std::string &name) const;
    bool drop_trigger(const std::string &name);
    std::vector<std::string> list_triggers() const;
    std::vector<const TriggerInfo *> triggers_for(const std::string &table,
                                                  TriggerEvent event,
                                                  TriggerTiming timing) const;

    // ─── Users & privileges (Phase 4) ───
    DatabaseMode db_mode() const { return db_mode_; }
    void set_db_mode(DatabaseMode m) { db_mode_ = m; }

    // Create a user, hashing `password` with PBKDF2-HMAC-SHA256 + a fresh salt.
    void add_user(const std::string &username, const std::string &password,
                  bool is_superuser);
    void add_user_raw(const std::string &username, UserInfo info);  // for dbfile load
    UserInfo *find_user(const std::string &username);
    const UserInfo *find_user(const std::string &username) const;
    bool drop_user(const std::string &username);
    std::vector<std::string> list_users() const;
    // Verify a plaintext password against the stored hash. False if no user.
    bool verify_password(const std::string &username, const std::string &password) const;
    // Re-hash and replace the stored password.
    bool set_password(const std::string &username, const std::string &password);

    void grant(const Privilege &p);
    void revoke(const std::string &grantee, const std::string &privilege,
                const std::string &object_kind, const std::string &object_name);
    // True if the user may perform `op` on (kind,name). Superusers and
    // ANONYMOUS mode always return true.
    bool has_privilege(const std::string &user, const std::string &op,
                       const std::string &object_kind,
                       const std::string &object_name) const;
    const std::vector<Privilege> &all_privileges() const { return privileges_; }
    void load_privileges(std::vector<Privilege> p) { privileges_ = std::move(p); }

    // ─── Schema helpers ───
    sql::Schema get_table_schema(const std::string &table_name) const;
    int find_column_index(const std::string &table_name, const std::string &col_name) const;
    sql::Schema get_matview_schema(const std::string &name) const;

    // ─── INFORMATION_SCHEMA ───
    std::pair<sql::Schema, std::vector<sql::Tuple>> get_information_schema(const std::string &view_name) const;

    // ─── Version history ───
    // Record an append-only history entry. `snapshot` is opaque bytes —
    // the dbfile layer owns the per-kind serialization format.
    void record_version(const std::string &kind, const std::string &name,
                        const ObjectVersion &v, std::string change_summary,
                        std::string serialized_snapshot);
    std::vector<VersionHistoryEntry> history_for(const std::string &kind,
                                                  const std::string &name) const;
    // Lookup a specific historic version. Returns nullptr if absent.
    const VersionHistoryEntry *get_version(const std::string &kind,
                                            const std::string &name,
                                            const ObjectVersion &v) const;
    const std::vector<VersionHistoryEntry> &all_history() const { return history_; }
    void load_history(std::vector<VersionHistoryEntry> h) { history_ = std::move(h); }

private:
    std::unordered_map<std::string, TableInfo> tables_;
    std::unordered_map<std::string, IndexInfo> indexes_;
    std::unordered_map<std::string, ViewInfo> views_;
    std::unordered_map<std::string, MaterializedViewInfo> matviews_;
    std::unordered_map<std::string, SavedQueryInfo> saved_queries_;
    std::unordered_map<std::string, TablespaceInfo> tablespaces_;
    std::unordered_map<std::string, SequenceInfo> sequences_;
    std::unordered_map<std::string, PreparedStmtInfo> prepared_;
    std::unordered_map<std::string, CompositeTypeInfo> composite_types_;
    std::unordered_map<std::string, EnumTypeInfo> enum_types_;
    std::unordered_map<std::string, DomainTypeInfo> domain_types_;
    std::unordered_map<std::string, DocumentInfo> documents_;
    std::unordered_map<std::string, ScriptInfo>   scripts_;
    std::unordered_map<std::string, TriggerInfo>  triggers_;
    std::unordered_map<std::string, UserInfo>     users_;
    std::vector<Privilege> privileges_;
    std::vector<VersionHistoryEntry> history_;
    DatabaseMode db_mode_ = DatabaseMode::ANONYMOUS;
};

} // namespace tdb::catalog

#endif // TDB_CATALOG_H
