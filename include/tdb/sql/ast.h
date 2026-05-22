#ifndef TDB_SQL_AST_H
#define TDB_SQL_AST_H

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include <unordered_map>
#include "tdb/sql/token.h"

namespace tdb::sql::ast {

// Forward declarations
struct Expr;
struct SelectStmt;
struct Statement;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Statement>;
using SelectPtr = std::unique_ptr<SelectStmt>;

// ─── Column Reference ───
struct ColumnRef {
    std::optional<std::string> schema;
    std::optional<std::string> table;
    std::string column;  // "*" for wildcard
};

// ─── Data Type ───
struct DataType {
    std::string name;  // INT, VARCHAR, GEOMETRY, etc.
    std::optional<int> precision;
    std::optional<int> scale;
    bool is_array = false;
    std::optional<int> srid;  // for spatial types
};

// ─── Expression Nodes ───
enum class ExprType {
    LITERAL,
    COLUMN_REF,
    UNARY_OP,
    BINARY_OP,
    FUNCTION_CALL,
    AGGREGATE_CALL,
    WINDOW_CALL,
    SUBQUERY,
    CASE_EXPR,
    CAST_EXPR,
    IN_EXPR,
    BETWEEN_EXPR,
    LIKE_EXPR,
    IS_NULL_EXPR,
    EXISTS_EXPR,
    PARAMETER,       // ? or $1
};

struct Literal {
    TokenType token_type;
    std::string value;
};

struct UnaryOp {
    TokenType op;   // MINUS, KW_NOT
    ExprPtr operand;
};

struct BinaryOp {
    TokenType op;
    ExprPtr left;
    ExprPtr right;
};

struct FunctionCall {
    std::string name;
    std::vector<ExprPtr> args;
    bool distinct = false;
};

struct AggregateCall {
    std::string name;  // COUNT, SUM, AVG, MIN, MAX
    std::vector<ExprPtr> args;
    bool distinct = false;
    ExprPtr filter;    // FILTER (WHERE ...)
};

// ─── Window Specification ───
struct WindowFrame {
    enum class Type { ROWS, RANGE, GROUPS };
    enum class Bound { UNBOUNDED_PRECEDING, CURRENT_ROW, UNBOUNDED_FOLLOWING, PRECEDING, FOLLOWING };

    Type type = Type::RANGE;
    Bound start = Bound::UNBOUNDED_PRECEDING;
    Bound end = Bound::CURRENT_ROW;
    ExprPtr start_offset;
    ExprPtr end_offset;
};

struct WindowSpec {
    std::optional<std::string> ref_name;
    std::vector<ExprPtr> partition_by;
    std::vector<std::pair<ExprPtr, bool>> order_by;  // expr, is_asc
    std::optional<WindowFrame> frame;
};

struct WindowCall {
    std::string name;
    std::vector<ExprPtr> args;
    bool distinct = false;
    WindowSpec window;
};

struct CaseExpr {
    ExprPtr operand;   // simple CASE: the value being compared
    std::vector<std::pair<ExprPtr, ExprPtr>> when_clauses;
    ExprPtr else_clause;
};

struct CastExpr {
    ExprPtr operand;
    DataType target_type;
};

struct InExpr {
    ExprPtr operand;
    bool negated = false;
    std::variant<std::vector<ExprPtr>, SelectPtr> values;
};

struct BetweenExpr {
    ExprPtr operand;
    bool negated = false;
    ExprPtr low;
    ExprPtr high;
};

struct LikeExpr {
    ExprPtr operand;
    bool negated = false;
    bool ilike = false;
    ExprPtr pattern;
    ExprPtr escape;
};

struct IsNullExpr {
    ExprPtr operand;
    bool negated = false;  // IS NOT NULL
};

struct ExistsExpr {
    SelectPtr subquery;
};

struct SubqueryExpr {
    SelectPtr query;
};

struct Expr {
    ExprType type;
    std::string alias;

