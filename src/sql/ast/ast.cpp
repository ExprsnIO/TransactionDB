#include "tdb/sql/ast.h"
#include <sstream>

namespace tdb::sql::ast {

static void indent_str(std::ostringstream &os, int indent) {
    for (int i = 0; i < indent; i++) os << "  ";
}

static std::string stmt_type_name(StmtType type) {
    switch (type) {
        case StmtType::SELECT: return "SELECT";
        case StmtType::INSERT: return "INSERT";
        case StmtType::UPDATE: return "UPDATE";
        case StmtType::DELETE: return "DELETE";
        case StmtType::MERGE: return "MERGE";
        case StmtType::CREATE_TABLE: return "CREATE TABLE";
        case StmtType::CREATE_TYPE: return "CREATE TYPE";
        case StmtType::CREATE_DOMAIN: return "CREATE DOMAIN";
        case StmtType::ALTER_TYPE: return "ALTER TYPE";
        case StmtType::DROP_TYPE: return "DROP TYPE";
        case StmtType::DROP_DOMAIN: return "DROP DOMAIN";
        case StmtType::CREATE_INDEX: return "CREATE INDEX";
        case StmtType::CREATE_VIEW: return "CREATE VIEW";
        case StmtType::CREATE_SEQUENCE: return "CREATE SEQUENCE";
        case StmtType::CREATE_TRIGGER: return "CREATE TRIGGER";
        case StmtType::ALTER_TABLE: return "ALTER TABLE";
        case StmtType::DROP_TABLE: return "DROP TABLE";
        case StmtType::DROP_INDEX: return "DROP INDEX";
        case StmtType::DROP_VIEW: return "DROP VIEW";
        case StmtType::TRUNCATE: return "TRUNCATE";
        case StmtType::BEGIN_TXN: return "BEGIN";
        case StmtType::COMMIT_TXN: return "COMMIT";
        case StmtType::ROLLBACK_TXN: return "ROLLBACK";
        case StmtType::SAVEPOINT: return "SAVEPOINT";
        case StmtType::RELEASE_SAVEPOINT: return "RELEASE SAVEPOINT";
        case StmtType::EXPLAIN: return "EXPLAIN";
        case StmtType::SET_VARIABLE: return "SET";
        case StmtType::CREATE_MATERIALIZED_VIEW: return "CREATE MATERIALIZED VIEW";
        case StmtType::ALTER_MATERIALIZED_VIEW: return "ALTER MATERIALIZED VIEW";
        case StmtType::DROP_MATERIALIZED_VIEW: return "DROP MATERIALIZED VIEW";
        case StmtType::REFRESH_MATERIALIZED_VIEW: return "REFRESH MATERIALIZED VIEW";
        case StmtType::CREATE_TABLESPACE: return "CREATE TABLESPACE";
        case StmtType::DROP_TABLESPACE: return "DROP TABLESPACE";
        case StmtType::CREATE_SAVED_QUERY: return "CREATE SAVED QUERY";
        case StmtType::DROP_SAVED_QUERY: return "DROP SAVED QUERY";
        case StmtType::LOCK_STMT: return "LOCK";
        case StmtType::UNLOCK_STMT: return "UNLOCK";
        case StmtType::LOGGING_STMT: return "LOGGING";
        case StmtType::XPATH_QUERY: return "XPATH";
        case StmtType::XQUERY_QUERY: return "XQUERY";
        case StmtType::GRAPHQL_QUERY: return "GRAPHQL";
        // v1.2.0
        case StmtType::DROP_TRIGGER: return "DROP TRIGGER";
        case StmtType::DROP_SEQUENCE: return "DROP SEQUENCE";
        case StmtType::CREATE_PROCEDURE: return "CREATE PROCEDURE";
        case StmtType::CREATE_FUNCTION: return "CREATE FUNCTION";
        case StmtType::DROP_PROCEDURE: return "DROP PROCEDURE";
        case StmtType::DROP_FUNCTION: return "DROP FUNCTION";
        case StmtType::CALL_STMT: return "CALL";
        case StmtType::GRANT_STMT: return "GRANT";
        case StmtType::REVOKE_STMT: return "REVOKE";
        case StmtType::PREPARE_STMT: return "PREPARE";
        case StmtType::EXECUTE_STMT: return "EXECUTE";
        case StmtType::DEALLOCATE_STMT: return "DEALLOCATE";
        case StmtType::DECLARE_CURSOR: return "DECLARE CURSOR";
        case StmtType::OPEN_CURSOR: return "OPEN CURSOR";
        case StmtType::FETCH_CURSOR: return "FETCH";
        case StmtType::CLOSE_CURSOR: return "CLOSE CURSOR";
        case StmtType::GRAPH_MATCH: return "GRAPH MATCH";
    }
    return "UNKNOWN";
}

std::string ast_to_string(const Statement &stmt, int indent) {
    std::ostringstream os;
    indent_str(os, indent);
    os << "Statement(" << stmt_type_name(stmt.type) << ")";

    if (stmt.type == StmtType::SELECT) {
        auto &sel = std::get<SelectStmt>(stmt.data);
        os << "\n";
        indent_str(os, indent + 1);
        os << "columns: " << sel.select_list.size();
        if (sel.distinct) os << " DISTINCT";
        if (sel.where_clause) {
            os << "\n";
            indent_str(os, indent + 1);
            os << "WHERE: <expr>";
        }
        if (!sel.group_by.empty()) {
            os << "\n";
            indent_str(os, indent + 1);
            os << "GROUP BY: " << sel.group_by.size() << " exprs";
        }
        if (!sel.order_by.empty()) {
            os << "\n";
            indent_str(os, indent + 1);
            os << "ORDER BY: " << sel.order_by.size() << " items";
        }
    } else if (stmt.type == StmtType::CREATE_TABLE) {
        auto &ct = std::get<CreateTableStmt>(stmt.data);
        os << " " << ct.name;
        os << "\n";
        indent_str(os, indent + 1);
        os << "columns: " << ct.columns.size();
        os << ", constraints: " << ct.constraints.size();
        if (ct.encrypted) os << " [ENCRYPTED]";
        if (ct.temporary) os << " [TEMPORARY]";
    } else if (stmt.type == StmtType::INSERT) {
        auto &is = std::get<InsertStmt>(stmt.data);
        os << " INTO " << is.table;
        os << "\n";
        indent_str(os, indent + 1);
        os << "columns: " << is.columns.size();
        os << ", rows: " << is.values.size();
    } else if (stmt.type == StmtType::CREATE_INDEX) {
        auto &ci = std::get<CreateIndexStmt>(stmt.data);
        os << " " << ci.name << " ON " << ci.table;
        if (ci.unique) os << " [UNIQUE]";
    }

    return os.str();
}

} // namespace tdb::sql::ast
