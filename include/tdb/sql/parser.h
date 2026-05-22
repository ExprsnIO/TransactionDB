#ifndef TDB_SQL_PARSER_H
#define TDB_SQL_PARSER_H

#include "tdb/sql/lexer.h"
#include "tdb/sql/ast.h"
#include <string>
#include <vector>
#include <stdexcept>

namespace tdb::sql {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string &msg, uint32_t line, uint32_t col)
        : std::runtime_error(msg), line_(line), col_(col) {}
    uint32_t line() const { return line_; }
    uint32_t col() const { return col_; }
private:
    uint32_t line_, col_;
};

class Parser {
public:
    explicit Parser(const std::string &input);

    ast::StmtPtr parse();
    std::vector<ast::StmtPtr> parse_all();

private:
    Lexer lexer_;
    Token current_;
    Token peek_;

    // Token navigation
    void advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    Token expect(TokenType type, const std::string &context);
    [[noreturn]] void error(const std::string &msg);

    // Top-level dispatch
    ast::StmtPtr parse_statement();

    // DDL
    ast::StmtPtr parse_create();
    ast::StmtPtr parse_create_table();
    ast::StmtPtr parse_create_type();
    ast::StmtPtr parse_create_domain();
    ast::StmtPtr parse_alter();
    ast::StmtPtr parse_alter_type();
    ast::StmtPtr parse_create_index(bool unique);
    ast::StmtPtr parse_create_view(bool or_replace);
    ast::StmtPtr parse_create_sequence();
    ast::StmtPtr parse_alter_table();
    ast::StmtPtr parse_drop();
    ast::StmtPtr parse_truncate();

    // DML
    ast::StmtPtr parse_select_stmt();
    ast::StmtPtr parse_insert();
    ast::StmtPtr parse_update();
    ast::StmtPtr parse_delete();
    ast::StmtPtr parse_merge();

    // Transaction
    ast::StmtPtr parse_begin();
    ast::StmtPtr parse_commit();
    ast::StmtPtr parse_rollback();
    ast::StmtPtr parse_savepoint();
    ast::StmtPtr parse_release_savepoint();

    // Materialized Views
    ast::StmtPtr parse_create_materialized_view(bool or_replace);
    ast::StmtPtr parse_refresh_materialized_view();

    // Saved Queries
    ast::StmtPtr parse_create_saved_query(bool or_replace);

    // Tablespaces
    ast::StmtPtr parse_create_tablespace();

    // Locking
    ast::StmtPtr parse_lock();
    ast::StmtPtr parse_unlock();

    // Logging
    ast::StmtPtr parse_logging();

    // Document queries
    ast::StmtPtr parse_xpath_query();
    ast::StmtPtr parse_xquery();
    ast::StmtPtr parse_graphql_query();

    // Utility
    ast::StmtPtr parse_explain();

    // SELECT internals
    ast::SelectPtr parse_select();
    ast::SelectPtr parse_select_body();
    std::vector<ast::CTE> parse_ctes();
    std::vector<ast::ExprPtr> parse_select_list();
    std::vector<ast::TableRefPtr> parse_from();
    ast::TableRefPtr parse_table_ref();
    ast::TableRefPtr parse_join(ast::TableRefPtr left);
    ast::ExprPtr parse_where();
    std::vector<ast::ExprPtr> parse_group_by();
    ast::ExprPtr parse_having();
    std::vector<ast::OrderByItem> parse_order_by();

    // Expression parser (precedence climbing)
    ast::ExprPtr parse_expr();
    ast::ExprPtr parse_or_expr();
    ast::ExprPtr parse_and_expr();
    ast::ExprPtr parse_not_expr();
    ast::ExprPtr parse_comparison();
    ast::ExprPtr parse_addition();
    ast::ExprPtr parse_multiplication();
    ast::ExprPtr parse_unary();
    ast::ExprPtr parse_postfix(ast::ExprPtr left);
    ast::ExprPtr parse_primary();
    ast::ExprPtr parse_function_call(const std::string &name);
    ast::ExprPtr parse_case();
    ast::ExprPtr parse_cast();
    ast::ExprPtr parse_subquery_expr();
    ast::ExprPtr parse_exists();

    // Window
    ast::WindowSpec parse_window_spec();
    ast::WindowFrame parse_window_frame();

    // Helpers
    bool is_identifier_or_keyword() const;
    Token expect_identifier(const std::string &context);
    ast::DataType parse_data_type();
    ast::ColumnDef parse_column_def();
    ast::TableConstraint parse_table_constraint();
    ast::IndexMethod parse_index_method();
    std::string parse_qualified_name();
    std::vector<std::string> parse_identifier_list();

    // Partitioning
    ast::PartitionSpec parse_partition_spec();
    ast::PartitionDef parse_partition_def(ast::PartitionType type);

    // v1.2.0: New statement parsers
    ast::StmtPtr parse_create_trigger(bool or_replace = false);
    ast::StmtPtr parse_create_procedure(bool or_replace, bool is_function);
    ast::StmtPtr parse_call();
    ast::StmtPtr parse_grant(bool is_revoke);
    ast::StmtPtr parse_prepare();
    ast::StmtPtr parse_execute();
    ast::StmtPtr parse_deallocate();
    ast::StmtPtr parse_declare_cursor();
    ast::StmtPtr parse_open_cursor();
    ast::StmtPtr parse_close_cursor();
    ast::StmtPtr parse_fetch_cursor();
    ast::StmtPtr parse_graph_match();
};

} // namespace tdb::sql

#endif // TDB_SQL_PARSER_H