    std::variant<
        Literal,
        ColumnRef,
        UnaryOp,
        BinaryOp,
        FunctionCall,
        AggregateCall,
        WindowCall,
        SubqueryExpr,
        CaseExpr,
        CastExpr,
        InExpr,
        BetweenExpr,
        LikeExpr,
        IsNullExpr,
        ExistsExpr
    > data;
};

// ─── Table References ───
enum class TableRefType {
    TABLE,
    SUBQUERY,
    JOIN,
    LATERAL,
    FUNCTION,
};

struct TableRef;
using TableRefPtr = std::unique_ptr<TableRef>;

enum class JoinType {
    INNER, LEFT, RIGHT, FULL, CROSS, NATURAL,
};

struct JoinClause {
    JoinType type;
    TableRefPtr right;
    ExprPtr on_condition;
    std::vector<std::string> using_columns;
};

struct TableRef {
    TableRefType type;
    std::optional<std::string> schema;
    std::string name;
    std::string alias;
    SelectPtr subquery;
    JoinClause join;
};

// ─── ORDER BY item ───
struct OrderByItem {
    ExprPtr expr;
    bool ascending = true;
    enum class NullOrder { DEFAULT, FIRST, LAST };
    NullOrder null_order = NullOrder::DEFAULT;
};

// ─── Common Table Expression ───
struct CTE {
    std::string name;
    std::vector<std::string> columns;
    SelectPtr query;
    bool recursive = false;
};

// ─── SELECT Statement ───
struct SelectStmt {
    bool distinct = false;
    std::vector<ExprPtr> select_list;
    std::vector<TableRefPtr> from;
    ExprPtr where_clause;
    std::vector<ExprPtr> group_by;
    // GROUPING SETS / CUBE / ROLLUP support
    enum class GroupByMode { SIMPLE, GROUPING_SETS, CUBE, ROLLUP };
    GroupByMode group_by_mode = GroupByMode::SIMPLE;
    // Each set is a vector of indices into group_by
    std::vector<std::vector<int>> grouping_sets;
    ExprPtr having;
    std::vector<OrderByItem> order_by;
    ExprPtr limit;
    ExprPtr offset;

    // Set operations
    enum class SetOp { NONE, UNION, UNION_ALL, INTERSECT, INTERSECT_ALL, EXCEPT, EXCEPT_ALL };
    SetOp set_op = SetOp::NONE;
    SelectPtr set_right;

    // CTEs
    std::vector<CTE> ctes;

