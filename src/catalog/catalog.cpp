#include "tdb/catalog.h"
#include "tdb/crypto.h"
#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace tdb::catalog {

// ─── Tables ───
void Catalog::add_table(const std::string &name, TableInfo info) {
    tables_[name] = std::move(info);
}

TableInfo *Catalog::find_table(const std::string &name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return nullptr;
    return &it->second;
}

const TableInfo *Catalog::find_table(const std::string &name) const {
    auto it = tables_.find(name);
    if (it == tables_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_table(const std::string &name) {
    return tables_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_tables() const {
    std::vector<std::string> names;
    for (auto &[k, v] : tables_) { (void)v; names.push_back(k); }
    return names;
}

// ─── Indexes ───
void Catalog::add_index(const std::string &name, IndexInfo info) {
    indexes_[name] = std::move(info);
}

const IndexInfo *Catalog::find_index(const std::string &name) const {
    auto it = indexes_.find(name);
    if (it == indexes_.end()) return nullptr;
    return &it->second;
}

// ─── Views ───
void Catalog::add_view(const std::string &name, ViewInfo info) {
    views_[name] = std::move(info);
}

ViewInfo *Catalog::find_view(const std::string &name) {
    auto it = views_.find(name);
    if (it == views_.end()) return nullptr;
    return &it->second;
}

const ViewInfo *Catalog::find_view(const std::string &name) const {
    auto it = views_.find(name);
    if (it == views_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_view(const std::string &name) {
    return views_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_views() const {
    std::vector<std::string> names;
    for (auto &[k, v] : views_) { (void)v; names.push_back(k); }
    return names;
}

// ─── Materialized Views ───
void Catalog::add_materialized_view(const std::string &name, MaterializedViewInfo info) {
    matviews_[name] = std::move(info);
}

MaterializedViewInfo *Catalog::find_materialized_view(const std::string &name) {
    auto it = matviews_.find(name);
    if (it == matviews_.end()) return nullptr;
    return &it->second;
}

const MaterializedViewInfo *Catalog::find_materialized_view(const std::string &name) const {
    auto it = matviews_.find(name);
    if (it == matviews_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_materialized_view(const std::string &name) {
    return matviews_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_materialized_views() const {
    std::vector<std::string> names;
    for (auto &[k, v] : matviews_) { (void)v; names.push_back(k); }
    return names;
}

sql::Schema Catalog::get_matview_schema(const std::string &name) const {
    auto *mv = find_materialized_view(name);
    if (!mv) return {};
    sql::Schema schema;
    for (auto &col : mv->columns) {
        schema.push_back({col.name, name});
    }
    return schema;
}

// ─── Saved Queries ───
void Catalog::add_saved_query(const std::string &name, SavedQueryInfo info) {
    saved_queries_[name] = std::move(info);
}

SavedQueryInfo *Catalog::find_saved_query(const std::string &name) {
    auto it = saved_queries_.find(name);
    if (it == saved_queries_.end()) return nullptr;
    return &it->second;
}

const SavedQueryInfo *Catalog::find_saved_query(const std::string &name) const {
    auto it = saved_queries_.find(name);
    if (it == saved_queries_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_saved_query(const std::string &name) {
    return saved_queries_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_saved_queries() const {
    std::vector<std::string> names;
    for (auto &[k, v] : saved_queries_) { (void)v; names.push_back(k); }
    return names;
}

// ─── Tablespaces ───
void Catalog::add_tablespace(const std::string &name, TablespaceInfo info) {
    tablespaces_[name] = std::move(info);
}

const TablespaceInfo *Catalog::find_tablespace(const std::string &name) const {
    auto it = tablespaces_.find(name);
    if (it == tablespaces_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_tablespace(const std::string &name) {
    return tablespaces_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_tablespaces() const {
    std::vector<std::string> names;
    for (auto &[k, v] : tablespaces_) { (void)v; names.push_back(k); }
    return names;
}

// ─── Schema Helpers ───
sql::Schema Catalog::get_table_schema(const std::string &table_name) const {
    auto *t = find_table(table_name);
    if (!t) return {};
    sql::Schema schema;
    for (auto &col : t->columns) {
        schema.push_back({col.name, table_name});
    }
    return schema;
}

int Catalog::find_column_index(const std::string &table_name, const std::string &col_name) const {
    auto *t = find_table(table_name);
    if (!t) return -1;
    for (size_t i = 0; i < t->columns.size(); i++) {
        if (t->columns[i].name == col_name) return (int)i;
    }
    return -1;
}

// ─── Indexes (drop / list) ───
bool Catalog::drop_index(const std::string &name) {
    return indexes_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_indexes() const {
    std::vector<std::string> names;
    for (auto &[k, v] : indexes_) { (void)v; names.push_back(k); }
    return names;
}

// ─── Sequences ───
void Catalog::add_sequence(const std::string &name, SequenceInfo info) {
    sequences_[name] = std::move(info);
}

SequenceInfo *Catalog::find_sequence(const std::string &name) {
    auto it = sequences_.find(name);
    if (it == sequences_.end()) return nullptr;
    return &it->second;
}

const SequenceInfo *Catalog::find_sequence(const std::string &name) const {
    auto it = sequences_.find(name);
    if (it == sequences_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_sequence(const std::string &name) {
    return sequences_.erase(name) > 0;
}

int64_t Catalog::nextval(const std::string &name) {
    auto *seq = find_sequence(name);
    if (!seq) throw std::runtime_error("sequence not found: " + name);
    seq->current_value += seq->increment;
    if (seq->current_value > seq->max_value) {
        if (seq->cycle) seq->current_value = seq->min_value;
        else throw std::runtime_error("sequence " + name + " reached max value");
    }
    return seq->current_value;
}

int64_t Catalog::currval(const std::string &name) const {
    auto *seq = find_sequence(name);
    if (!seq) throw std::runtime_error("sequence not found: " + name);
    return seq->current_value;
}

// ─── Prepared Statements ───
void Catalog::add_prepared(const std::string &name, PreparedStmtInfo info) {
    prepared_[name] = std::move(info);
}

PreparedStmtInfo *Catalog::find_prepared(const std::string &name) {
    auto it = prepared_.find(name);
    if (it == prepared_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_prepared(const std::string &name) {
    return prepared_.erase(name) > 0;
}

// ─── Documents ───

void Catalog::add_document(const std::string &name, DocumentInfo info) {
    documents_[name] = std::move(info);
}

DocumentInfo *Catalog::find_document(const std::string &name) {
    auto it = documents_.find(name);
    if (it == documents_.end()) return nullptr;
    return &it->second;
}

const DocumentInfo *Catalog::find_document(const std::string &name) const {
    auto it = documents_.find(name);
    if (it == documents_.end()) return nullptr;
    return &it->second;
}

bool Catalog::drop_document(const std::string &name) {
    return documents_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_documents() const {
    std::vector<std::string> out;
    out.reserve(documents_.size());
    for (auto &[n, _] : documents_) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

// ─── Scripts ───

void Catalog::add_script(const std::string &name, ScriptInfo info) {
    scripts_[name] = std::move(info);
}

ScriptInfo *Catalog::find_script(const std::string &name) {
    auto it = scripts_.find(name);
    return it == scripts_.end() ? nullptr : &it->second;
}

const ScriptInfo *Catalog::find_script(const std::string &name) const {
    auto it = scripts_.find(name);
    return it == scripts_.end() ? nullptr : &it->second;
}

bool Catalog::drop_script(const std::string &name) {
    return scripts_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_scripts() const {
    std::vector<std::string> out;
    out.reserve(scripts_.size());
    for (auto &[n, _] : scripts_) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

// ─── Triggers ───

void Catalog::add_trigger(const std::string &name, TriggerInfo info) {
    triggers_[name] = std::move(info);
}

TriggerInfo *Catalog::find_trigger(const std::string &name) {
    auto it = triggers_.find(name);
    return it == triggers_.end() ? nullptr : &it->second;
}

const TriggerInfo *Catalog::find_trigger(const std::string &name) const {
    auto it = triggers_.find(name);
    return it == triggers_.end() ? nullptr : &it->second;
}

bool Catalog::drop_trigger(const std::string &name) {
    return triggers_.erase(name) > 0;
}

std::vector<std::string> Catalog::list_triggers() const {
    std::vector<std::string> out;
    out.reserve(triggers_.size());
    for (auto &[n, _] : triggers_) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const TriggerInfo *> Catalog::triggers_for(const std::string &table,
                                                      TriggerEvent event,
                                                      TriggerTiming timing) const {
    std::vector<const TriggerInfo *> out;
    for (auto &[_, t] : triggers_) {
        if (t.table == table && t.event == event && t.timing == timing) out.push_back(&t);
    }
    return out;
}

// ─── Users & privileges ───

void Catalog::add_user(const std::string &username, const std::string &password,
                       bool is_superuser) {
    UserInfo u;
    u.username = username;
    u.is_superuser = is_superuser;
    u.kdf_iterations = 100000;
    tdb_crypto_random_bytes(u.salt, sizeof(u.salt));
    tdb_pbkdf2_sha256(password.data(), password.size(),
                      u.salt, sizeof(u.salt), u.kdf_iterations,
                      u.password_hash, sizeof(u.password_hash));
    users_[username] = std::move(u);
}

void Catalog::add_user_raw(const std::string &username, UserInfo info) {
    users_[username] = std::move(info);
}

UserInfo *Catalog::find_user(const std::string &username) {
    auto it = users_.find(username);
    return it == users_.end() ? nullptr : &it->second;
}

const UserInfo *Catalog::find_user(const std::string &username) const {
    auto it = users_.find(username);
    return it == users_.end() ? nullptr : &it->second;
}

bool Catalog::drop_user(const std::string &username) {
    // Also revoke all privileges held by the dropped user.
    privileges_.erase(
        std::remove_if(privileges_.begin(), privileges_.end(),
                       [&](const Privilege &p){ return p.grantee == username; }),
        privileges_.end());
    return users_.erase(username) > 0;
}

std::vector<std::string> Catalog::list_users() const {
    std::vector<std::string> out;
    out.reserve(users_.size());
    for (auto &[n, _] : users_) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

bool Catalog::verify_password(const std::string &username,
                              const std::string &password) const {
    auto it = users_.find(username);
    if (it == users_.end()) return false;
    const UserInfo &u = it->second;
    uint8_t computed[32];
    tdb_pbkdf2_sha256(password.data(), password.size(),
                      u.salt, sizeof(u.salt), u.kdf_iterations,
                      computed, sizeof(computed));
    // Constant-time-ish comparison.
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(computed); i++)
        diff |= (uint8_t)(computed[i] ^ u.password_hash[i]);
    return diff == 0;
}

bool Catalog::set_password(const std::string &username,
                           const std::string &password) {
    auto it = users_.find(username);
    if (it == users_.end()) return false;
    UserInfo &u = it->second;
    tdb_crypto_random_bytes(u.salt, sizeof(u.salt));
    tdb_pbkdf2_sha256(password.data(), password.size(),
                      u.salt, sizeof(u.salt), u.kdf_iterations,
                      u.password_hash, sizeof(u.password_hash));
    return true;
}

void Catalog::grant(const Privilege &p) {
    // Replace an existing identical grant rather than duplicating.
    for (auto &existing : privileges_) {
        if (existing.grantee == p.grantee && existing.privilege == p.privilege &&
            existing.object_kind == p.object_kind &&
            existing.object_name == p.object_name) {
            existing.with_grant_option = p.with_grant_option;
            return;
        }
    }
    privileges_.push_back(p);
}

void Catalog::revoke(const std::string &grantee, const std::string &privilege,
                     const std::string &object_kind,
                     const std::string &object_name) {
    privileges_.erase(
        std::remove_if(privileges_.begin(), privileges_.end(),
            [&](const Privilege &p){
                return p.grantee == grantee && p.privilege == privilege &&
                       p.object_kind == object_kind &&
                       p.object_name == object_name;
            }),
        privileges_.end());
}

bool Catalog::has_privilege(const std::string &user, const std::string &op,
                            const std::string &object_kind,
                            const std::string &object_name) const {
    // ANONYMOUS mode: unrestricted.
    if (db_mode_ == DatabaseMode::ANONYMOUS) return true;
    // Superusers bypass all checks.
    auto it = users_.find(user);
    if (it != users_.end() && it->second.is_superuser) return true;

    for (const auto &p : privileges_) {
        if (p.grantee != user) continue;
        bool priv_ok  = (p.privilege == op || p.privilege == "ALL");
        bool kind_ok  = (p.object_kind == object_kind || p.object_kind == "*");
        bool name_ok  = (p.object_name == object_name || p.object_name == "*");
        if (priv_ok && kind_ok && name_ok) return true;
    }
    return false;
}

// ─── INFORMATION_SCHEMA ───

static std::string index_method_to_string(sql::ast::IndexMethod m) {
    switch (m) {
        case sql::ast::IndexMethod::BTREE:  return "BTREE";
        case sql::ast::IndexMethod::BPTREE: return "BPTREE";
        case sql::ast::IndexMethod::HASH:   return "HASH";
        case sql::ast::IndexMethod::RTREE:  return "RTREE";
        case sql::ast::IndexMethod::RPTREE: return "RPTREE";
        case sql::ast::IndexMethod::GIST:   return "GIST";
    }
    return "UNKNOWN";
}

std::pair<sql::Schema, std::vector<sql::Tuple>>
Catalog::get_information_schema(const std::string &view_name) const {
    using V = sql::Value;
    const std::string catalog_name = "tdb";
    const std::string default_schema = "public";

    if (view_name == "TABLES") {
        sql::Schema schema = {
            {"table_catalog", "TABLES"},
            {"table_schema",  "TABLES"},
            {"table_name",    "TABLES"},
            {"table_type",    "TABLES"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[name, ti] : tables_) {
            std::string s = ti.schema.empty() ? default_schema : ti.schema;
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(s),
                V::make_string(name),
                V::make_string("BASE TABLE"),
            });
        }
        for (auto &[name, vi] : views_) {
            std::string s = vi.schema.empty() ? default_schema : vi.schema;
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(s),
                V::make_string(name),
                V::make_string("VIEW"),
            });
        }
        for (auto &[name, mv] : matviews_) {
            std::string s = mv.schema.empty() ? default_schema : mv.schema;
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(s),
                V::make_string(name),
                V::make_string("MATERIALIZED VIEW"),
            });
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            return a[2].str_val < b[2].str_val;
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "COLUMNS") {
        sql::Schema schema = {
            {"table_catalog",   "COLUMNS"},
            {"table_schema",    "COLUMNS"},
            {"table_name",      "COLUMNS"},
            {"column_name",     "COLUMNS"},
            {"ordinal_position","COLUMNS"},
            {"column_default",  "COLUMNS"},
            {"is_nullable",     "COLUMNS"},
            {"data_type",       "COLUMNS"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[tname, ti] : tables_) {
            std::string s = ti.schema.empty() ? default_schema : ti.schema;
            for (size_t i = 0; i < ti.columns.size(); ++i) {
                auto &col = ti.columns[i];
                rows.push_back({
                    V::make_string(catalog_name),
                    V::make_string(s),
                    V::make_string(tname),
                    V::make_string(col.name),
                    V::make_string(std::to_string(i + 1)),
                    V::make_string(""),  // column_default (simplified)
                    V::make_string(col.nullable ? "YES" : "NO"),
                    V::make_string(col.type.name),
                });
            }
        }
        for (auto &[mvname, mv] : matviews_) {
            std::string s = mv.schema.empty() ? default_schema : mv.schema;
            for (size_t i = 0; i < mv.columns.size(); ++i) {
                auto &col = mv.columns[i];
                rows.push_back({
                    V::make_string(catalog_name),
                    V::make_string(s),
                    V::make_string(mvname),
                    V::make_string(col.name),
                    V::make_string(std::to_string(i + 1)),
                    V::make_string(""),
                    V::make_string(col.nullable ? "YES" : "NO"),
                    V::make_string(col.type.name),
                });
            }
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            if (a[2].str_val != b[2].str_val) return a[2].str_val < b[2].str_val;
            return a[4].str_val < b[4].str_val;  // ordinal_position (string compare OK for single digits)
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "VIEWS") {
        sql::Schema schema = {
            {"table_catalog",   "VIEWS"},
            {"table_schema",    "VIEWS"},
            {"table_name",      "VIEWS"},
            {"view_definition", "VIEWS"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[name, vi] : views_) {
            std::string s = vi.schema.empty() ? default_schema : vi.schema;
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(s),
                V::make_string(name),
                V::make_string(vi.query_text),
            });
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            return a[2].str_val < b[2].str_val;
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "INDEXES") {
        sql::Schema schema = {
            {"index_name",  "INDEXES"},
            {"table_name",  "INDEXES"},
            {"index_type",  "INDEXES"},
            {"is_unique",   "INDEXES"},
            {"column_name", "INDEXES"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[iname, ii] : indexes_) {
            std::string method = index_method_to_string(ii.method);
            std::string uniq = ii.unique ? "YES" : "NO";
            for (auto &col : ii.columns) {
                rows.push_back({
                    V::make_string(iname),
                    V::make_string(ii.table),
                    V::make_string(method),
                    V::make_string(uniq),
                    V::make_string(col),
                });
            }
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            if (a[0].str_val != b[0].str_val) return a[0].str_val < b[0].str_val;
            return a[4].str_val < b[4].str_val;
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "SCHEMATA") {
        sql::Schema schema = {
            {"catalog_name", "SCHEMATA"},
            {"schema_name",  "SCHEMATA"},
            {"schema_owner", "SCHEMATA"},
        };
        std::vector<sql::Tuple> rows;

        // Collect unique schema names from all catalog objects
        std::vector<std::string> schema_names;
        auto add_if_new = [&](const std::string &s) {
            std::string resolved = s.empty() ? default_schema : s;
            if (std::find(schema_names.begin(), schema_names.end(), resolved) == schema_names.end()) {
                schema_names.push_back(resolved);
            }
        };

        // Always include "public"
        add_if_new(default_schema);
        for (auto &[_, ti] : tables_) add_if_new(ti.schema);
        for (auto &[_, vi] : views_) add_if_new(vi.schema);
        for (auto &[_, mv] : matviews_) add_if_new(mv.schema);
        for (auto &[_, sq] : saved_queries_) add_if_new(sq.schema);
        for (auto &[_, si] : sequences_) add_if_new(si.schema);

        std::sort(schema_names.begin(), schema_names.end());
        for (auto &sn : schema_names) {
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(sn),
                V::make_string(""),  // schema_owner not tracked
            });
        }
        return {schema, std::move(rows)};
    }

    if (view_name == "TABLE_CONSTRAINTS") {
        sql::Schema schema = {
            {"constraint_name", "TABLE_CONSTRAINTS"},
            {"table_name",      "TABLE_CONSTRAINTS"},
            {"constraint_type", "TABLE_CONSTRAINTS"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[tname, ti] : tables_) {
            // Primary key constraint
            if (!ti.primary_key_cols.empty()) {
                rows.push_back({
                    V::make_string(tname + "_pkey"),
                    V::make_string(tname),
                    V::make_string("PRIMARY KEY"),
                });
            }
            // Per-column unique constraints
            for (auto &col : ti.columns) {
                if (col.unique) {
                    rows.push_back({
                        V::make_string(tname + "_" + col.name + "_key"),
                        V::make_string(tname),
                        V::make_string("UNIQUE"),
                    });
                }
                if (col.check_expr) {
                    rows.push_back({
                        V::make_string(tname + "_" + col.name + "_check"),
                        V::make_string(tname),
                        V::make_string("CHECK"),
                    });
                }
            }
            // Foreign key constraints
            for (auto &fk : ti.foreign_keys) {
                std::string fk_name = fk.name.empty()
                    ? (tname + "_" + fk.columns.front() + "_fkey")
                    : fk.name;
                rows.push_back({
                    V::make_string(fk_name),
                    V::make_string(tname),
                    V::make_string("FOREIGN KEY"),
                });
            }
        }

        // Unique indexes also imply constraints
        for (auto &[iname, ii] : indexes_) {
            if (ii.unique) {
                rows.push_back({
                    V::make_string(iname),
                    V::make_string(ii.table),
                    V::make_string("UNIQUE"),
                });
            }
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            if (a[1].str_val != b[1].str_val) return a[1].str_val < b[1].str_val;
            return a[0].str_val < b[0].str_val;
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "KEY_COLUMN_USAGE") {
        sql::Schema schema = {
            {"constraint_name",  "KEY_COLUMN_USAGE"},
            {"table_name",       "KEY_COLUMN_USAGE"},
            {"column_name",      "KEY_COLUMN_USAGE"},
            {"ordinal_position", "KEY_COLUMN_USAGE"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[tname, ti] : tables_) {
            // Primary key columns
            if (!ti.primary_key_cols.empty()) {
                std::string cname = tname + "_pkey";
                for (size_t i = 0; i < ti.primary_key_cols.size(); ++i) {
                    rows.push_back({
                        V::make_string(cname),
                        V::make_string(tname),
                        V::make_string(ti.primary_key_cols[i]),
                        V::make_string(std::to_string(i + 1)),
                    });
                }
            }
            // Per-column unique constraints
            for (auto &col : ti.columns) {
                if (col.unique) {
                    rows.push_back({
                        V::make_string(tname + "_" + col.name + "_key"),
                        V::make_string(tname),
                        V::make_string(col.name),
                        V::make_string("1"),
                    });
                }
            }
            // Foreign key columns
            for (auto &fk : ti.foreign_keys) {
                std::string fk_name = fk.name.empty()
                    ? (tname + "_" + fk.columns.front() + "_fkey")
                    : fk.name;
                for (size_t i = 0; i < fk.columns.size(); ++i) {
                    rows.push_back({
                        V::make_string(fk_name),
                        V::make_string(tname),
                        V::make_string(fk.columns[i]),
                        V::make_string(std::to_string(i + 1)),
                    });
                }
            }
        }

        // Unique indexes
        for (auto &[iname, ii] : indexes_) {
            if (ii.unique) {
                for (size_t i = 0; i < ii.columns.size(); ++i) {
                    rows.push_back({
                        V::make_string(iname),
                        V::make_string(ii.table),
                        V::make_string(ii.columns[i]),
                        V::make_string(std::to_string(i + 1)),
                    });
                }
            }
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            if (a[1].str_val != b[1].str_val) return a[1].str_val < b[1].str_val;
            if (a[0].str_val != b[0].str_val) return a[0].str_val < b[0].str_val;
            return a[3].str_val < b[3].str_val;
        });
        return {schema, std::move(rows)};
    }

    if (view_name == "SEQUENCES") {
        sql::Schema schema = {
            {"sequence_catalog",  "SEQUENCES"},
            {"sequence_schema",   "SEQUENCES"},
            {"sequence_name",     "SEQUENCES"},
            {"start_value",       "SEQUENCES"},
            {"minimum_value",     "SEQUENCES"},
            {"maximum_value",     "SEQUENCES"},
            {"increment",         "SEQUENCES"},
            {"cycle_option",      "SEQUENCES"},
        };
        std::vector<sql::Tuple> rows;

        for (auto &[sname, si] : sequences_) {
            std::string s = si.schema.empty() ? default_schema : si.schema;
            rows.push_back({
                V::make_string(catalog_name),
                V::make_string(s),
                V::make_string(sname),
                V::make_string(std::to_string(si.start)),
                V::make_string(std::to_string(si.min_value)),
                V::make_string(std::to_string(si.max_value)),
                V::make_string(std::to_string(si.increment)),
                V::make_string(si.cycle ? "YES" : "NO"),
            });
        }

        std::sort(rows.begin(), rows.end(), [](const sql::Tuple &a, const sql::Tuple &b) {
            return a[2].str_val < b[2].str_val;
        });
        return {schema, std::move(rows)};
    }

    throw std::runtime_error("Unknown INFORMATION_SCHEMA view: " + view_name);
}

// ─── Version history ───

void Catalog::record_version(const std::string &kind, const std::string &name,
                             const ObjectVersion &v, std::string change_summary,
                             std::string serialized_snapshot) {
    VersionHistoryEntry e;
    e.object_kind = kind;
    e.object_name = name;
    e.version = v;
    e.timestamp_ms = ObjectVersion::now_ms();
    e.change_summary = std::move(change_summary);
    e.serialized_snapshot = std::move(serialized_snapshot);
    history_.push_back(std::move(e));
}

std::vector<VersionHistoryEntry> Catalog::history_for(const std::string &kind,
                                                      const std::string &name) const {
    std::vector<VersionHistoryEntry> out;
    for (auto &e : history_) {
        if (e.object_kind == kind && e.object_name == name) out.push_back(e);
    }
    return out;
}

const VersionHistoryEntry *Catalog::get_version(const std::string &kind,
                                                const std::string &name,
                                                const ObjectVersion &v) const {
    for (auto &e : history_) {
        if (e.object_kind == kind && e.object_name == name && e.version.equals(v))
            return &e;
    }
    return nullptr;
}

} // namespace tdb::catalog