    // Window definitions: WINDOW w AS (...)
    std::vector<std::pair<std::string, WindowSpec>> window_defs;
};

// ─── Column Definition (for CREATE TABLE) ───
struct ColumnDef {
    std::string name;
    DataType type;
    bool nullable = true;
    ExprPtr default_value;
    bool primary_key = false;
    bool unique = false;
    bool auto_increment = false;
    bool encrypted = false;
    std::optional<std::string> collation;
    ExprPtr check_expr;
    // Calculated/generated column: GENERATED ALWAYS AS (expr) [STORED | VIRTUAL]
    bool generated = false;
    ExprPtr generated_expr;
    enum class GeneratedMode { STORED, VIRTUAL };
    GeneratedMode generated_mode = GeneratedMode::STORED;
};

// ─── Table Constraint ───
struct TableConstraint {
    enum class Type { PRIMARY_KEY, UNIQUE, FOREIGN_KEY, CHECK };
    Type type;
    std::optional<std::string> name;
    std::vector<std::string> columns;
    // Foreign key
    std::string ref_table;
    std::vector<std::string> ref_columns;
    std::string on_delete;  // CASCADE, RESTRICT, SET NULL, etc.
    std::string on_update;
    // Check
    ExprPtr check_expr;
};

// ─── Index Definition ───
enum class IndexMethod { BTREE, BPTREE, HASH, RTREE, RPTREE, GIST };

struct IndexColumn {
    std::string name;
    bool ascending = true;
    std::optional<std::string> opclass;
};

// ─── MERGE clause ───
struct MergeWhenClause {
    bool matched;
    ExprPtr condition;
    enum class Action { INSERT, UPDATE, DELETE };
    Action action;
    std::vector<std::string> columns;          // for INSERT/UPDATE
    std::vector<ExprPtr> values;               // for INSERT
    std::vector<std::pair<std::string, ExprPtr>> assignments;  // for UPDATE
};

// ─── Lock Granularity ───
enum class LockGranularity { ROW, OBJECT, TABLE, PAGE, DATABASE, ADVISORY };
enum class LockMode {
    ACCESS_SHARE, ROW_SHARE, ROW_EXCLUSIVE, SHARE_UPDATE_EXCLUSIVE,
    SHARE, SHARE_ROW_EXCLUSIVE, EXCLUSIVE, ACCESS_EXCLUSIVE,
    INTENT_SHARED, INTENT_EXCLUSIVE
};

// ─── Tablespace Definition ───
struct TablespaceInfo {
    std::string name;
    std::string location;  // filesystem path
    std::optional<std::string> owner;
    std::unordered_map<std::string, std::string> options;
};

// ─── Statements ───
enum class StmtType {
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    MERGE,
    CREATE_TABLE,
    CREATE_TYPE,
    CREATE_DOMAIN,
    ALTER_TYPE,
    CREATE_INDEX,
    CREATE_VIEW,
    CREATE_MATERIALIZED_VIEW,
    CREATE_SEQUENCE,
    CREATE_TRIGGER,
    CREATE_TABLESPACE,
    CREATE_SAVED_QUERY,
    ALTER_TABLE,
    ALTER_MATERIALIZED_VIEW,
    DROP_TABLE,
    DROP_TYPE,
    DROP_DOMAIN,
    DROP_INDEX,
    DROP_VIEW,
    DROP_MATERIALIZED_VIEW,
    DROP_TABLESPACE,
    DROP_SAVED_QUERY,
    DROP_TRIGGER,
    DROP_SEQUENCE,
    REFRESH_MATERIALIZED_VIEW,
    TRUNCATE,
    BEGIN_TXN,
    COMMIT_TXN,
    ROLLBACK_TXN,
    SAVEPOINT,
    RELEASE_SAVEPOINT,
    EXPLAIN,
    SET_VARIABLE,
    LOCK_STMT,
    UNLOCK_STMT,
    LOGGING_STMT,
    XPATH_QUERY,
    XQUERY_QUERY,
    GRAPHQL_QUERY,
    // ─── v1.2.0: New statement types ───
    CREATE_PROCEDURE,
    CREATE_FUNCTION,
    DROP_PROCEDURE,
    DROP_FUNCTION,
    CALL_STMT,
    GRANT_STMT,
    REVOKE_STMT,
    PREPARE_STMT,
    EXECUTE_STMT,
    DEALLOCATE_STMT,
    DECLARE_CURSOR,
    OPEN_CURSOR,
    FETCH_CURSOR,
    CLOSE_CURSOR,
    GRAPH_MATCH,
};

struct InsertStmt {
    std::string table;
    std::optional<std::string> schema;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> values;  // multi-row insert
    SelectPtr select;                          // INSERT ... SELECT
    std::string on_conflict;                   // ON CONFLICT handling: "DO NOTHING" or "DO UPDATE"
    std::vector<std::pair<std::string, ExprPtr>> on_conflict_updates; // SET col=expr for DO UPDATE
    std::vector<ExprPtr> returning;            // RETURNING expressions
};

struct UpdateStmt {
    std::string table;
    std::optional<std::string> schema;
    std::string alias;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    std::vector<TableRefPtr> from;
    ExprPtr where_clause;
    std::vector<ExprPtr> returning;            // RETURNING expressions
};

struct DeleteStmt {
    std::string table;
    std::optional<std::string> schema;
    std::string alias;
    std::vector<TableRefPtr> using_tables;
    ExprPtr where_clause;
    std::vector<ExprPtr> returning;            // RETURNING expressions
};

struct MergeStmt {
    std::string target_table;
    std::string target_alias;
    TableRefPtr source;
    ExprPtr on_condition;
    std::vector<MergeWhenClause> when_clauses;
};

// ─── Partition Specification ───
enum class PartitionType { RANGE, LIST, HASH };

struct PartitionBound {
    std::vector<ExprPtr> values;    // bound values (for RANGE: upper bound, for LIST: value list)
    bool is_maxvalue = false;       // MAXVALUE sentinel for RANGE
    bool is_minvalue = false;       // MINVALUE sentinel for RANGE (future)
    bool is_default = false;        // DEFAULT partition (catch-all)
    // HASH partitioning
    std::optional<int> modulus;
    std::optional<int> remainder;
};

struct PartitionDef {
    std::string name;
    PartitionBound bound;
};

struct PartitionSpec {
    PartitionType type;
    std::vector<std::string> columns;   // partition key columns
    std::vector<PartitionDef> partitions;
};

struct CreateTableStmt {
    std::string name;
    std::optional<std::string> schema;
    bool if_not_exists = false;
    bool temporary = false;
    bool encrypted = false;               // TDE for this table
    bool columnar = false;                // STORE AS COLUMN
    std::vector<ColumnDef> columns;
    std::vector<TableConstraint> constraints;
    SelectPtr as_select;                  // CREATE TABLE AS SELECT
    std::optional<PartitionSpec> partition;  // PARTITION BY clause
};

struct CreateIndexStmt {
    std::string name;
    std::string table;
    std::optional<std::string> schema;
    bool unique = false;
    bool if_not_exists = false;
    IndexMethod method = IndexMethod::BPTREE;
    std::vector<IndexColumn> columns;
    ExprPtr where_clause;                 // partial index
};

struct CreateViewStmt {
    std::string name;
    std::optional<std::string> schema;
    bool or_replace = false;
    bool if_not_exists = false;
    std::vector<std::string> columns;
    SelectPtr query;
};

struct CreateSequenceStmt {
    std::string name;
    std::optional<std::string> schema;
    int64_t start = 1;
    int64_t increment = 1;
    std::optional<int64_t> min_value;
    std::optional<int64_t> max_value;
    bool cycle = false;
    bool if_not_exists = false;
};

struct DropStmt {
    StmtType target_type;  // DROP_TABLE, DROP_INDEX, DROP_VIEW
    std::string name;
    std::optional<std::string> schema;
    bool if_exists = false;
    bool cascade = false;
};

struct TruncateStmt {
    std::string table;
    std::optional<std::string> schema;
    bool cascade = false;
};

struct AlterTableStmt {
    std::string table;
    std::optional<std::string> schema;
    enum class Action {
        ADD_COLUMN, DROP_COLUMN, ALTER_COLUMN,
        ADD_CONSTRAINT, DROP_CONSTRAINT,
        RENAME_TABLE, RENAME_COLUMN,
        ADD_PARTITION, DROP_PARTITION,
    };
    Action action;
    ColumnDef column;           // for ADD/ALTER COLUMN
    std::string target_name;    // column or constraint being altered
    std::string new_name;       // for RENAME
    TableConstraint constraint; // for ADD CONSTRAINT
    bool cascade = false;
    PartitionDef partition;     // for ADD/DROP PARTITION
};

struct BeginStmt {
    std::string isolation_level;  // e.g. "READ COMMITTED", "SERIALIZABLE"
    bool read_only = false;
};

struct SavepointStmt {
    std::string name;
};

// ─── CREATE TYPE (Batch 5b) ───
struct CreateTypeStmt {
    std::string name;
    std::optional<std::string> schema;
    bool if_not_exists = false;
    // Composite type fields. For ENUM types, `fields` is empty and labels
    // live in `enum_labels`. For DOMAIN types the base type is in
    // `domain_base` and the optional CHECK in `check_expr`.
    struct Field { std::string field_name; DataType type; };
    std::vector<Field> fields;
    // ENUM
    bool is_enum = false;
    std::vector<std::string> enum_labels;
    // DOMAIN
    bool is_domain = false;
    DataType domain_base;
    ExprPtr check_expr;
};

// ─── ALTER TYPE (Batch 5 close-out) ───
// Covers composite ADD/DROP/RENAME ATTRIBUTE, RENAME TO, and enum ADD VALUE
// / RENAME VALUE. The action enum decides which fields are populated.
struct AlterTypeStmt {
    std::string name;
    std::optional<std::string> schema;
    enum class Action {
        ADD_ATTRIBUTE,
        DROP_ATTRIBUTE,
        RENAME_ATTRIBUTE,
        RENAME_TYPE,
        ADD_ENUM_VALUE,
        RENAME_ENUM_VALUE,
    };
    Action action;
    // ADD_ATTRIBUTE
    std::string attr_name;
    DataType attr_type;
    // DROP_ATTRIBUTE / RENAME_ATTRIBUTE / RENAME_ENUM_VALUE source
    std::string target_name;
    // RENAME_* destinations
    std::string new_name;
    // ADD_ENUM_VALUE / RENAME_ENUM_VALUE destination value
    std::string enum_value;
};

struct ExplainStmt {
    bool analyze = false;
    StmtPtr statement;
};

// ─── Materialized View ───
struct CreateMaterializedViewStmt {
    std::string name;
    std::optional<std::string> schema;
    bool if_not_exists = false;
    bool writable = false;       // writable materialized view
    bool or_replace = false;
    std::vector<std::string> columns;
    SelectPtr query;
    std::optional<std::string> tablespace;
    bool with_data = true;       // WITH DATA / WITH NO DATA
};

struct RefreshMaterializedViewStmt {
    std::string name;
    std::optional<std::string> schema;
    bool concurrently = false;
    bool with_data = true;
};

struct AlterMaterializedViewStmt {
    std::string name;
    std::optional<std::string> schema;
    enum class Action { SET_TABLESPACE, SET_WRITABLE, RENAME, SET_SCHEMA };
    Action action;
    std::string new_value;  // new tablespace/name/schema
    bool writable = false;
};

// ─── Saved Query ───
struct CreateSavedQueryStmt {
    std::string name;
    std::optional<std::string> schema;
    bool or_replace = false;
    std::string description;
    std::vector<std::pair<std::string, DataType>> parameters;  // named params
    SelectPtr query;
};

// ─── Tablespace ───
struct CreateTablespaceStmt {
    std::string name;
    std::string location;
    std::optional<std::string> owner;
    std::unordered_map<std::string, std::string> options;
};

struct DropTablespaceStmt {
    std::string name;
    bool if_exists = false;
};

// ─── Lock / Unlock ───
struct LockStmt {
    std::string table;
    std::optional<std::string> schema;
    LockMode mode = LockMode::ACCESS_EXCLUSIVE;
    LockGranularity granularity = LockGranularity::TABLE;
    bool nowait = false;
    int timeout_ms = -1;      // -1 = wait forever
    ExprPtr row_condition;     // for row-level locks: WHERE clause
    std::string object_name;   // for object-level: specific object identifier
};

struct UnlockStmt {
    std::string table;
    std::optional<std::string> schema;
    LockGranularity granularity = LockGranularity::TABLE;
    std::string object_name;
};

// ─── Logging Configuration ───
struct LoggingStmt {
    enum class Action { ENABLE, DISABLE, SET_LEVEL, SET_DESTINATION, AUDIT_ENABLE, AUDIT_DISABLE };
    Action action;
    std::string target;     // table name, or "*" for all
    std::string level;      // "DEBUG", "INFO", "WARN", "ERROR"
    std::string destination; // file path, "STDOUT", "SYSLOG"
    std::vector<std::string> audit_events; // "SELECT", "INSERT", "UPDATE", "DELETE", "DDL"
};

// ─── Document Query: XPath ───
struct XPathQueryStmt {
    std::string xpath_expr;         // the XPath expression string
    std::string source_table;       // table containing XML data
    std::string source_column;      // column containing XML
    std::optional<std::string> alias;
    ExprPtr where_clause;           // optional SQL WHERE filter
    std::vector<std::pair<std::string, std::string>> namespaces;  // prefix -> URI
};

// ─── Document Query: XQuery ───
struct XQueryStmt {
    std::string xquery_expr;        // the XQuery expression string
    std::string source_table;
    std::string source_column;
    std::optional<std::string> alias;
    ExprPtr where_clause;
    std::vector<std::pair<std::string, std::string>> namespaces;
    std::vector<std::pair<std::string, ExprPtr>> passing_params;  // PASSING clause
};

// ─── Document Query: GraphQL ───
struct GraphQLQueryStmt {
    std::string graphql_query;      // the GraphQL query string
    std::string source_table;       // table containing JSON/XML document data
    std::string source_column;
    std::optional<std::string> alias;
    ExprPtr where_clause;
    std::vector<std::pair<std::string, ExprPtr>> variables;  // GraphQL variables
    // Join support: join document results with columnar tables
    std::vector<std::pair<std::string, std::string>> joins;  // (target_table, join_condition_str)
};

// ─── v1.2.0: CREATE TRIGGER ───
struct CreateTriggerStmt {
    std::string name;
    std::string table;
    std::optional<std::string> schema;
    bool if_not_exists = false;
    bool or_replace = false;
    std::string timing;     // "BEFORE", "AFTER", "INSTEAD OF"
    std::string event;      // "INSERT", "UPDATE", "DELETE"
    bool for_each_row = true;
    std::string body;       // PL/SQL body or Lua source
    std::string language;   // "plsql" (default), "lua", "javascript"
};

// ─── v1.2.0: CREATE PROCEDURE / FUNCTION (PL/SQL) ───
struct PLParam {
    std::string name;
    DataType type;
    std::string mode = "IN";  // IN, OUT, INOUT
};

struct CreateProcedureStmt {
    std::string name;
    std::optional<std::string> schema;
    bool if_not_exists = false;
    bool or_replace = false;
    bool is_function = false;   // true for CREATE FUNCTION
    std::vector<PLParam> params;
    DataType return_type;       // for functions
    std::string language;       // "plsql" (default), "lua", "javascript"
    std::string body;           // the PL/SQL body between BEGIN...END or $$ ... $$
    bool deterministic = false;
};

// ─── v1.2.0: CALL procedure ───
struct CallStmt {
    std::string name;
    std::optional<std::string> schema;
    std::vector<ExprPtr> args;
};

// ─── v1.2.0: GRANT / REVOKE ───
struct GrantStmt {
    bool is_revoke = false;
    std::vector<std::string> privileges;  // "SELECT", "INSERT", "UPDATE", "DELETE", "ALL"
    std::string object_type;              // "TABLE", "VIEW", "SEQUENCE", "PROCEDURE", "SCHEMA"
    std::string object_name;
    std::string grantee;                  // role or user name
    bool with_grant_option = false;
};

// ─── v1.2.0: PREPARE / EXECUTE / DEALLOCATE ───
struct PrepareStmt {
    std::string name;
    std::string sql_text;       // the SQL template with $1, $2 placeholders
};

struct ExecuteStmt {
    std::string name;
    std::vector<ExprPtr> params;
};

struct DeallocateStmt {
    std::string name;
    bool all = false;           // DEALLOCATE ALL
};

// ─── v1.2.0: Cursors ───
struct DeclareCursorStmt {
    std::string name;
    bool scroll = false;
    bool hold = false;
    bool insensitive = false;
    SelectPtr query;
};

struct OpenCursorStmt { std::string name; };
struct CloseCursorStmt { std::string name; };

struct FetchCursorStmt {
    std::string name;
    enum class Direction { NEXT, PRIOR, FIRST, LAST, ABSOLUTE, RELATIVE, FORWARD, BACKWARD };
    Direction direction = Direction::NEXT;
    int64_t count = 1;
};

// ─── v1.2.0: Graph MATCH query (SQL/PGQ) ───
// Graph pattern node: (alias:table)
struct GraphNode {
    std::string alias;
    std::string label;  // table name
};

// Graph pattern edge: -[alias:fk_table]->
struct GraphEdge {
    std::string alias;
    std::string label;       // edge label (FK table or relationship name)
    bool directed = true;
    int from_node = 0;       // index into nodes
    int to_node = 1;         // index into nodes
};

struct GraphMatchStmt {
    // MATCH (a:Table)-[r:Rel]->(b:Table) WHERE ... RETURN ...
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    ExprPtr where_clause;
    std::vector<ExprPtr> return_list;
    std::vector<std::string> return_aliases;
    std::vector<OrderByItem> order_by;
    ExprPtr limit;
};

struct Statement {
    StmtType type;
    uint32_t line = 0;

    std::variant<
        SelectStmt,
        InsertStmt,
        UpdateStmt,
        DeleteStmt,
        MergeStmt,
        CreateTableStmt,
        CreateTypeStmt,
        AlterTypeStmt,
        CreateIndexStmt,
        CreateViewStmt,
        CreateSequenceStmt,
        AlterTableStmt,
        DropStmt,
        TruncateStmt,
        BeginStmt,
        SavepointStmt,
        ExplainStmt,
        CreateMaterializedViewStmt,
        RefreshMaterializedViewStmt,
        AlterMaterializedViewStmt,
        CreateSavedQueryStmt,
        CreateTablespaceStmt,
        DropTablespaceStmt,
        LockStmt,
        UnlockStmt,
        LoggingStmt,
        XPathQueryStmt,
        XQueryStmt,
        GraphQLQueryStmt,
        // v1.2.0
        CreateTriggerStmt,
        CreateProcedureStmt,
        CallStmt,
        GrantStmt,
        PrepareStmt,
        ExecuteStmt,
        DeallocateStmt,
        DeclareCursorStmt,
        OpenCursorStmt,
        CloseCursorStmt,
        FetchCursorStmt,
        GraphMatchStmt
    > data;
};

// Helper to print AST for debugging
std::string ast_to_string(const Statement &stmt, int indent = 0);

} // namespace tdb::sql::ast

#endif // TDB_SQL_AST_H
