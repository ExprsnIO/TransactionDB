#include "tdb/sql/parser.h"
#include <sstream>
#include <algorithm>

namespace tdb::sql {

// ─── Constructor ───
Parser::Parser(const std::string &input)
    : lexer_(input)
{
    current_ = lexer_.next_token();
    peek_ = lexer_.next_token();
}

// ─── Token Navigation ───
void Parser::advance() {
    current_ = peek_;
    peek_ = lexer_.next_token();
}

bool Parser::check(TokenType type) const {
    return current_.type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

Token Parser::expect(TokenType type, const std::string &context) {
    if (check(type)) {
        Token t = current_;
        advance();
        return t;
    }
    std::ostringstream oss;
    oss << "Expected " << token_type_name(type) << " in " << context
        << ", got " << token_type_name(current_.type)
        << " ('" << current_.value << "')";
    error(oss.str());
}

[[noreturn]] void Parser::error(const std::string &msg) {
    throw ParseError(msg, current_.line, current_.col);
}

bool Parser::is_identifier_or_keyword() const {
    if (check(TokenType::IDENTIFIER) || check(TokenType::QUOTED_IDENTIFIER))
        return true;
    // Non-reserved keywords can be used as identifiers.
    // Only truly reserved words (SELECT, FROM, WHERE, etc.) are excluded.
    switch (current_.type) {
        // Reserved: SELECT, INSERT, UPDATE, DELETE, FROM, WHERE, GROUP, HAVING,
        //           ORDER, BY, AND, OR, NOT, NULL, CREATE, ALTER, DROP, JOIN,
        //           INNER, LEFT, RIGHT, FULL, OUTER, CROSS, NATURAL, ON,
        //           AS, CASE, WHEN, THEN, ELSE, END, UNION, INTERSECT, EXCEPT,
        //           WITH, INTO, SET, PRIMARY, FOREIGN, IF, BEGIN, COMMIT, ROLLBACK,
        //           GRANT, REVOKE, EXISTS, IN, BETWEEN, LIKE, IS, CAST, FOR
        // Everything else is non-reserved:
        case TokenType::KW_TABLE: case TokenType::KW_INDEX: case TokenType::KW_VIEW:
        case TokenType::KW_SEQUENCE: case TokenType::KW_SCHEMA: case TokenType::KW_DATABASE:
        case TokenType::KW_TRIGGER: case TokenType::KW_COLUMN: case TokenType::KW_CONSTRAINT:
        case TokenType::KW_KEY: case TokenType::KW_CASCADE: case TokenType::KW_RESTRICT:
        case TokenType::KW_ADD: case TokenType::KW_RENAME: case TokenType::KW_TO:
        case TokenType::KW_TEMPORARY: case TokenType::KW_TEMP:
        case TokenType::KW_USING: case TokenType::KW_REPLACE:
        case TokenType::KW_FIRST: case TokenType::KW_LAST:
        case TokenType::KW_ROWS: case TokenType::KW_ONLY:
        case TokenType::KW_VALUES: case TokenType::KW_ALL: case TokenType::KW_DISTINCT:
        case TokenType::KW_MATCHED: case TokenType::KW_TARGET: case TokenType::KW_SOURCE:
        case TokenType::KW_COUNT: case TokenType::KW_SUM: case TokenType::KW_AVG:
        case TokenType::KW_MIN: case TokenType::KW_MAX:
        case TokenType::KW_PARTITION: case TokenType::KW_WINDOW:
        case TokenType::KW_RANGE: case TokenType::KW_ROW:
        case TokenType::KW_UNBOUNDED: case TokenType::KW_PRECEDING: case TokenType::KW_FOLLOWING:
        case TokenType::KW_CURRENT:
        // Types as identifiers
        case TokenType::KW_INT: case TokenType::KW_INTEGER: case TokenType::KW_SMALLINT:
        case TokenType::KW_BIGINT: case TokenType::KW_TINYINT:
        case TokenType::KW_FLOAT: case TokenType::KW_DOUBLE: case TokenType::KW_REAL:
        case TokenType::KW_DECIMAL: case TokenType::KW_NUMERIC: case TokenType::KW_PRECISION:
        case TokenType::KW_CHAR: case TokenType::KW_VARCHAR: case TokenType::KW_TEXT:
        case TokenType::KW_CHARACTER: case TokenType::KW_VARYING:
        case TokenType::KW_NATIONAL: case TokenType::KW_NCHAR: case TokenType::KW_NVARCHAR:
        case TokenType::KW_NCLOB:
        case TokenType::KW_BOOLEAN: case TokenType::KW_BOOL:
        case TokenType::KW_DATE: case TokenType::KW_TIME: case TokenType::KW_TIMESTAMP:
        case TokenType::KW_INTERVAL:
        case TokenType::KW_BLOB: case TokenType::KW_CLOB:
        case TokenType::KW_BINARY: case TokenType::KW_VARBINARY: case TokenType::KW_BIT:
        case TokenType::KW_JSON: case TokenType::KW_JSONB: case TokenType::KW_XML:
        case TokenType::KW_ARRAY: case TokenType::KW_MULTISET:
        case TokenType::KW_GEOMETRY: case TokenType::KW_GEOGRAPHY: case TokenType::KW_RASTER:
        case TokenType::KW_SERIAL: case TokenType::KW_BIGSERIAL:
        case TokenType::KW_UUID: case TokenType::KW_ENUM: case TokenType::KW_MONEY:
        // Transaction-related
        case TokenType::KW_READ: case TokenType::KW_WRITE:
        case TokenType::KW_COMMITTED: case TokenType::KW_UNCOMMITTED:
        case TokenType::KW_REPEATABLE: case TokenType::KW_SERIALIZABLE: case TokenType::KW_SNAPSHOT:
        case TokenType::KW_TRANSACTION: case TokenType::KW_ISOLATION: case TokenType::KW_LEVEL:
        case TokenType::KW_START: case TokenType::KW_WORK: case TokenType::KW_CONSTRAINTS:
        // Encryption
        case TokenType::KW_ENCRYPT: case TokenType::KW_ENCRYPTED: case TokenType::KW_DECRYPT:
        // Misc non-reserved
        case TokenType::KW_LATERAL: case TokenType::KW_ANALYZE: case TokenType::KW_VACUUM:
        case TokenType::KW_PRIVILEGES: case TokenType::KW_COLLATE: case TokenType::KW_ESCAPE:
        case TokenType::KW_NEXT: case TokenType::KW_NULLS:
        case TokenType::KW_AUTO_INCREMENT:
        case TokenType::KW_RETURNING: case TokenType::KW_CONFLICT: case TokenType::KW_NOTHING:
        case TokenType::KW_DO: case TokenType::KW_TIES:
        case TokenType::KW_TABLESAMPLE: case TokenType::KW_BERNOULLI:
        case TokenType::KW_SHARE: case TokenType::KW_NOWAIT: case TokenType::KW_LOCKED:
        case TokenType::KW_MATERIALIZED: case TokenType::KW_CALL:
        case TokenType::KW_SIMILAR: case TokenType::KW_OVERLAPS: case TokenType::KW_UNKNOWN:
        case TokenType::KW_SYMMETRIC: case TokenType::KW_ASYMMETRIC:
        case TokenType::KW_GROUPS: case TokenType::KW_EXCLUDE: case TokenType::KW_OTHERS:
        case TokenType::KW_FILTER: case TokenType::KW_WITHIN:
        case TokenType::KW_GROUPING: case TokenType::KW_SETS:
        case TokenType::KW_CUBE: case TokenType::KW_ROLLUP:
        case TokenType::KW_ROW_NUMBER: case TokenType::KW_RANK: case TokenType::KW_DENSE_RANK:
        case TokenType::KW_NTILE: case TokenType::KW_LAG: case TokenType::KW_LEAD:
        case TokenType::KW_FIRST_VALUE: case TokenType::KW_LAST_VALUE: case TokenType::KW_NTH_VALUE:
        case TokenType::KW_PERCENT_RANK: case TokenType::KW_CUME_DIST:
        case TokenType::KW_LISTAGG: case TokenType::KW_PERCENTILE_CONT:
        case TokenType::KW_PERCENTILE_DISC: case TokenType::KW_MODE:
        // Date/time
        case TokenType::KW_ZONE: case TokenType::KW_WITHOUT:
        case TokenType::KW_EXTRACT:
        case TokenType::KW_YEAR: case TokenType::KW_MONTH: case TokenType::KW_DAY:
        case TokenType::KW_HOUR: case TokenType::KW_MINUTE: case TokenType::KW_SECOND:
        case TokenType::KW_EPOCH: case TokenType::KW_WEEK: case TokenType::KW_QUARTER:
        case TokenType::KW_DOW: case TokenType::KW_DOY:
        case TokenType::KW_CURRENT_DATE: case TokenType::KW_CURRENT_TIME:
        case TokenType::KW_CURRENT_TIMESTAMP:
        case TokenType::KW_LOCALTIME: case TokenType::KW_LOCALTIMESTAMP:
        case TokenType::KW_AT_KW:
        // String function keywords
        case TokenType::KW_SUBSTRING: case TokenType::KW_TRIM:
        case TokenType::KW_LEADING: case TokenType::KW_TRAILING: case TokenType::KW_BOTH:
        case TokenType::KW_POSITION: case TokenType::KW_OVERLAY: case TokenType::KW_PLACING:
        case TokenType::KW_UPPER: case TokenType::KW_LOWER:
        case TokenType::KW_NORMALIZE: case TokenType::KW_TRANSLATE: case TokenType::KW_CONVERT:
        case TokenType::KW_COALESCE: case TokenType::KW_NULLIF:
        case TokenType::KW_GREATEST: case TokenType::KW_LEAST: case TokenType::KW_CONCAT:
        // DDL non-reserved
        case TokenType::KW_FUNCTION: case TokenType::KW_PROCEDURE:
        case TokenType::KW_RETURNS: case TokenType::KW_RETURN:
        case TokenType::KW_LANGUAGE: case TokenType::KW_CALLED: case TokenType::KW_DETERMINISTIC:
        case TokenType::KW_DOMAIN: case TokenType::KW_TYPE:
        case TokenType::KW_ROLE: case TokenType::KW_USER:
        case TokenType::KW_AUTHORIZATION:
        case TokenType::KW_GENERATED: case TokenType::KW_ALWAYS: case TokenType::KW_IDENTITY:
        case TokenType::KW_DEFERRED: case TokenType::KW_IMMEDIATE: case TokenType::KW_INITIALLY:
        case TokenType::KW_DEFERRABLE:
        case TokenType::KW_NO: case TokenType::KW_ACTION:
        case TokenType::KW_PRESERVE: case TokenType::KW_GLOBAL: case TokenType::KW_LOCAL:
        case TokenType::KW_COMMENT:
        // Misc
        case TokenType::KW_SHOW: case TokenType::KW_DESCRIBE:
        case TokenType::KW_PREPARE: case TokenType::KW_DEALLOCATE:
        case TokenType::KW_DECLARE: case TokenType::KW_CURSOR:
        case TokenType::KW_OPEN: case TokenType::KW_CLOSE:
        case TokenType::KW_SCROLL: case TokenType::KW_INSENSITIVE: case TokenType::KW_SENSITIVE:
        case TokenType::KW_HOLD:
        case TokenType::KW_COPY: case TokenType::KW_VERBOSE: case TokenType::KW_FORMAT:
        case TokenType::KW_UNNEST: case TokenType::KW_CORRESPONDING:
        case TokenType::KW_USAGE: case TokenType::KW_OPTION: case TokenType::KW_PUBLIC:
        case TokenType::KW_EXECUTE:
        // Trigger
        case TokenType::KW_BEFORE: case TokenType::KW_AFTER: case TokenType::KW_INSTEAD:
        case TokenType::KW_EACH: case TokenType::KW_NEW: case TokenType::KW_OLD:
        case TokenType::KW_REFERENCING: case TokenType::KW_STATEMENT:
        // PSM
        case TokenType::KW_ATOMIC: case TokenType::KW_ELSEIF:
        case TokenType::KW_WHILE: case TokenType::KW_LOOP: case TokenType::KW_REPEAT:
        case TokenType::KW_UNTIL: case TokenType::KW_LEAVE: case TokenType::KW_ITERATE:
        case TokenType::KW_SIGNAL: case TokenType::KW_RESIGNAL:
        case TokenType::KW_HANDLER: case TokenType::KW_CONDITION:
        case TokenType::KW_SQLSTATE: case TokenType::KW_SQLEXCEPTION: case TokenType::KW_SQLWARNING:
        case TokenType::KW_INOUT: case TokenType::KW_OUT:
        // JSON / XML
        case TokenType::KW_JSON_OBJECT: case TokenType::KW_JSON_ARRAY:
        case TokenType::KW_JSON_VALUE: case TokenType::KW_JSON_QUERY:
        case TokenType::KW_JSON_TABLE: case TokenType::KW_JSON_EXISTS:
        case TokenType::KW_XMLELEMENT: case TokenType::KW_XMLFOREST:
        case TokenType::KW_XMLAGG: case TokenType::KW_XMLPARSE:
        case TokenType::KW_XMLSERIALIZE: case TokenType::KW_XMLNAMESPACES:
        // New features
        case TokenType::KW_REFRESH: case TokenType::KW_CONCURRENTLY: case TokenType::KW_WRITABLE:
        case TokenType::KW_SAVE: case TokenType::KW_SAVED: case TokenType::KW_QUERY:
        case TokenType::KW_TABLESPACE: case TokenType::KW_LOCATION: case TokenType::KW_OWNER:
        case TokenType::KW_MOVE:
        case TokenType::KW_LOG: case TokenType::KW_LOGGING: case TokenType::KW_NOLOGGING:
        case TokenType::KW_AUDIT: case TokenType::KW_NOAUDIT:
        case TokenType::KW_XPATH: case TokenType::KW_XQUERY: case TokenType::KW_GRAPHQL:
        case TokenType::KW_DOCUMENT: case TokenType::KW_CONTENT: case TokenType::KW_PATH:
        // Allow LEFT/RIGHT/IF/MATCH as identifiers when not in JOIN/conditional/graph context
        case TokenType::KW_LEFT: case TokenType::KW_RIGHT: case TokenType::KW_IF:
        case TokenType::KW_MATCH: case TokenType::KW_GRAPH:
        case TokenType::KW_VERTEX: case TokenType::KW_EDGE:
        case TokenType::KW_PASSING:
        case TokenType::KW_OBJECT: case TokenType::KW_ADVISORY:
        case TokenType::KW_INTENT: case TokenType::KW_ACCESS:
        case TokenType::KW_WAIT:
        case TokenType::KW_COLUMNAR: case TokenType::KW_EXTERNAL_KW:
        case TokenType::KW_SERVER: case TokenType::KW_WRAPPER: case TokenType::KW_OPTIONS:
        case TokenType::KW_MAPPING:
        // Partitioning
        case TokenType::KW_HASH: case TokenType::KW_LIST:
        case TokenType::KW_LESS: case TokenType::KW_THAN:
        case TokenType::KW_MODULUS: case TokenType::KW_REMAINDER:
        case TokenType::KW_MAXVALUE: case TokenType::KW_MINVALUE:
            return true;
        default:
            return false;
    }
}

Token Parser::expect_identifier(const std::string &context) {
    if (is_identifier_or_keyword()) {
        Token t = current_;
        t.type = TokenType::IDENTIFIER; // normalize
        advance();
        return t;
    }
    std::ostringstream oss;
    oss << "Expected identifier in " << context
        << ", got " << token_type_name(current_.type)
        << " ('" << current_.value << "')";
    error(oss.str());
}

// ─── Public Interface ───
ast::StmtPtr Parser::parse() {
    auto stmt = parse_statement();
    match(TokenType::SEMICOLON);
    return stmt;
}

std::vector<ast::StmtPtr> Parser::parse_all() {
    std::vector<ast::StmtPtr> stmts;
    while (!check(TokenType::END_OF_INPUT)) {
        if (match(TokenType::SEMICOLON)) continue;
        stmts.push_back(parse_statement());
        match(TokenType::SEMICOLON);
    }
    return stmts;
}

// ─── Statement Dispatch ───
ast::StmtPtr Parser::parse_statement() {
    switch (current_.type) {
        case TokenType::KW_SELECT:
        case TokenType::KW_WITH:
            return parse_select_stmt();
        case TokenType::KW_INSERT:
            return parse_insert();
        case TokenType::KW_UPDATE:
            return parse_update();
        case TokenType::KW_DELETE:
            return parse_delete();
        case TokenType::KW_MERGE:
            return parse_merge();
        case TokenType::KW_CREATE:
            return parse_create();
        case TokenType::KW_ALTER:
            return parse_alter();
        case TokenType::KW_DROP:
            return parse_drop();
        case TokenType::KW_TRUNCATE:
            return parse_truncate();
        case TokenType::KW_BEGIN:
            return parse_begin();
        case TokenType::KW_COMMIT:
            return parse_commit();
        case TokenType::KW_ROLLBACK:
            return parse_rollback();
        case TokenType::KW_SAVEPOINT:
            return parse_savepoint();
        case TokenType::KW_RELEASE:
            return parse_release_savepoint();
        case TokenType::KW_EXPLAIN:
            return parse_explain();
        case TokenType::KW_REFRESH:
            return parse_refresh_materialized_view();
        case TokenType::KW_LOCK_KW:
            return parse_lock();
        case TokenType::KW_LOGGING:
        case TokenType::KW_NOLOGGING:
        case TokenType::KW_AUDIT:
        case TokenType::KW_NOAUDIT:
            return parse_logging();
        case TokenType::KW_XPATH:
            return parse_xpath_query();
        case TokenType::KW_XQUERY:
            return parse_xquery();
        case TokenType::KW_GRAPHQL:
            return parse_graphql_query();
        case TokenType::KW_START:
            return parse_begin();  // START TRANSACTION
        case TokenType::KW_CALL:
            return parse_call();
        case TokenType::KW_GRANT:
            return parse_grant(false);
        case TokenType::KW_REVOKE:
            return parse_grant(true);
        case TokenType::KW_PREPARE:
            return parse_prepare();
        case TokenType::KW_EXECUTE:
            return parse_execute();
        case TokenType::KW_DEALLOCATE:
            return parse_deallocate();
        case TokenType::KW_DECLARE:
            return parse_declare_cursor();
        case TokenType::KW_OPEN:
            return parse_open_cursor();
        case TokenType::KW_CLOSE:
            return parse_close_cursor();
        case TokenType::KW_FETCH:
            return parse_fetch_cursor();
        case TokenType::KW_MATCH:
            return parse_graph_match();
        default:
            error("Unexpected token: " + current_.value);
    }
}

// ─── CREATE ───
ast::StmtPtr Parser::parse_create() {
    advance(); // CREATE

    bool or_replace = false;
    if (check(TokenType::KW_OR)) {
        advance();
        expect(TokenType::KW_REPLACE, "CREATE OR REPLACE");
        or_replace = true;
    }

    bool temporary = false;
    if (match(TokenType::KW_TEMPORARY) || match(TokenType::KW_TEMP)) {
        temporary = true;
    }

    bool unique = false;
    if (match(TokenType::KW_UNIQUE)) {
        unique = true;
    }

    if (check(TokenType::KW_TABLE)) {
        auto stmt = parse_create_table();
        auto &ct = std::get<ast::CreateTableStmt>(stmt->data);
        ct.temporary = temporary;
        return stmt;
    }
    if (check(TokenType::KW_INDEX)) {
        return parse_create_index(unique);
    }
    if (check(TokenType::KW_VIEW)) {
        return parse_create_view(or_replace);
    }
    if (check(TokenType::KW_SEQUENCE)) {
        return parse_create_sequence();
    }
    if (check(TokenType::KW_MATERIALIZED)) {
        return parse_create_materialized_view(or_replace);
    }
    if (check(TokenType::KW_TABLESPACE)) {
        return parse_create_tablespace();
    }
    if (check(TokenType::KW_SAVED)) {
        return parse_create_saved_query(or_replace);
    }
    if (check(TokenType::KW_TYPE)) {
        return parse_create_type();
    }
    if (check(TokenType::KW_DOMAIN)) {
        return parse_create_domain();
    }
    if (check(TokenType::KW_TRIGGER)) {
        return parse_create_trigger(or_replace);
    }
    if (check(TokenType::KW_PROCEDURE)) {
        return parse_create_procedure(or_replace, false);
    }
    if (check(TokenType::KW_FUNCTION)) {
        return parse_create_procedure(or_replace, true);
    }

    error("Expected TABLE, TYPE, DOMAIN, INDEX, VIEW, SEQUENCE, TRIGGER, PROCEDURE, FUNCTION, or TABLESPACE after CREATE");
}

// ─── CREATE TYPE name AS ( field1 type1, ... ) (Batch 5b) ───
// Fields may reference registered composite types or any built-in type.
// Nesting just works because parse_data_type accepts arbitrary typenames.
ast::StmtPtr Parser::parse_create_type() {
    advance(); // TYPE
    ast::CreateTypeStmt ct;
    ct.if_not_exists = false;
    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        ct.if_not_exists = true;
    }
    std::string nm = expect_identifier("type name").value;
    if (match(TokenType::DOT)) {
        ct.schema = nm;
        ct.name = expect_identifier("type name").value;
    } else {
        ct.name = nm;
    }
    expect(TokenType::KW_AS, "AS");

    // ENUM form: CREATE TYPE name AS ENUM ('a', 'b', 'c')
    if (check(TokenType::KW_ENUM)) {
        advance();
        ct.is_enum = true;
        expect(TokenType::LPAREN, "(");
        while (!check(TokenType::RPAREN)) {
            if (!check(TokenType::STRING_LITERAL))
                error("Expected string literal in ENUM body");
            ct.enum_labels.push_back(current_.value);
            advance();
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, ")");
        auto stmt = std::make_unique<ast::Statement>();
        stmt->type = ast::StmtType::CREATE_TYPE;
        stmt->data = std::move(ct);
        return stmt;
    }

    // Composite form: CREATE TYPE name AS ( field type, ... )
    expect(TokenType::LPAREN, "(");
    while (!check(TokenType::RPAREN)) {
        ast::CreateTypeStmt::Field f;
        f.field_name = expect_identifier("field name").value;
        f.type = parse_data_type();
        ct.fields.push_back(std::move(f));
        if (!match(TokenType::COMMA)) break;
    }
    expect(TokenType::RPAREN, ")");

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_TYPE;
    stmt->data = std::move(ct);
    return stmt;
}

// ─── CREATE DOMAIN name AS base_type [CHECK (expr)] (Batch 5 close-out) ─
ast::StmtPtr Parser::parse_create_domain() {
    advance(); // DOMAIN
    ast::CreateTypeStmt ct; // reused as the domain carrier
    ct.is_domain = true;
    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        ct.if_not_exists = true;
    }
    std::string nm = expect_identifier("domain name").value;
    if (match(TokenType::DOT)) { ct.schema = nm; ct.name = expect_identifier("domain name").value; }
    else                       { ct.name = nm; }
    expect(TokenType::KW_AS, "AS");
    ct.domain_base = parse_data_type();
    if (check(TokenType::KW_CHECK)) {
        advance();
        expect(TokenType::LPAREN, "(");
        ct.check_expr = parse_expr();
        expect(TokenType::RPAREN, ")");
    }
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_DOMAIN;
    stmt->data = std::move(ct);
    return stmt;
}

// ─── ALTER TYPE name {ADD|DROP|RENAME} ... (Batch 5 close-out) ─────────
// ATTRIBUTE and VALUE are not lexer keywords; we recognize them
// case-insensitively as bare identifiers via match_kw_ident.
ast::StmtPtr Parser::parse_alter_type() {
    advance(); // TYPE
    ast::AlterTypeStmt at;
    std::string nm = expect_identifier("type name").value;
    if (match(TokenType::DOT)) { at.schema = nm; at.name = expect_identifier("type name").value; }
    else                       { at.name = nm; }

    auto match_kw_ident = [&](const char *kw) -> bool {
        if (current_.type != TokenType::IDENTIFIER && current_.type != TokenType::QUOTED_IDENTIFIER) return false;
        std::string s = current_.value;
        for (auto &c : s) c = (char)std::toupper((unsigned char)c);
        if (s != kw) return false;
        advance();
        return true;
    };
    auto check_kw_ident = [&](const char *kw) -> bool {
        if (current_.type != TokenType::IDENTIFIER && current_.type != TokenType::QUOTED_IDENTIFIER) return false;
        std::string s = current_.value;
        for (auto &c : s) c = (char)std::toupper((unsigned char)c);
        return s == kw;
    };

    if (check(TokenType::KW_ADD)) {
        advance();
        if (check_kw_ident("VALUE") || check(TokenType::KW_VALUES)) {
            (void)match_kw_ident("VALUE");
            (void)match(TokenType::KW_VALUES);
            if (!check(TokenType::STRING_LITERAL))
                error("Expected string literal after ADD VALUE");
            at.action = ast::AlterTypeStmt::Action::ADD_ENUM_VALUE;
            at.enum_value = current_.value;
            advance();
        } else {
            (void)match_kw_ident("ATTRIBUTE");
            at.action = ast::AlterTypeStmt::Action::ADD_ATTRIBUTE;
            at.attr_name = expect_identifier("attribute name").value;
            at.attr_type = parse_data_type();
        }
    } else if (check(TokenType::KW_DROP)) {
        advance();
        (void)match_kw_ident("ATTRIBUTE");
        at.action = ast::AlterTypeStmt::Action::DROP_ATTRIBUTE;
        at.target_name = expect_identifier("attribute name").value;
    } else if (check(TokenType::KW_RENAME)) {
        advance();
        if (check(TokenType::KW_TO)) {
            advance();
            at.action = ast::AlterTypeStmt::Action::RENAME_TYPE;
            at.new_name = expect_identifier("new type name").value;
        } else if (check_kw_ident("ATTRIBUTE")) {
            match_kw_ident("ATTRIBUTE");
            at.action = ast::AlterTypeStmt::Action::RENAME_ATTRIBUTE;
            at.target_name = expect_identifier("attribute name").value;
            expect(TokenType::KW_TO, "TO");
            at.new_name = expect_identifier("new attribute name").value;
        } else if (check_kw_ident("VALUE") || check(TokenType::KW_VALUES)) {
            (void)match_kw_ident("VALUE");
            (void)match(TokenType::KW_VALUES);
            if (!check(TokenType::STRING_LITERAL))
                error("Expected string literal after RENAME VALUE");
            at.action = ast::AlterTypeStmt::Action::RENAME_ENUM_VALUE;
            at.target_name = current_.value;
            advance();
            expect(TokenType::KW_TO, "TO");
            if (!check(TokenType::STRING_LITERAL))
                error("Expected string literal as new enum value");
            at.enum_value = current_.value;
            advance();
        } else {
            error("Expected TO, ATTRIBUTE, or VALUE after ALTER TYPE ... RENAME");
        }
    } else {
        error("Expected ADD / DROP / RENAME after ALTER TYPE name");
    }
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::ALTER_TYPE;
    stmt->data = std::move(at);
    return stmt;
}

// ─── CREATE TABLE ───
ast::StmtPtr Parser::parse_create_table() {
    advance(); // TABLE
    uint32_t stmt_line = current_.line;

    ast::CreateTableStmt ct;
    ct.if_not_exists = false;

    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        ct.if_not_exists = true;
    }

    // Table name (possibly schema-qualified)
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        ct.schema = name;
        ct.name = expect_identifier("table name").value;
    } else {
        ct.name = name;
    }

    // ENCRYPTED keyword for TDE
    if (match(TokenType::KW_ENCRYPTED)) {
        ct.encrypted = true;
    }

    // STORE AS COLUMN (columnar storage)
    if (check(TokenType::KW_COLUMNAR)) {
        advance();
        ct.columnar = true;
    }

    // CREATE TABLE ... AS SELECT
    if (check(TokenType::KW_AS)) {
        advance();
        ct.as_select = parse_select();
        auto stmt = std::make_unique<ast::Statement>();
        stmt->type = ast::StmtType::CREATE_TABLE;
        stmt->line = stmt_line;
        stmt->data = std::move(ct);
        return stmt;
    }

    expect(TokenType::LPAREN, "CREATE TABLE column list");

    // Parse columns and table constraints
    while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
        // Table constraint or column?
        if (check(TokenType::KW_PRIMARY) || check(TokenType::KW_FOREIGN) ||
            check(TokenType::KW_UNIQUE) || check(TokenType::KW_CHECK) ||
            check(TokenType::KW_CONSTRAINT)) {
            ct.constraints.push_back(parse_table_constraint());
        } else {
            ct.columns.push_back(parse_column_def());
        }

        if (!match(TokenType::COMMA)) break;
    }

    expect(TokenType::RPAREN, "CREATE TABLE");

    // PARTITION BY {RANGE|LIST|HASH} (col, ...) (partition_defs)
    if (check(TokenType::KW_PARTITION)) {
        advance(); // PARTITION
        expect(TokenType::KW_BY, "PARTITION BY");
        ct.partition = parse_partition_spec();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_TABLE;
    stmt->line = stmt_line;
    stmt->data = std::move(ct);
    return stmt;
}

// ─── Partition Specification ───
// PARTITION BY {RANGE|LIST|HASH} (col, ...) (partition_def, ...)
ast::PartitionSpec Parser::parse_partition_spec() {
    ast::PartitionSpec spec;

    if (check(TokenType::KW_RANGE)) {
        spec.type = ast::PartitionType::RANGE;
        advance();
    } else if (check(TokenType::KW_LIST)) {
        spec.type = ast::PartitionType::LIST;
        advance();
    } else if (check(TokenType::KW_HASH)) {
        spec.type = ast::PartitionType::HASH;
        advance();
    } else {
        error("Expected RANGE, LIST, or HASH after PARTITION BY");
    }

    // Parse partition key columns: (col1, col2, ...)
    expect(TokenType::LPAREN, "partition key columns");
    spec.columns.push_back(expect_identifier("partition column").value);
    while (match(TokenType::COMMA)) {
        spec.columns.push_back(expect_identifier("partition column").value);
    }
    expect(TokenType::RPAREN, "partition key columns");

    // Parse partition definitions: (PARTITION p1 ..., PARTITION p2 ...)
    expect(TokenType::LPAREN, "partition definitions");
    while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
        spec.partitions.push_back(parse_partition_def(spec.type));
        if (!match(TokenType::COMMA)) break;
    }
    expect(TokenType::RPAREN, "partition definitions");

    return spec;
}

// Parse a single partition definition
ast::PartitionDef Parser::parse_partition_def(ast::PartitionType type) {
    ast::PartitionDef def;

    expect(TokenType::KW_PARTITION, "partition definition");
    def.name = expect_identifier("partition name").value;

    if (type == ast::PartitionType::RANGE) {
        // VALUES LESS THAN (expr) | VALUES LESS THAN MAXVALUE
        expect(TokenType::KW_VALUES, "RANGE partition");
        expect(TokenType::KW_LESS, "VALUES LESS THAN");
        expect(TokenType::KW_THAN, "VALUES LESS THAN");

        if (check(TokenType::KW_MAXVALUE)) {
            advance();
            def.bound.is_maxvalue = true;
        } else {
            expect(TokenType::LPAREN, "RANGE bound values");
            def.bound.values.push_back(parse_expr());
            while (match(TokenType::COMMA)) {
                def.bound.values.push_back(parse_expr());
            }
            expect(TokenType::RPAREN, "RANGE bound values");
        }
    } else if (type == ast::PartitionType::LIST) {
        // VALUES IN (expr, expr, ...)
        expect(TokenType::KW_VALUES, "LIST partition");
        expect(TokenType::KW_IN, "VALUES IN");
        expect(TokenType::LPAREN, "LIST values");
        def.bound.values.push_back(parse_expr());
        while (match(TokenType::COMMA)) {
            def.bound.values.push_back(parse_expr());
        }
        expect(TokenType::RPAREN, "LIST values");
    } else if (type == ast::PartitionType::HASH) {
        // VALUES WITH (MODULUS n, REMAINDER r)
        expect(TokenType::KW_VALUES, "HASH partition");
        expect(TokenType::KW_WITH, "VALUES WITH");
        expect(TokenType::LPAREN, "HASH modulus/remainder");
        expect(TokenType::KW_MODULUS, "MODULUS");
        auto mod_tok = expect(TokenType::INTEGER_LITERAL, "modulus value");
        def.bound.modulus = std::stoi(mod_tok.value);
        expect(TokenType::COMMA, "HASH partition");
        expect(TokenType::KW_REMAINDER, "REMAINDER");
        auto rem_tok = expect(TokenType::INTEGER_LITERAL, "remainder value");
        def.bound.remainder = std::stoi(rem_tok.value);
        expect(TokenType::RPAREN, "HASH modulus/remainder");
    }

    return def;
}

// ─── Column Definition ───
ast::ColumnDef Parser::parse_column_def() {
    ast::ColumnDef col;
    col.name = expect_identifier("column name").value;
    col.type = parse_data_type();

    // Column constraints
    while (true) {
        if (check(TokenType::KW_NOT)) {
            advance();
            expect(TokenType::KW_NULL_KW, "NOT NULL");
            col.nullable = false;
        } else if (match(TokenType::KW_NULL_KW)) {
            col.nullable = true;
        } else if (check(TokenType::KW_PRIMARY)) {
            advance();
            expect(TokenType::KW_KEY, "PRIMARY KEY");
            col.primary_key = true;
        } else if (match(TokenType::KW_UNIQUE)) {
            col.unique = true;
        } else if (match(TokenType::KW_DEFAULT)) {
            col.default_value = parse_expr();
        } else if (match(TokenType::KW_AUTO_INCREMENT) || match(TokenType::KW_SERIAL) || match(TokenType::KW_BIGSERIAL)) {
            col.auto_increment = true;
        } else if (match(TokenType::KW_ENCRYPTED)) {
            col.encrypted = true;
        } else if (match(TokenType::KW_CHECK)) {
            expect(TokenType::LPAREN, "CHECK constraint");
            col.check_expr = parse_expr();
            expect(TokenType::RPAREN, "CHECK constraint");
        } else if (match(TokenType::KW_COLLATE)) {
            col.collation = expect_identifier("COLLATE").value;
        } else if (match(TokenType::KW_GENERATED)) {
            // GENERATED ALWAYS AS (expr) [STORED | VIRTUAL]
            col.generated = true;
            expect(TokenType::KW_ALWAYS, "GENERATED ALWAYS");
            expect(TokenType::KW_AS, "GENERATED ALWAYS AS");
            expect(TokenType::LPAREN, "GENERATED ALWAYS AS (");
            col.generated_expr = parse_expr();
            expect(TokenType::RPAREN, "GENERATED ALWAYS AS (expr)");
            // Optional STORED/VIRTUAL keyword
            if (is_identifier_or_keyword() && current_.value == "STORED") {
                col.generated_mode = ast::ColumnDef::GeneratedMode::STORED;
                advance();
            } else if (is_identifier_or_keyword() && current_.value == "VIRTUAL") {
                col.generated_mode = ast::ColumnDef::GeneratedMode::VIRTUAL;
                advance();
            }
        } else if (check(TokenType::KW_REFERENCES)) {
            // Inline foreign key — skip for now, handled as table constraint
            break;
        } else {
            break;
        }
    }

    return col;
}

// ─── Table Constraint ───
ast::TableConstraint Parser::parse_table_constraint() {
    ast::TableConstraint tc;

    if (match(TokenType::KW_CONSTRAINT)) {
        tc.name = expect_identifier("constraint name").value;
    }

    if (check(TokenType::KW_PRIMARY)) {
        advance();
        expect(TokenType::KW_KEY, "PRIMARY KEY");
        tc.type = ast::TableConstraint::Type::PRIMARY_KEY;
        expect(TokenType::LPAREN, "PRIMARY KEY columns");
        tc.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "PRIMARY KEY columns");
    } else if (match(TokenType::KW_UNIQUE)) {
        tc.type = ast::TableConstraint::Type::UNIQUE;
        expect(TokenType::LPAREN, "UNIQUE columns");
        tc.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "UNIQUE columns");
    } else if (check(TokenType::KW_FOREIGN)) {
        advance();
        expect(TokenType::KW_KEY, "FOREIGN KEY");
        tc.type = ast::TableConstraint::Type::FOREIGN_KEY;
        expect(TokenType::LPAREN, "FOREIGN KEY columns");
        tc.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "FOREIGN KEY columns");
        expect(TokenType::KW_REFERENCES, "FOREIGN KEY");
        tc.ref_table = expect_identifier("referenced table").value;
        if (match(TokenType::LPAREN)) {
            tc.ref_columns = parse_identifier_list();
            expect(TokenType::RPAREN, "referenced columns");
        }
        // ON DELETE / ON UPDATE
        while (check(TokenType::KW_ON)) {
            advance();
            if (match(TokenType::KW_DELETE)) {
                if (match(TokenType::KW_CASCADE)) tc.on_delete = "CASCADE";
                else if (match(TokenType::KW_RESTRICT)) tc.on_delete = "RESTRICT";
                else if (check(TokenType::KW_SET)) {
                    advance();
                    if (match(TokenType::KW_NULL_KW)) tc.on_delete = "SET NULL";
                    else if (match(TokenType::KW_DEFAULT)) tc.on_delete = "SET DEFAULT";
                }
            } else if (match(TokenType::KW_UPDATE)) {
                if (match(TokenType::KW_CASCADE)) tc.on_update = "CASCADE";
                else if (match(TokenType::KW_RESTRICT)) tc.on_update = "RESTRICT";
                else if (check(TokenType::KW_SET)) {
                    advance();
                    if (match(TokenType::KW_NULL_KW)) tc.on_update = "SET NULL";
                    else if (match(TokenType::KW_DEFAULT)) tc.on_update = "SET DEFAULT";
                }
            }
        }
    } else if (match(TokenType::KW_CHECK)) {
        tc.type = ast::TableConstraint::Type::CHECK;
        expect(TokenType::LPAREN, "CHECK expression");
        tc.check_expr = parse_expr();
        expect(TokenType::RPAREN, "CHECK expression");
    } else {
        error("Expected PRIMARY, UNIQUE, FOREIGN, or CHECK");
    }

    return tc;
}

// ─── Data Type ───
ast::DataType Parser::parse_data_type() {
    ast::DataType dt;
    dt.name = current_.value;
    advance();

    // Precision/scale: TYPE(p) or TYPE(p, s)
    if (match(TokenType::LPAREN)) {
        Token p = expect(TokenType::INTEGER_LITERAL, "type precision");
        dt.precision = std::stoi(p.value);
        if (match(TokenType::COMMA)) {
            Token s = expect(TokenType::INTEGER_LITERAL, "type scale");
            dt.scale = std::stoi(s.value);
        }
        expect(TokenType::RPAREN, "type parameters");
    }

    // GEOMETRY(type, srid) — handle SRID for spatial types
    if (dt.name == "GEOMETRY" || dt.name == "GEOGRAPHY") {
        // Already handled in the parenthesized form above;
        // precision re-used as sub-type indicator if needed
    }

    return dt;
}

// ─── CREATE INDEX ───
ast::StmtPtr Parser::parse_create_index(bool unique) {
    advance(); // INDEX
    uint32_t stmt_line = current_.line;

    ast::CreateIndexStmt ci;
    ci.unique = unique;

    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        ci.if_not_exists = true;
    }

    ci.name = expect_identifier("index name").value;
    expect(TokenType::KW_ON, "CREATE INDEX");
    ci.table = expect_identifier("table name").value;

    // USING method
    if (match(TokenType::KW_USING)) {
        ci.method = parse_index_method();
    }

    expect(TokenType::LPAREN, "index columns");
    while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
        ast::IndexColumn ic;
        ic.name = expect_identifier("index column").value;
        if (match(TokenType::KW_ASC)) ic.ascending = true;
        else if (match(TokenType::KW_DESC)) ic.ascending = false;
        ci.columns.push_back(std::move(ic));
        if (!match(TokenType::COMMA)) break;
    }
    expect(TokenType::RPAREN, "index columns");

    // Partial index: WHERE ...
    if (check(TokenType::KW_WHERE)) {
        advance();
        ci.where_clause = parse_expr();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_INDEX;
    stmt->line = stmt_line;
    stmt->data = std::move(ci);
    return stmt;
}

ast::IndexMethod Parser::parse_index_method() {
    std::string method = current_.value;
    advance();
    // Normalize to uppercase for comparison
    for (auto &c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (method == "BTREE" || method == "B_TREE") return ast::IndexMethod::BTREE;
    if (method == "BPTREE" || method == "B_PLUS_TREE") return ast::IndexMethod::BPTREE;
    if (method == "HASH") return ast::IndexMethod::HASH;
    if (method == "RTREE" || method == "R_TREE") return ast::IndexMethod::RTREE;
    if (method == "RPTREE" || method == "R_PLUS_TREE") return ast::IndexMethod::RPTREE;
    if (method == "GIST") return ast::IndexMethod::GIST;
    return ast::IndexMethod::BPTREE; // default
}

// ─── CREATE VIEW ───
ast::StmtPtr Parser::parse_create_view(bool or_replace) {
    advance(); // VIEW
    uint32_t stmt_line = current_.line;

    ast::CreateViewStmt cv;
    cv.or_replace = or_replace;

    if (check(TokenType::KW_IF)) {
        advance(); expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        cv.if_not_exists = true;
    }

    std::string name = expect_identifier("view name").value;
    if (match(TokenType::DOT)) {
        cv.schema = name;
        cv.name = expect_identifier("view name").value;
    } else {
        cv.name = name;
    }

    if (match(TokenType::LPAREN)) {
        cv.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "view columns");
    }

    expect(TokenType::KW_AS, "CREATE VIEW");
    cv.query = parse_select();

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_VIEW;
    stmt->line = stmt_line;
    stmt->data = std::move(cv);
    return stmt;
}

// ─── CREATE SEQUENCE ───
ast::StmtPtr Parser::parse_create_sequence() {
    advance(); // SEQUENCE
    uint32_t stmt_line = current_.line;

    ast::CreateSequenceStmt cs;

    // IF NOT EXISTS
    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        cs.if_not_exists = true;
    }

    std::string name = expect_identifier("sequence name").value;
    if (match(TokenType::DOT)) {
        cs.schema = name;
        cs.name = expect_identifier("sequence name").value;
    } else {
        cs.name = name;
    }

    // Sequence options: START [WITH] n, INCREMENT [BY] n, MINVALUE n, MAXVALUE n,
    //                   NO MINVALUE, NO MAXVALUE, CYCLE, NO CYCLE
    auto upper_val = [](const std::string &s) {
        std::string u = s;
        for (auto &c : u) c = (char)std::toupper((unsigned char)c);
        return u;
    };
    while (!check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT)) {
        std::string kw = upper_val(current_.value);
        if (kw == "START") {
            advance();
            if (upper_val(current_.value) == "WITH") advance();
            bool neg = match(TokenType::MINUS);
            auto tok = expect(TokenType::INTEGER_LITERAL, "START value");
            cs.start = std::stoll(tok.value);
            if (neg) cs.start = -cs.start;
        } else if (kw == "INCREMENT") {
            advance();
            if (upper_val(current_.value) == "BY") advance();
            bool neg = match(TokenType::MINUS);
            auto tok = expect(TokenType::INTEGER_LITERAL, "INCREMENT value");
            cs.increment = std::stoll(tok.value);
            if (neg) cs.increment = -cs.increment;
        } else if (kw == "MINVALUE") {
            advance();
            bool neg = match(TokenType::MINUS);
            auto tok = expect(TokenType::INTEGER_LITERAL, "MINVALUE");
            cs.min_value = std::stoll(tok.value);
            if (neg) cs.min_value = -cs.min_value.value();
        } else if (kw == "MAXVALUE") {
            advance();
            bool neg = match(TokenType::MINUS);
            auto tok = expect(TokenType::INTEGER_LITERAL, "MAXVALUE");
            cs.max_value = std::stoll(tok.value);
            if (neg) cs.max_value = -cs.max_value.value();
        } else if (kw == "NO") {
            advance();
            std::string next = upper_val(current_.value);
            advance(); // skip MINVALUE/MAXVALUE/CYCLE
            if (next == "CYCLE") cs.cycle = false;
        } else if (kw == "CYCLE") {
            advance();
            cs.cycle = true;
        } else {
            break; // unknown keyword — stop parsing options
        }
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_SEQUENCE;
    stmt->line = stmt_line;
    stmt->data = std::move(cs);
    return stmt;
}

// ─── ALTER TABLE ───
// Dispatch ALTER {TABLE|TYPE|...}.
ast::StmtPtr Parser::parse_alter() {
    if (peek_.type == TokenType::KW_TABLE) return parse_alter_table();
    if (peek_.type == TokenType::KW_TYPE)  { advance(); return parse_alter_type(); }
    error("Expected TABLE or TYPE after ALTER");
}

ast::StmtPtr Parser::parse_alter_table() {
    advance(); // ALTER
    expect(TokenType::KW_TABLE, "ALTER TABLE");
    uint32_t stmt_line = current_.line;

    ast::AlterTableStmt at;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        at.schema = name;
        at.table = expect_identifier("table name").value;
    } else {
        at.table = name;
    }

    if (match(TokenType::KW_ADD)) {
        if (check(TokenType::KW_COLUMN)) advance();
        if (check(TokenType::KW_PARTITION)) {
            // ALTER TABLE t ADD PARTITION p1 VALUES ...
            at.action = ast::AlterTableStmt::Action::ADD_PARTITION;
            // Need partition type from the existing table (default RANGE for parsing)
            // The partition type determines parsing — we'll use RANGE/LIST/HASH detection
            // by looking at what follows the partition name
            expect(TokenType::KW_PARTITION, "ADD PARTITION");
            at.partition.name = expect_identifier("partition name").value;
            expect(TokenType::KW_VALUES, "ADD PARTITION ... VALUES");
            if (check(TokenType::KW_LESS)) {
                // RANGE: VALUES LESS THAN (expr) | MAXVALUE
                advance(); // LESS
                expect(TokenType::KW_THAN, "VALUES LESS THAN");
                if (check(TokenType::KW_MAXVALUE)) {
                    advance();
                    at.partition.bound.is_maxvalue = true;
                } else {
                    expect(TokenType::LPAREN, "RANGE bound");
                    at.partition.bound.values.push_back(parse_expr());
                    while (match(TokenType::COMMA))
                        at.partition.bound.values.push_back(parse_expr());
                    expect(TokenType::RPAREN, "RANGE bound");
                }
            } else if (check(TokenType::KW_IN)) {
                // LIST: VALUES IN (expr, ...)
                advance(); // IN
                expect(TokenType::LPAREN, "LIST values");
                at.partition.bound.values.push_back(parse_expr());
                while (match(TokenType::COMMA))
                    at.partition.bound.values.push_back(parse_expr());
                expect(TokenType::RPAREN, "LIST values");
            } else if (check(TokenType::KW_WITH)) {
                // HASH: VALUES WITH (MODULUS n, REMAINDER r)
                advance(); // WITH
                expect(TokenType::LPAREN, "HASH partition");
                expect(TokenType::KW_MODULUS, "MODULUS");
                auto mod_tok = expect(TokenType::INTEGER_LITERAL, "modulus");
                at.partition.bound.modulus = std::stoi(mod_tok.value);
                expect(TokenType::COMMA, "HASH partition");
                expect(TokenType::KW_REMAINDER, "REMAINDER");
                auto rem_tok = expect(TokenType::INTEGER_LITERAL, "remainder");
                at.partition.bound.remainder = std::stoi(rem_tok.value);
                expect(TokenType::RPAREN, "HASH partition");
            }
        } else if (check(TokenType::KW_CONSTRAINT) || check(TokenType::KW_PRIMARY) ||
            check(TokenType::KW_FOREIGN) || check(TokenType::KW_UNIQUE) ||
            check(TokenType::KW_CHECK)) {
            at.action = ast::AlterTableStmt::Action::ADD_CONSTRAINT;
            at.constraint = parse_table_constraint();
        } else {
            at.action = ast::AlterTableStmt::Action::ADD_COLUMN;
            at.column = parse_column_def();
        }
    } else if (check(TokenType::KW_DROP)) {
        advance();
        if (match(TokenType::KW_PARTITION)) {
            at.action = ast::AlterTableStmt::Action::DROP_PARTITION;
            at.partition.name = expect_identifier("partition name").value;
        } else if (match(TokenType::KW_COLUMN)) {
            at.action = ast::AlterTableStmt::Action::DROP_COLUMN;
            at.target_name = expect_identifier("column name").value;
        } else if (match(TokenType::KW_CONSTRAINT)) {
            at.action = ast::AlterTableStmt::Action::DROP_CONSTRAINT;
            at.target_name = expect_identifier("constraint name").value;
        }
        if (match(TokenType::KW_CASCADE)) at.cascade = true;
    } else if (match(TokenType::KW_RENAME)) {
        if (match(TokenType::KW_TO)) {
            at.action = ast::AlterTableStmt::Action::RENAME_TABLE;
            at.new_name = expect_identifier("new table name").value;
        } else if (match(TokenType::KW_COLUMN)) {
            at.action = ast::AlterTableStmt::Action::RENAME_COLUMN;
            at.target_name = expect_identifier("column name").value;
            expect(TokenType::KW_TO, "RENAME COLUMN ... TO");
            at.new_name = expect_identifier("new column name").value;
        }
    } else {
        error("Expected ADD, DROP, or RENAME in ALTER TABLE");
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::ALTER_TABLE;
    stmt->line = stmt_line;
    stmt->data = std::move(at);
    return stmt;
}

// ─── DROP ───
ast::StmtPtr Parser::parse_drop() {
    advance(); // DROP
    uint32_t stmt_line = current_.line;

    ast::DropStmt ds;

    if (check(TokenType::KW_TABLE)) { ds.target_type = ast::StmtType::DROP_TABLE; advance(); }
    else if (check(TokenType::KW_INDEX)) { ds.target_type = ast::StmtType::DROP_INDEX; advance(); }
    else if (check(TokenType::KW_VIEW)) { ds.target_type = ast::StmtType::DROP_VIEW; advance(); }
    else if (check(TokenType::KW_MATERIALIZED)) {
        advance();
        expect(TokenType::KW_VIEW, "DROP MATERIALIZED VIEW");
        ds.target_type = ast::StmtType::DROP_MATERIALIZED_VIEW;
    }
    else if (check(TokenType::KW_TABLESPACE)) {
        advance();
        ds.target_type = ast::StmtType::DROP_TABLESPACE;
    }
    else if (check(TokenType::KW_SAVED)) {
        advance();
        match(TokenType::KW_QUERY);
        ds.target_type = ast::StmtType::DROP_SAVED_QUERY;
    }
    else if (check(TokenType::KW_TYPE)) {
        advance();
        ds.target_type = ast::StmtType::DROP_TYPE;
    }
    else if (check(TokenType::KW_DOMAIN)) {
        advance();
        ds.target_type = ast::StmtType::DROP_DOMAIN;
    }
    else if (check(TokenType::KW_TRIGGER)) {
        advance();
        ds.target_type = ast::StmtType::DROP_TRIGGER;
    }
    else if (check(TokenType::KW_SEQUENCE)) {
        advance();
        ds.target_type = ast::StmtType::DROP_SEQUENCE;
    }
    else if (check(TokenType::KW_PROCEDURE)) {
        advance();
        ds.target_type = ast::StmtType::DROP_PROCEDURE;
    }
    else if (check(TokenType::KW_FUNCTION)) {
        advance();
        ds.target_type = ast::StmtType::DROP_FUNCTION;
    }
    else error("Expected TABLE, TYPE, DOMAIN, INDEX, VIEW, SEQUENCE, TRIGGER, PROCEDURE, FUNCTION, or TABLESPACE after DROP");

    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_EXISTS_KW, "IF EXISTS");
        ds.if_exists = true;
    }

    std::string name = expect_identifier("object name").value;
    if (match(TokenType::DOT)) {
        ds.schema = name;
        ds.name = expect_identifier("object name").value;
    } else {
        ds.name = name;
    }

    if (match(TokenType::KW_CASCADE)) ds.cascade = true;
    else if (match(TokenType::KW_RESTRICT)) ds.cascade = false;

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ds.target_type;
    stmt->line = stmt_line;
    stmt->data = std::move(ds);
    return stmt;
}

// ─── TRUNCATE ───
ast::StmtPtr Parser::parse_truncate() {
    advance(); // TRUNCATE
    uint32_t stmt_line = current_.line;
    match(TokenType::KW_TABLE); // optional

    ast::TruncateStmt ts;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        ts.schema = name;
        ts.table = expect_identifier("table name").value;
    } else {
        ts.table = name;
    }
    if (match(TokenType::KW_CASCADE)) ts.cascade = true;

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::TRUNCATE;
    stmt->line = stmt_line;
    stmt->data = std::move(ts);
    return stmt;
}

// ─── SELECT ───
ast::StmtPtr Parser::parse_select_stmt() {
    uint32_t stmt_line = current_.line;
    auto sel = parse_select();

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::SELECT;
    stmt->line = stmt_line;
    stmt->data = std::move(*sel);
    return stmt;
}

ast::SelectPtr Parser::parse_select() {
    auto sel = std::make_unique<ast::SelectStmt>();

    // WITH clause
    std::vector<ast::CTE> saved_ctes;
    if (check(TokenType::KW_WITH)) {
        saved_ctes = parse_ctes();
    }

    // SELECT body
    auto body = parse_select_body();
    *sel = std::move(*body);
    // Restore CTEs (the move from body overwrites them)
    if (!saved_ctes.empty()) sel->ctes = std::move(saved_ctes);

    // Set operations
    while (check(TokenType::KW_UNION) || check(TokenType::KW_INTERSECT) || check(TokenType::KW_EXCEPT)) {
        if (match(TokenType::KW_UNION)) {
            sel->set_op = match(TokenType::KW_ALL) ? ast::SelectStmt::SetOp::UNION_ALL
                                                     : ast::SelectStmt::SetOp::UNION;
        } else if (match(TokenType::KW_INTERSECT)) {
            sel->set_op = match(TokenType::KW_ALL) ? ast::SelectStmt::SetOp::INTERSECT_ALL
                                                     : ast::SelectStmt::SetOp::INTERSECT;
        } else if (match(TokenType::KW_EXCEPT)) {
            sel->set_op = match(TokenType::KW_ALL) ? ast::SelectStmt::SetOp::EXCEPT_ALL
                                                     : ast::SelectStmt::SetOp::EXCEPT;
        }
        sel->set_right = parse_select_body();
    }

    return sel;
}

ast::SelectPtr Parser::parse_select_body() {
    auto sel = std::make_unique<ast::SelectStmt>();
    expect(TokenType::KW_SELECT, "SELECT");

    if (match(TokenType::KW_DISTINCT)) sel->distinct = true;
    else match(TokenType::KW_ALL);

    sel->select_list = parse_select_list();

    if (check(TokenType::KW_FROM)) {
        advance();
        sel->from = parse_from();
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        sel->where_clause = parse_expr();
    }

    if (check(TokenType::KW_GROUP)) {
        advance(); // GROUP
        expect(TokenType::KW_BY, "GROUP BY");

        // Check for ROLLUP, CUBE, or GROUPING SETS after GROUP BY
        if (check(TokenType::KW_ROLLUP)) {
            advance(); // ROLLUP
            expect(TokenType::LPAREN, "ROLLUP(");
            std::vector<ast::ExprPtr> exprs;
            do {
                exprs.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, "ROLLUP)");
            sel->group_by = std::move(exprs);
            sel->group_by_mode = ast::SelectStmt::GroupByMode::ROLLUP;
            // Generate ROLLUP sets: {0,1,...,n-1}, {0,...,n-2}, ..., {0}, {}
            int n = (int)sel->group_by.size();
            for (int i = n; i >= 0; i--) {
                std::vector<int> s;
                for (int j = 0; j < i; j++) s.push_back(j);
                sel->grouping_sets.push_back(std::move(s));
            }
        } else if (check(TokenType::KW_CUBE)) {
            advance(); // CUBE
            expect(TokenType::LPAREN, "CUBE(");
            std::vector<ast::ExprPtr> exprs;
            do {
                exprs.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, "CUBE)");
            sel->group_by = std::move(exprs);
            sel->group_by_mode = ast::SelectStmt::GroupByMode::CUBE;
            // Generate all 2^n subsets in descending size order
            int n = (int)sel->group_by.size();
            int total = 1 << n;
            // Sort subsets: largest first, then by bitmask
            std::vector<int> masks;
            for (int m = 0; m < total; m++) masks.push_back(m);
            std::sort(masks.begin(), masks.end(), [&](int a, int b) {
                int pa = __builtin_popcount(a), pb = __builtin_popcount(b);
                if (pa != pb) return pa > pb;
                return a < b;
            });
            for (int m : masks) {
                std::vector<int> s;
                for (int j = 0; j < n; j++) {
                    if (m & (1 << j)) s.push_back(j);
                }
                sel->grouping_sets.push_back(std::move(s));
            }
        } else if (check(TokenType::KW_GROUPING)) {
            advance(); // GROUPING
            expect(TokenType::KW_SETS, "GROUPING SETS");
            expect(TokenType::LPAREN, "GROUPING SETS(");
            sel->group_by_mode = ast::SelectStmt::GroupByMode::GROUPING_SETS;
            // Parse each set: (a, b), (a), ()
            // First pass: collect all unique expressions and build sets
            std::vector<std::string> expr_names;
            auto find_or_add_expr = [&](ast::ExprPtr e) -> int {
                std::string name;
                if (e->type == ast::ExprType::COLUMN_REF) {
                    auto &ref = std::get<ast::ColumnRef>(e->data);
                    name = ref.column;
                }
                for (int i = 0; i < (int)expr_names.size(); i++) {
                    if (expr_names[i] == name) return i;
                }
                int idx = (int)sel->group_by.size();
                expr_names.push_back(name);
                sel->group_by.push_back(std::move(e));
                return idx;
            };
            do {
                expect(TokenType::LPAREN, "grouping set");
                std::vector<int> s;
                if (!check(TokenType::RPAREN)) {
                    do {
                        auto e = parse_expr();
                        int idx = find_or_add_expr(std::move(e));
                        s.push_back(idx);
                    } while (match(TokenType::COMMA));
                }
                expect(TokenType::RPAREN, "grouping set");
                sel->grouping_sets.push_back(std::move(s));
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, "GROUPING SETS)");
        } else {
            // Simple GROUP BY: comma-separated expressions
            std::vector<ast::ExprPtr> exprs;
            do {
                exprs.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            sel->group_by = std::move(exprs);
        }

        if (check(TokenType::KW_HAVING)) {
            advance();
            sel->having = parse_expr();
        }
    }

    // WINDOW clause
    if (check(TokenType::KW_WINDOW)) {
        advance();
        do {
            std::string wname = expect_identifier("window name").value;
            expect(TokenType::KW_AS, "WINDOW ... AS");
            expect(TokenType::LPAREN, "window specification");
            auto wspec = parse_window_spec();
            expect(TokenType::RPAREN, "window specification");
            sel->window_defs.emplace_back(wname, std::move(wspec));
        } while (match(TokenType::COMMA));
    }

    if (check(TokenType::KW_ORDER)) {
        sel->order_by = parse_order_by();
    }

    // LIMIT / OFFSET
    if (match(TokenType::KW_LIMIT)) {
        sel->limit = parse_expr();
        if (match(TokenType::KW_OFFSET)) {
            sel->offset = parse_expr();
        }
    } else if (match(TokenType::KW_OFFSET)) {
        sel->offset = parse_expr();
        if (match(TokenType::KW_FETCH)) {
            expect(TokenType::KW_NEXT, "FETCH NEXT");
            sel->limit = parse_expr();
            expect(TokenType::KW_ROWS, "FETCH NEXT n ROWS");
            expect(TokenType::KW_ONLY, "FETCH NEXT n ROWS ONLY");
        }
    } else if (match(TokenType::KW_FETCH)) {
        expect(TokenType::KW_NEXT, "FETCH NEXT");
        sel->limit = parse_expr();
        expect(TokenType::KW_ROWS, "FETCH NEXT n ROWS");
        expect(TokenType::KW_ONLY, "FETCH NEXT n ROWS ONLY");
    }

    return sel;
}

std::vector<ast::CTE> Parser::parse_ctes() {
    advance(); // WITH
    std::vector<ast::CTE> ctes;
    bool recursive = match(TokenType::KW_RECURSIVE);

    do {
        ast::CTE cte;
        cte.recursive = recursive;
        cte.name = expect_identifier("CTE name").value;
        if (match(TokenType::LPAREN)) {
            cte.columns = parse_identifier_list();
            expect(TokenType::RPAREN, "CTE columns");
        }
        expect(TokenType::KW_AS, "CTE");
        expect(TokenType::LPAREN, "CTE query");
        cte.query = parse_select();
        expect(TokenType::RPAREN, "CTE query");
        ctes.push_back(std::move(cte));
    } while (match(TokenType::COMMA));

    return ctes;
}

std::vector<ast::ExprPtr> Parser::parse_select_list() {
    std::vector<ast::ExprPtr> list;
    do {
        if (check(TokenType::STAR)) {
            auto expr = std::make_unique<ast::Expr>();
            expr->type = ast::ExprType::COLUMN_REF;
            expr->data = ast::ColumnRef{{}, {}, "*"};
            advance();
            list.push_back(std::move(expr));
        } else {
            auto expr = parse_expr();
            // Alias: expr AS alias or expr alias
            if (match(TokenType::KW_AS)) {
                if (is_identifier_or_keyword()) {
                    expr->alias = current_.value;
                    advance();
                }
            } else if (check(TokenType::IDENTIFIER) &&
                       !check(TokenType::KW_FROM) && !check(TokenType::KW_WHERE)) {
                expr->alias = current_.value;
                advance();
            }
            list.push_back(std::move(expr));
        }
    } while (match(TokenType::COMMA));
    return list;
}

// ─── FROM clause ───
std::vector<ast::TableRefPtr> Parser::parse_from() {
    std::vector<ast::TableRefPtr> refs;
    do {
        auto ref = parse_table_ref();
        // Check for joins
        while (check(TokenType::KW_JOIN) || check(TokenType::KW_INNER) ||
               check(TokenType::KW_LEFT) || check(TokenType::KW_RIGHT) ||
               check(TokenType::KW_FULL) || check(TokenType::KW_CROSS) ||
               check(TokenType::KW_NATURAL)) {
            ref = parse_join(std::move(ref));
        }
        refs.push_back(std::move(ref));
    } while (match(TokenType::COMMA));
    return refs;
}

ast::TableRefPtr Parser::parse_table_ref() {
    auto ref = std::make_unique<ast::TableRef>();

    if (match(TokenType::LPAREN)) {
        // Subquery
        if (check(TokenType::KW_SELECT) || check(TokenType::KW_WITH)) {
            ref->type = ast::TableRefType::SUBQUERY;
            ref->subquery = parse_select();
            expect(TokenType::RPAREN, "subquery");
        } else {
            // Parenthesized table reference — parse inner then close
            auto inner = parse_table_ref();
            expect(TokenType::RPAREN, "parenthesized table ref");
            return inner;
        }
    } else if (match(TokenType::KW_LATERAL)) {
        ref->type = ast::TableRefType::LATERAL;
        expect(TokenType::LPAREN, "LATERAL subquery");
        ref->subquery = parse_select();
        expect(TokenType::RPAREN, "LATERAL subquery");
    } else {
        ref->type = ast::TableRefType::TABLE;
        std::string name = expect_identifier("table name").value;
        if (match(TokenType::DOT)) {
            ref->schema = name;
            ref->name = expect_identifier("table name").value;
        } else {
            ref->name = name;
        }
    }

    // Alias
    if (match(TokenType::KW_AS)) {
        ref->alias = expect_identifier("table alias").value;
    } else if (check(TokenType::IDENTIFIER) &&
               current_.type != TokenType::KW_JOIN && current_.type != TokenType::KW_INNER &&
               current_.type != TokenType::KW_LEFT && current_.type != TokenType::KW_RIGHT &&
               current_.type != TokenType::KW_FULL && current_.type != TokenType::KW_CROSS &&
               current_.type != TokenType::KW_ON && current_.type != TokenType::KW_WHERE &&
               current_.type != TokenType::KW_GROUP && current_.type != TokenType::KW_ORDER &&
               current_.type != TokenType::KW_HAVING && current_.type != TokenType::KW_LIMIT &&
               current_.type != TokenType::KW_UNION && current_.type != TokenType::KW_NATURAL) {
        ref->alias = current_.value;
        advance();
    }

    return ref;
}

ast::TableRefPtr Parser::parse_join(ast::TableRefPtr left) {
    ast::JoinType jtype = ast::JoinType::INNER;

    if (match(TokenType::KW_NATURAL)) {
        jtype = ast::JoinType::NATURAL;
        match(TokenType::KW_JOIN);
    } else if (match(TokenType::KW_CROSS)) {
        jtype = ast::JoinType::CROSS;
        expect(TokenType::KW_JOIN, "CROSS JOIN");
    } else if (match(TokenType::KW_INNER)) {
        jtype = ast::JoinType::INNER;
        expect(TokenType::KW_JOIN, "INNER JOIN");
    } else if (match(TokenType::KW_LEFT)) {
        jtype = ast::JoinType::LEFT;
        match(TokenType::KW_OUTER);
        expect(TokenType::KW_JOIN, "LEFT JOIN");
    } else if (match(TokenType::KW_RIGHT)) {
        jtype = ast::JoinType::RIGHT;
        match(TokenType::KW_OUTER);
        expect(TokenType::KW_JOIN, "RIGHT JOIN");
    } else if (match(TokenType::KW_FULL)) {
        jtype = ast::JoinType::FULL;
        match(TokenType::KW_OUTER);
        expect(TokenType::KW_JOIN, "FULL JOIN");
    } else if (match(TokenType::KW_JOIN)) {
        jtype = ast::JoinType::INNER;
    }

    // The left side becomes a wrapper; we build a JOIN node
    auto result = std::make_unique<ast::TableRef>();
    result->type = ast::TableRefType::JOIN;
    result->join.type = jtype;

    // Parse the right side
    result->join.right = parse_table_ref();

    // ON or USING
    if (match(TokenType::KW_ON)) {
        result->join.on_condition = parse_expr();
    } else if (match(TokenType::KW_USING)) {
        expect(TokenType::LPAREN, "USING");
        result->join.using_columns = parse_identifier_list();
        expect(TokenType::RPAREN, "USING");
    }

    // Transfer left into the result's name/schema to hold reference
    // In a real implementation, the join node would hold both sides
    result->name = left->name;
    result->schema = left->schema;
    result->alias = left->alias;

    return result;
}

std::vector<ast::ExprPtr> Parser::parse_group_by() {
    advance(); // GROUP
    expect(TokenType::KW_BY, "GROUP BY");
    std::vector<ast::ExprPtr> exprs;
    do {
        exprs.push_back(parse_expr());
    } while (match(TokenType::COMMA));
    return exprs;
}

std::vector<ast::OrderByItem> Parser::parse_order_by() {
    advance(); // ORDER
    expect(TokenType::KW_BY, "ORDER BY");
    std::vector<ast::OrderByItem> items;
    do {
        ast::OrderByItem item;
        item.expr = parse_expr();
        if (match(TokenType::KW_ASC)) item.ascending = true;
        else if (match(TokenType::KW_DESC)) item.ascending = false;
        if (check(TokenType::KW_NULLS)) {
            advance();
            if (match(TokenType::KW_FIRST)) item.null_order = ast::OrderByItem::NullOrder::FIRST;
            else if (match(TokenType::KW_LAST)) item.null_order = ast::OrderByItem::NullOrder::LAST;
        }
        items.push_back(std::move(item));
    } while (match(TokenType::COMMA));
    return items;
}

// ─── INSERT ───
ast::StmtPtr Parser::parse_insert() {
    advance(); // INSERT
    expect(TokenType::KW_INTO, "INSERT INTO");
    uint32_t stmt_line = current_.line;

    ast::InsertStmt is;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        is.schema = name;
        is.table = expect_identifier("table name").value;
    } else {
        is.table = name;
    }

    // Column list
    if (match(TokenType::LPAREN)) {
        is.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "column list");
    }

    if (check(TokenType::KW_SELECT) || check(TokenType::KW_WITH)) {
        is.select = parse_select();
    } else {
        expect(TokenType::KW_VALUES, "INSERT VALUES");
        do {
            expect(TokenType::LPAREN, "value list");
            std::vector<ast::ExprPtr> row;
            do {
                row.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, "value list");
            is.values.push_back(std::move(row));
        } while (match(TokenType::COMMA));
    }

    // ON CONFLICT
    if (check(TokenType::KW_ON) && peek_.type == TokenType::KW_CONFLICT) {
        advance(); advance(); // ON CONFLICT
        if (match(TokenType::KW_DO)) {
            if (match(TokenType::KW_NOTHING)) {
                is.on_conflict = "DO NOTHING";
            } else if (check(TokenType::KW_UPDATE)) {
                advance(); // UPDATE
                is.on_conflict = "DO UPDATE";
                expect(TokenType::KW_SET, "ON CONFLICT DO UPDATE SET");
                do {
                    std::string col = expect_identifier("column").value;
                    expect(TokenType::EQ, "=");
                    auto expr = parse_expr();
                    is.on_conflict_updates.emplace_back(col, std::move(expr));
                } while (match(TokenType::COMMA));
            }
        }
    }

    // RETURNING
    if (match(TokenType::KW_RETURNING)) {
        do {
            is.returning.push_back(parse_expr());
        } while (match(TokenType::COMMA));
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::INSERT;
    stmt->line = stmt_line;
    stmt->data = std::move(is);
    return stmt;
}

// ─── UPDATE ───
ast::StmtPtr Parser::parse_update() {
    advance(); // UPDATE
    uint32_t stmt_line = current_.line;

    ast::UpdateStmt us;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        us.schema = name;
        us.table = expect_identifier("table name").value;
    } else {
        us.table = name;
    }

    // Alias
    if (match(TokenType::KW_AS)) {
        us.alias = expect_identifier("alias").value;
    } else if (check(TokenType::IDENTIFIER) && peek_.type == TokenType::KW_SET) {
        us.alias = current_.value;
        advance();
    }

    expect(TokenType::KW_SET, "UPDATE SET");
    do {
        std::string col = expect_identifier("column").value;
        expect(TokenType::EQ, "assignment");
        auto val = parse_expr();
        us.assignments.emplace_back(col, std::move(val));
    } while (match(TokenType::COMMA));

    if (check(TokenType::KW_FROM)) {
        advance();
        us.from = parse_from();
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        us.where_clause = parse_expr();
    }

    // RETURNING
    if (match(TokenType::KW_RETURNING)) {
        do { us.returning.push_back(parse_expr()); } while (match(TokenType::COMMA));
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::UPDATE;
    stmt->line = stmt_line;
    stmt->data = std::move(us);
    return stmt;
}

// ─── DELETE ───
ast::StmtPtr Parser::parse_delete() {
    advance(); // DELETE
    expect(TokenType::KW_FROM, "DELETE FROM");
    uint32_t stmt_line = current_.line;

    ast::DeleteStmt ds;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        ds.schema = name;
        ds.table = expect_identifier("table name").value;
    } else {
        ds.table = name;
    }

    // Alias
    if (match(TokenType::KW_AS)) {
        ds.alias = expect_identifier("alias").value;
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        ds.where_clause = parse_expr();
    }

    // RETURNING
    if (match(TokenType::KW_RETURNING)) {
        do { ds.returning.push_back(parse_expr()); } while (match(TokenType::COMMA));
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::DELETE;
    stmt->line = stmt_line;
    stmt->data = std::move(ds);
    return stmt;
}

// ─── MERGE ───
ast::StmtPtr Parser::parse_merge() {
    advance(); // MERGE
    expect(TokenType::KW_INTO, "MERGE INTO");
    uint32_t stmt_line = current_.line;

    ast::MergeStmt ms;
    ms.target_table = expect_identifier("target table").value;
    if (match(TokenType::KW_AS)) {
        ms.target_alias = expect_identifier("target alias").value;
    } else if (is_identifier_or_keyword() && !check(TokenType::KW_USING)) {
        ms.target_alias = current_.value;
        advance();
    }

    expect(TokenType::KW_USING, "MERGE USING");
    ms.source = parse_table_ref();

    expect(TokenType::KW_ON, "MERGE ON");
    ms.on_condition = parse_expr();

    // WHEN MATCHED / WHEN NOT MATCHED clauses
    while (check(TokenType::KW_WHEN)) {
        advance(); // WHEN
        ast::MergeWhenClause clause;
        if (match(TokenType::KW_MATCHED)) {
            clause.matched = true;
        } else if (match(TokenType::KW_NOT)) {
            expect(TokenType::KW_MATCHED, "WHEN NOT MATCHED");
            clause.matched = false;
        }

        // Optional AND condition
        if (match(TokenType::KW_AND)) {
            clause.condition = parse_expr();
        }

        expect(TokenType::KW_THEN, "WHEN ... THEN");

        if (match(TokenType::KW_UPDATE)) {
            clause.action = ast::MergeWhenClause::Action::UPDATE;
            expect(TokenType::KW_SET, "UPDATE SET");
            do {
                std::string col = expect_identifier("column").value;
                if (match(TokenType::DOT)) {
                    col += "." + expect_identifier("column").value;
                }
                expect(TokenType::EQ, "assignment");
                auto val = parse_expr();
                clause.assignments.emplace_back(col, std::move(val));
            } while (match(TokenType::COMMA));
        } else if (match(TokenType::KW_INSERT)) {
            clause.action = ast::MergeWhenClause::Action::INSERT;
            if (match(TokenType::LPAREN)) {
                clause.columns = parse_identifier_list();
                expect(TokenType::RPAREN, "INSERT columns");
            }
            expect(TokenType::KW_VALUES, "INSERT VALUES");
            expect(TokenType::LPAREN, "INSERT values");
            do {
                clause.values.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, "INSERT values");
        } else if (match(TokenType::KW_DELETE)) {
            clause.action = ast::MergeWhenClause::Action::DELETE;
        }

        ms.when_clauses.push_back(std::move(clause));
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::MERGE;
    stmt->line = stmt_line;
    stmt->data = std::move(ms);
    return stmt;
}

// ─── Transaction Statements ───
ast::StmtPtr Parser::parse_begin() {
    advance(); // BEGIN
    uint32_t stmt_line = current_.line;
    match(TokenType::KW_TRANSACTION);

    ast::BeginStmt bs;
    if (match(TokenType::KW_ISOLATION)) {
        expect(TokenType::KW_LEVEL, "ISOLATION LEVEL");
        if (match(TokenType::KW_READ)) {
            if (match(TokenType::KW_COMMITTED)) bs.isolation_level = "READ COMMITTED";
            else if (match(TokenType::KW_UNCOMMITTED)) bs.isolation_level = "READ UNCOMMITTED";
        } else if (match(TokenType::KW_REPEATABLE)) {
            expect(TokenType::KW_READ, "REPEATABLE READ");
            bs.isolation_level = "REPEATABLE READ";
        } else if (match(TokenType::KW_SERIALIZABLE)) {
            bs.isolation_level = "SERIALIZABLE";
        } else if (match(TokenType::KW_SNAPSHOT)) {
            bs.isolation_level = "SNAPSHOT";
        }
    }
    if (match(TokenType::KW_READ)) {
        if (match(TokenType::KW_ONLY)) bs.read_only = true;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::BEGIN_TXN;
    stmt->line = stmt_line;
    stmt->data = std::move(bs);
    return stmt;
}

ast::StmtPtr Parser::parse_commit() {
    advance();
    uint32_t stmt_line = current_.line;
    match(TokenType::KW_TRANSACTION);
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::COMMIT_TXN;
    stmt->line = stmt_line;
    stmt->data = ast::BeginStmt{};
    return stmt;
}

ast::StmtPtr Parser::parse_rollback() {
    advance();
    uint32_t stmt_line = current_.line;
    match(TokenType::KW_TRANSACTION);

    // ROLLBACK TO SAVEPOINT name
    if (match(TokenType::KW_TO)) {
        match(TokenType::KW_SAVEPOINT);
        ast::SavepointStmt sp;
        sp.name = expect_identifier("savepoint name").value;
        auto stmt = std::make_unique<ast::Statement>();
        stmt->type = ast::StmtType::ROLLBACK_TXN;
        stmt->line = stmt_line;
        stmt->data = std::move(sp);
        return stmt;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::ROLLBACK_TXN;
    stmt->line = stmt_line;
    stmt->data = ast::BeginStmt{};
    return stmt;
}

ast::StmtPtr Parser::parse_savepoint() {
    advance();
    uint32_t stmt_line = current_.line;
    ast::SavepointStmt sp;
    sp.name = expect_identifier("savepoint name").value;
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::SAVEPOINT;
    stmt->line = stmt_line;
    stmt->data = std::move(sp);
    return stmt;
}

ast::StmtPtr Parser::parse_release_savepoint() {
    advance(); // RELEASE
    match(TokenType::KW_SAVEPOINT);
    uint32_t stmt_line = current_.line;
    ast::SavepointStmt sp;
    sp.name = expect_identifier("savepoint name").value;
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::RELEASE_SAVEPOINT;
    stmt->line = stmt_line;
    stmt->data = std::move(sp);
    return stmt;
}

// ─── EXPLAIN ───
ast::StmtPtr Parser::parse_explain() {
    advance(); // EXPLAIN
    uint32_t stmt_line = current_.line;
    ast::ExplainStmt es;
    if (match(TokenType::KW_ANALYZE)) es.analyze = true;
    es.statement = parse_statement();
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::EXPLAIN;
    stmt->line = stmt_line;
    stmt->data = std::move(es);
    return stmt;
}

// ─── Expression Parser (Precedence Climbing) ───

ast::ExprPtr Parser::parse_expr() {
    return parse_or_expr();
}

ast::ExprPtr Parser::parse_or_expr() {
    auto left = parse_and_expr();
    while (match(TokenType::KW_OR)) {
        auto right = parse_and_expr();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BINARY_OP;
        expr->data = ast::BinaryOp{TokenType::KW_OR, std::move(left), std::move(right)};
        left = std::move(expr);
    }
    return left;
}

ast::ExprPtr Parser::parse_and_expr() {
    auto left = parse_not_expr();
    while (match(TokenType::KW_AND)) {
        auto right = parse_not_expr();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BINARY_OP;
        expr->data = ast::BinaryOp{TokenType::KW_AND, std::move(left), std::move(right)};
        left = std::move(expr);
    }
    return left;
}

ast::ExprPtr Parser::parse_not_expr() {
    if (match(TokenType::KW_NOT)) {
        auto operand = parse_not_expr();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::UNARY_OP;
        expr->data = ast::UnaryOp{TokenType::KW_NOT, std::move(operand)};
        return expr;
    }
    return parse_comparison();
}

ast::ExprPtr Parser::parse_comparison() {
    auto left = parse_addition();
    left = parse_postfix(std::move(left));

    if (check(TokenType::EQ) || check(TokenType::NEQ) ||
        check(TokenType::LT) || check(TokenType::GT) ||
        check(TokenType::LTE) || check(TokenType::GTE)) {
        TokenType op = current_.type;
        advance();
        auto right = parse_addition();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BINARY_OP;
        expr->data = ast::BinaryOp{op, std::move(left), std::move(right)};
        return expr;
    }

    return left;
}

ast::ExprPtr Parser::parse_postfix(ast::ExprPtr left) {
    // IS [NOT] NULL / IS [NOT] TRUE / IS [NOT] FALSE / IS [NOT] UNKNOWN
    // IS [NOT] DISTINCT FROM
    if (check(TokenType::KW_IS)) {
        advance();
        bool negated = match(TokenType::KW_NOT);

        if (check(TokenType::KW_NULL_KW)) {
            advance();
            auto expr = std::make_unique<ast::Expr>();
            expr->type = ast::ExprType::IS_NULL_EXPR;
            expr->data = ast::IsNullExpr{std::move(left), negated};
            return expr;
        }
        if (check(TokenType::KW_DISTINCT)) {
            advance();
            expect(TokenType::KW_FROM, "IS [NOT] DISTINCT FROM");
            auto right = parse_addition();
            auto expr = std::make_unique<ast::Expr>();
            expr->type = ast::ExprType::BINARY_OP;
            // Encode IS DISTINCT FROM as NEQ, IS NOT DISTINCT FROM as EQ
            TokenType op = negated ? TokenType::EQ : TokenType::NEQ;
            expr->data = ast::BinaryOp{op, std::move(left), std::move(right)};
            return expr;
        }
        // IS [NOT] TRUE / FALSE / UNKNOWN
        if (check(TokenType::BOOLEAN_LITERAL) || check(TokenType::KW_UNKNOWN)) {
            std::string val = current_.value;
            advance();
            // Compare to boolean literal
            auto bool_val = std::make_unique<ast::Expr>();
            bool_val->type = ast::ExprType::LITERAL;
            if (val == "TRUE") {
                bool_val->data = ast::Literal{TokenType::BOOLEAN_LITERAL, "TRUE"};
            } else if (val == "FALSE") {
                bool_val->data = ast::Literal{TokenType::BOOLEAN_LITERAL, "FALSE"};
            } else {
                bool_val->data = ast::Literal{TokenType::NULL_LITERAL, "NULL"};
            }
            auto expr = std::make_unique<ast::Expr>();
            expr->type = ast::ExprType::BINARY_OP;
            TokenType op = negated ? TokenType::NEQ : TokenType::EQ;
            expr->data = ast::BinaryOp{op, std::move(left), std::move(bool_val)};
            return expr;
        }

        error("Expected NULL, TRUE, FALSE, UNKNOWN, or DISTINCT after IS");
    }

    bool negated = false;
    if (check(TokenType::KW_NOT)) {
        // Look ahead to see if it's NOT IN/BETWEEN/LIKE
        if (peek_.type == TokenType::KW_IN || peek_.type == TokenType::KW_BETWEEN ||
            peek_.type == TokenType::KW_LIKE || peek_.type == TokenType::KW_ILIKE ||
            peek_.type == TokenType::KW_SIMILAR) {
            advance(); // consume NOT
            negated = true;
        } else {
            return left;
        }
    }

    // IN
    if (check(TokenType::KW_IN)) {
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::IN_EXPR;
        expect(TokenType::LPAREN, "IN list");

        ast::InExpr in_expr;
        in_expr.operand = std::move(left);
        in_expr.negated = negated;

        if (check(TokenType::KW_SELECT) || check(TokenType::KW_WITH)) {
            in_expr.values = parse_select();
        } else {
            std::vector<ast::ExprPtr> vals;
            do {
                vals.push_back(parse_expr());
            } while (match(TokenType::COMMA));
            in_expr.values = std::move(vals);
        }
        expect(TokenType::RPAREN, "IN list");
        expr->data = std::move(in_expr);
        return expr;
    }

    // BETWEEN
    if (check(TokenType::KW_BETWEEN)) {
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BETWEEN_EXPR;
        auto low = parse_addition();
        expect(TokenType::KW_AND, "BETWEEN ... AND");
        auto high = parse_addition();
        expr->data = ast::BetweenExpr{std::move(left), negated, std::move(low), std::move(high)};
        return expr;
    }

    // LIKE / ILIKE
    if (check(TokenType::KW_LIKE) || check(TokenType::KW_ILIKE)) {
        bool ilike = check(TokenType::KW_ILIKE);
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LIKE_EXPR;
        auto pattern = parse_addition();
        ast::ExprPtr escape;
        if (match(TokenType::KW_ESCAPE)) {
            escape = parse_primary();
        }
        expr->data = ast::LikeExpr{std::move(left), negated, ilike, std::move(pattern), std::move(escape)};
        return expr;
    }

    // SIMILAR TO (SQL:1999) — treated like LIKE with regex semantics
    if (check(TokenType::KW_SIMILAR)) {
        advance();
        expect(TokenType::KW_TO, "SIMILAR TO");
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LIKE_EXPR;
        auto pattern = parse_addition();
        ast::ExprPtr escape;
        if (match(TokenType::KW_ESCAPE)) {
            escape = parse_primary();
        }
        // Use ilike=false but the function name indicates SIMILAR TO semantics
        expr->data = ast::LikeExpr{std::move(left), negated, false, std::move(pattern), std::move(escape)};
        return expr;
    }

    return left;
}

ast::ExprPtr Parser::parse_addition() {
    auto left = parse_multiplication();
    while (check(TokenType::PLUS) || check(TokenType::MINUS) || check(TokenType::CONCAT)) {
        TokenType op = current_.type;
        advance();
        auto right = parse_multiplication();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BINARY_OP;
        expr->data = ast::BinaryOp{op, std::move(left), std::move(right)};
        left = std::move(expr);
    }
    return left;
}

ast::ExprPtr Parser::parse_multiplication() {
    auto left = parse_unary();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        TokenType op = current_.type;
        advance();
        auto right = parse_unary();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::BINARY_OP;
        expr->data = ast::BinaryOp{op, std::move(left), std::move(right)};
        left = std::move(expr);
    }
    return left;
}

ast::ExprPtr Parser::parse_unary() {
    if (check(TokenType::MINUS) || check(TokenType::PLUS)) {
        TokenType op = current_.type;
        advance();
        auto operand = parse_unary();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::UNARY_OP;
        expr->data = ast::UnaryOp{op, std::move(operand)};
        return expr;
    }
    return parse_primary();
}

ast::ExprPtr Parser::parse_primary() {
    // Typed-literal: TIMESTAMP 'YYYY-MM-DD HH:MM:SS', DATE 'YYYY-MM-DD',
    // TIME 'HH:MM:SS'. Lowered to a function call on the appropriate
    // *_PARSE builtin so the existing parse_date/parse_time/parse_timestamp
    // helpers do the work at runtime.
    if ((check(TokenType::KW_TIMESTAMP) || check(TokenType::KW_DATE) || check(TokenType::KW_TIME))
        && peek_.type == TokenType::STRING_LITERAL) {
        const char *fn = nullptr;
        if (check(TokenType::KW_TIMESTAMP)) fn = "TS_PARSE";
        else if (check(TokenType::KW_DATE)) fn = "DATE_PARSE";
        else                                fn = "TIME_PARSE";
        advance(); // consume the type keyword
        std::string lit = current_.value;
        advance(); // consume the string literal
        auto lit_expr = std::make_unique<ast::Expr>();
        lit_expr->type = ast::ExprType::LITERAL;
        lit_expr->data = ast::Literal{TokenType::STRING_LITERAL, lit};
        std::vector<ast::ExprPtr> args;
        args.push_back(std::move(lit_expr));
        auto call = std::make_unique<ast::Expr>();
        call->type = ast::ExprType::FUNCTION_CALL;
        call->data = ast::FunctionCall{fn, std::move(args), false};
        return call;
    }

    // NULL
    if (match(TokenType::KW_NULL_KW)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LITERAL;
        expr->data = ast::Literal{TokenType::NULL_LITERAL, "NULL"};
        return expr;
    }

    // Boolean
    if (check(TokenType::BOOLEAN_LITERAL)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LITERAL;
        expr->data = ast::Literal{TokenType::BOOLEAN_LITERAL, current_.value};
        advance();
        return expr;
    }

    // Numeric literals
    if (check(TokenType::INTEGER_LITERAL) || check(TokenType::FLOAT_LITERAL)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LITERAL;
        expr->data = ast::Literal{current_.type, current_.value};
        advance();
        return expr;
    }

    // String literal
    if (check(TokenType::STRING_LITERAL)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LITERAL;
        expr->data = ast::Literal{TokenType::STRING_LITERAL, current_.value};
        advance();
        return expr;
    }

    // Blob literal
    if (check(TokenType::BLOB_LITERAL)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::LITERAL;
        expr->data = ast::Literal{TokenType::BLOB_LITERAL, current_.value};
        advance();
        return expr;
    }

    // Parameter: ?
    if (match(TokenType::QUESTION_MARK)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::PARAMETER;
        expr->data = ast::Literal{TokenType::QUESTION_MARK, "?"};
        return expr;
    }

    // CASE
    if (check(TokenType::KW_CASE)) {
        return parse_case();
    }

    // CAST
    if (check(TokenType::KW_CAST)) {
        return parse_cast();
    }

    // EXISTS
    if (check(TokenType::KW_EXISTS_KW)) {
        return parse_exists();
    }

    // Parenthesized expression or subquery
    if (check(TokenType::LPAREN)) {
        advance();
        if (check(TokenType::KW_SELECT) || check(TokenType::KW_WITH)) {
            auto sq = parse_select();
            expect(TokenType::RPAREN, "subquery");
            auto expr = std::make_unique<ast::Expr>();
            expr->type = ast::ExprType::SUBQUERY;
            expr->data = ast::SubqueryExpr{std::move(sq)};
            return expr;
        }
        auto inner = parse_expr();
        expect(TokenType::RPAREN, "parenthesized expression");
        return inner;
    }

    // Identifier: column ref, function call, or table.column
    if (check(TokenType::IDENTIFIER) || check(TokenType::QUOTED_IDENTIFIER)) {
        std::string name = current_.value;
        advance();

        // Function call
        if (check(TokenType::LPAREN)) {
            return parse_function_call(name);
        }

        // Qualified name: schema.table.column or table.column
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::COLUMN_REF;
        ast::ColumnRef ref;

        if (match(TokenType::DOT)) {
            std::string second = current_.value;
            advance();
            if (match(TokenType::DOT)) {
                ref.schema = name;
                ref.table = second;
                ref.column = current_.value;
                advance();
            } else {
                ref.table = name;
                ref.column = second;
            }
        } else {
            ref.column = name;
        }
        expr->data = std::move(ref);

        // OVER clause => window function on a column ref (rare but valid)
        // Usually handled in function call path

        // :: cast operator
        if (match(TokenType::DOUBLE_COLON)) {
            auto cast = std::make_unique<ast::Expr>();
            cast->type = ast::ExprType::CAST_EXPR;
            cast->data = ast::CastExpr{std::move(expr), parse_data_type()};
            return cast;
        }

        return expr;
    }

    // Aggregate and window function keywords used as function names
    if (check(TokenType::KW_COUNT) || check(TokenType::KW_SUM) ||
        check(TokenType::KW_AVG) || check(TokenType::KW_MIN) ||
        check(TokenType::KW_MAX) ||
        check(TokenType::KW_ROW_NUMBER) || check(TokenType::KW_RANK) ||
        check(TokenType::KW_DENSE_RANK) || check(TokenType::KW_NTILE) ||
        check(TokenType::KW_LAG) || check(TokenType::KW_LEAD) ||
        check(TokenType::KW_FIRST_VALUE) || check(TokenType::KW_LAST_VALUE) ||
        check(TokenType::KW_NTH_VALUE) ||
        check(TokenType::KW_PERCENT_RANK) || check(TokenType::KW_CUME_DIST) ||
        check(TokenType::KW_LISTAGG) ||
        check(TokenType::KW_GROUPING) ||
        check(TokenType::KW_COALESCE) || check(TokenType::KW_NULLIF) ||
        check(TokenType::KW_GREATEST) || check(TokenType::KW_LEAST) ||
        check(TokenType::KW_UPPER) || check(TokenType::KW_LOWER) ||
        check(TokenType::KW_CONCAT) ||
        check(TokenType::KW_SUBSTRING) || check(TokenType::KW_TRIM) ||
        check(TokenType::KW_POSITION) || check(TokenType::KW_OVERLAY) ||
        check(TokenType::KW_NORMALIZE) || check(TokenType::KW_TRANSLATE) ||
        check(TokenType::KW_CONVERT) ||
        check(TokenType::KW_JSON_OBJECT) || check(TokenType::KW_JSON_ARRAY) ||
        check(TokenType::KW_JSON_VALUE) || check(TokenType::KW_JSON_QUERY) ||
        check(TokenType::KW_JSON_TABLE) || check(TokenType::KW_JSON_EXISTS) ||
        check(TokenType::KW_XMLELEMENT) || check(TokenType::KW_XMLFOREST) ||
        check(TokenType::KW_XMLAGG) || check(TokenType::KW_XMLPARSE) ||
        check(TokenType::KW_XMLSERIALIZE) ||
        // Date/time extraction functions
        check(TokenType::KW_YEAR) || check(TokenType::KW_MONTH) ||
        check(TokenType::KW_DAY) || check(TokenType::KW_HOUR) ||
        check(TokenType::KW_MINUTE) || check(TokenType::KW_SECOND) ||
        // String functions that are also keywords
        check(TokenType::KW_LEFT) || check(TokenType::KW_RIGHT) ||
        check(TokenType::KW_REPLACE) || check(TokenType::KW_REPEAT) ||
        // Conditional
        check(TokenType::KW_IF) ||
        // Misc keyword-functions
        check(TokenType::KW_ACTION)) {
        std::string name = current_.value;
        advance();
        return parse_function_call(name);
    }

    // EXTRACT(field FROM expr) — special syntax
    if (check(TokenType::KW_EXTRACT)) {
        advance();
        expect(TokenType::LPAREN, "EXTRACT");
        std::string field = current_.value;
        advance(); // field name (YEAR, MONTH, DAY, etc.)
        expect(TokenType::KW_FROM, "EXTRACT ... FROM");
        auto operand = parse_expr();
        expect(TokenType::RPAREN, "EXTRACT");
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::FUNCTION_CALL;
        std::vector<ast::ExprPtr> args;
        auto field_expr = std::make_unique<ast::Expr>();
        field_expr->type = ast::ExprType::LITERAL;
        field_expr->data = ast::Literal{TokenType::STRING_LITERAL, field};
        args.push_back(std::move(field_expr));
        args.push_back(std::move(operand));
        expr->data = ast::FunctionCall{"EXTRACT", std::move(args), false};
        return expr;
    }

    // CURRENT_DATE, CURRENT_TIME, CURRENT_TIMESTAMP — nilary functions
    if (check(TokenType::KW_CURRENT_DATE) || check(TokenType::KW_CURRENT_TIME) ||
        check(TokenType::KW_CURRENT_TIMESTAMP) ||
        check(TokenType::KW_LOCALTIME) || check(TokenType::KW_LOCALTIMESTAMP)) {
        std::string name = current_.value;
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::FUNCTION_CALL;
        expr->data = ast::FunctionCall{name, {}, false};
        return expr;
    }

    error("Expected expression, got: " + current_.value);
}

ast::ExprPtr Parser::parse_function_call(const std::string &name) {
    advance(); // (

    // Aggregate functions
    bool is_agg = (name == "COUNT" || name == "SUM" || name == "AVG" ||
                   name == "MIN" || name == "MAX" ||
                   name == "LISTAGG" || name == "XMLAGG" ||
                   name == "PERCENTILE_CONT" || name == "PERCENTILE_DISC" ||
                   name == "MODE" ||
                   name == "STDDEV" || name == "STDDEV_POP" || name == "STDDEV_SAMP" ||
                   name == "STDEV" ||
                   name == "VARIANCE" || name == "VAR_POP" || name == "VAR_SAMP" ||
                   name == "VARP" || name == "VAR" ||
                   name == "STRING_AGG" || name == "GROUP_CONCAT" ||
                   name == "MEDIAN");

    bool distinct = false;
    if (match(TokenType::KW_DISTINCT)) distinct = true;

    std::vector<ast::ExprPtr> args;
    if (!check(TokenType::RPAREN)) {
        // COUNT(*)
        if (check(TokenType::STAR) && is_agg) {
            auto star = std::make_unique<ast::Expr>();
            star->type = ast::ExprType::COLUMN_REF;
            star->data = ast::ColumnRef{{}, {}, "*"};
            advance();
            args.push_back(std::move(star));
        } else {
            do {
                args.push_back(parse_expr());
            } while (match(TokenType::COMMA));
        }
    }
    expect(TokenType::RPAREN, "function call");

    // Check for OVER => window function
    if (check(TokenType::KW_OVER)) {
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::WINDOW_CALL;

        ast::WindowSpec wspec;
        if (check(TokenType::IDENTIFIER)) {
            wspec.ref_name = current_.value;
            advance();
        } else {
            expect(TokenType::LPAREN, "window specification");
            wspec = parse_window_spec();
            expect(TokenType::RPAREN, "window specification");
        }

        expr->data = ast::WindowCall{name, std::move(args), distinct, std::move(wspec)};
        return expr;
    }

    // Aggregate vs regular function
    if (is_agg) {
        auto expr = std::make_unique<ast::Expr>();
        expr->type = ast::ExprType::AGGREGATE_CALL;
        ast::ExprPtr filter;
        // FILTER (WHERE ...) — SQL:2003
        if (check(TokenType::KW_FILTER)) {
            advance();
            expect(TokenType::LPAREN, "FILTER");
            expect(TokenType::KW_WHERE, "FILTER (WHERE ...)");
            filter = parse_expr();
            expect(TokenType::RPAREN, "FILTER (WHERE ...)");
        }
        expr->data = ast::AggregateCall{name, std::move(args), distinct, std::move(filter)};

        // :: cast operator
        if (match(TokenType::DOUBLE_COLON)) {
            auto cast = std::make_unique<ast::Expr>();
            cast->type = ast::ExprType::CAST_EXPR;
            cast->data = ast::CastExpr{std::move(expr), parse_data_type()};
            return cast;
        }

        return expr;
    }

    auto expr = std::make_unique<ast::Expr>();
    expr->type = ast::ExprType::FUNCTION_CALL;
    expr->data = ast::FunctionCall{name, std::move(args), distinct};

    // :: cast operator
    if (match(TokenType::DOUBLE_COLON)) {
        auto cast = std::make_unique<ast::Expr>();
        cast->type = ast::ExprType::CAST_EXPR;
        cast->data = ast::CastExpr{std::move(expr), parse_data_type()};
        return cast;
    }

    return expr;
}

ast::ExprPtr Parser::parse_case() {
    advance(); // CASE

    ast::CaseExpr ce;

    // Simple CASE vs searched CASE
    if (!check(TokenType::KW_WHEN)) {
        ce.operand = parse_expr();
    }

    while (match(TokenType::KW_WHEN)) {
        auto when_expr = parse_expr();
        expect(TokenType::KW_THEN, "CASE WHEN ... THEN");
        auto then_expr = parse_expr();
        ce.when_clauses.emplace_back(std::move(when_expr), std::move(then_expr));
    }

    if (match(TokenType::KW_ELSE)) {
        ce.else_clause = parse_expr();
    }

    expect(TokenType::KW_END, "CASE ... END");

    auto expr = std::make_unique<ast::Expr>();
    expr->type = ast::ExprType::CASE_EXPR;
    expr->data = std::move(ce);
    return expr;
}

ast::ExprPtr Parser::parse_cast() {
    advance(); // CAST
    expect(TokenType::LPAREN, "CAST");
    auto operand = parse_expr();
    expect(TokenType::KW_AS, "CAST ... AS");
    auto type = parse_data_type();
    expect(TokenType::RPAREN, "CAST");

    auto expr = std::make_unique<ast::Expr>();
    expr->type = ast::ExprType::CAST_EXPR;
    expr->data = ast::CastExpr{std::move(operand), std::move(type)};
    return expr;
}

ast::ExprPtr Parser::parse_exists() {
    advance(); // EXISTS
    expect(TokenType::LPAREN, "EXISTS");
    auto sq = parse_select();
    expect(TokenType::RPAREN, "EXISTS");

    auto expr = std::make_unique<ast::Expr>();
    expr->type = ast::ExprType::EXISTS_EXPR;
    expr->data = ast::ExistsExpr{std::move(sq)};
    return expr;
}

ast::ExprPtr Parser::parse_subquery_expr() {
    auto sq = parse_select();
    auto expr = std::make_unique<ast::Expr>();
    expr->type = ast::ExprType::SUBQUERY;
    expr->data = ast::SubqueryExpr{std::move(sq)};
    return expr;
}

// ─── Window Specification ───
ast::WindowSpec Parser::parse_window_spec() {
    ast::WindowSpec ws;

    if (check(TokenType::KW_PARTITION)) {
        advance();
        expect(TokenType::KW_BY, "PARTITION BY");
        do {
            ws.partition_by.push_back(parse_expr());
        } while (match(TokenType::COMMA));
    }

    if (check(TokenType::KW_ORDER)) {
        advance();
        expect(TokenType::KW_BY, "ORDER BY in window");
        do {
            auto expr = parse_expr();
            bool asc = true;
            if (match(TokenType::KW_DESC)) asc = false;
            else match(TokenType::KW_ASC);
            ws.order_by.emplace_back(std::move(expr), asc);
        } while (match(TokenType::COMMA));
    }

    if (check(TokenType::KW_ROWS) || check(TokenType::KW_RANGE) || check(TokenType::KW_GROUPS)) {
        ws.frame = parse_window_frame();
    }

    return ws;
}

ast::WindowFrame Parser::parse_window_frame() {
    ast::WindowFrame frame;
    if (match(TokenType::KW_ROWS)) frame.type = ast::WindowFrame::Type::ROWS;
    else if (match(TokenType::KW_RANGE)) frame.type = ast::WindowFrame::Type::RANGE;
    else if (match(TokenType::KW_GROUPS)) frame.type = ast::WindowFrame::Type::GROUPS;

    // BETWEEN start AND end, or just start
    if (match(TokenType::KW_BETWEEN)) {
        // start
        if (match(TokenType::KW_UNBOUNDED)) {
            expect(TokenType::KW_PRECEDING, "UNBOUNDED PRECEDING");
            frame.start = ast::WindowFrame::Bound::UNBOUNDED_PRECEDING;
        } else if (check(TokenType::KW_CURRENT)) {
            advance();
            expect(TokenType::KW_ROW, "CURRENT ROW");
            frame.start = ast::WindowFrame::Bound::CURRENT_ROW;
        } else {
            frame.start_offset = parse_expr();
            if (match(TokenType::KW_PRECEDING)) frame.start = ast::WindowFrame::Bound::PRECEDING;
            else { expect(TokenType::KW_FOLLOWING, "PRECEDING or FOLLOWING"); frame.start = ast::WindowFrame::Bound::FOLLOWING; }
        }

        expect(TokenType::KW_AND, "BETWEEN ... AND");

        // end
        if (match(TokenType::KW_UNBOUNDED)) {
            expect(TokenType::KW_FOLLOWING, "UNBOUNDED FOLLOWING");
            frame.end = ast::WindowFrame::Bound::UNBOUNDED_FOLLOWING;
        } else if (check(TokenType::KW_CURRENT)) {
            advance();
            expect(TokenType::KW_ROW, "CURRENT ROW");
            frame.end = ast::WindowFrame::Bound::CURRENT_ROW;
        } else {
            frame.end_offset = parse_expr();
            if (match(TokenType::KW_PRECEDING)) frame.end = ast::WindowFrame::Bound::PRECEDING;
            else { expect(TokenType::KW_FOLLOWING, "PRECEDING or FOLLOWING"); frame.end = ast::WindowFrame::Bound::FOLLOWING; }
        }
    } else {
        // Single bound
        if (match(TokenType::KW_UNBOUNDED)) {
            expect(TokenType::KW_PRECEDING, "UNBOUNDED PRECEDING");
            frame.start = ast::WindowFrame::Bound::UNBOUNDED_PRECEDING;
        } else if (check(TokenType::KW_CURRENT)) {
            advance();
            expect(TokenType::KW_ROW, "CURRENT ROW");
            frame.start = ast::WindowFrame::Bound::CURRENT_ROW;
        }
    }

    return frame;
}

// ─── Helpers ───
std::string Parser::parse_qualified_name() {
    std::string name = expect_identifier("name").value;
    if (match(TokenType::DOT)) {
        name += "." + expect_identifier("qualified name").value;
    }
    return name;
}

std::vector<std::string> Parser::parse_identifier_list() {
    std::vector<std::string> ids;
    do {
        ids.push_back(expect_identifier("identifier").value);
    } while (match(TokenType::COMMA));
    return ids;
}

// ─── CREATE MATERIALIZED VIEW ───
ast::StmtPtr Parser::parse_create_materialized_view(bool or_replace) {
    advance(); // MATERIALIZED
    expect(TokenType::KW_VIEW, "CREATE MATERIALIZED VIEW");
    uint32_t stmt_line = current_.line;

    ast::CreateMaterializedViewStmt mv;
    mv.or_replace = or_replace;

    if (check(TokenType::KW_IF)) {
        advance();
        expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        mv.if_not_exists = true;
    }

    std::string name = expect_identifier("materialized view name").value;
    if (match(TokenType::DOT)) {
        mv.schema = name;
        mv.name = expect_identifier("materialized view name").value;
    } else {
        mv.name = name;
    }

    // WRITABLE keyword
    if (match(TokenType::KW_WRITABLE)) {
        mv.writable = true;
    }

    // Optional column list
    if (match(TokenType::LPAREN)) {
        mv.columns = parse_identifier_list();
        expect(TokenType::RPAREN, "column list");
    }

    // TABLESPACE clause
    if (match(TokenType::KW_TABLESPACE)) {
        mv.tablespace = expect_identifier("tablespace name").value;
    }

    expect(TokenType::KW_AS, "CREATE MATERIALIZED VIEW ... AS");
    mv.query = parse_select();

    // WITH [NO] DATA
    if (check(TokenType::KW_WITH)) {
        advance();
        if (match(TokenType::KW_NO)) {
            // "WITH NO DATA" context: next should be a data-like identifier
            // Just set with_data = false
            mv.with_data = false;
        } else {
            mv.with_data = true;
        }
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_MATERIALIZED_VIEW;
    stmt->line = stmt_line;
    stmt->data = std::move(mv);
    return stmt;
}

// ─── REFRESH MATERIALIZED VIEW ───
ast::StmtPtr Parser::parse_refresh_materialized_view() {
    advance(); // REFRESH
    expect(TokenType::KW_MATERIALIZED, "REFRESH MATERIALIZED VIEW");
    expect(TokenType::KW_VIEW, "REFRESH MATERIALIZED VIEW");
    uint32_t stmt_line = current_.line;

    ast::RefreshMaterializedViewStmt rmv;

    if (match(TokenType::KW_CONCURRENTLY)) {
        rmv.concurrently = true;
    }

    std::string name = expect_identifier("materialized view name").value;
    if (match(TokenType::DOT)) {
        rmv.schema = name;
        rmv.name = expect_identifier("materialized view name").value;
    } else {
        rmv.name = name;
    }

    if (check(TokenType::KW_WITH)) {
        advance();
        if (match(TokenType::KW_NO)) {
            rmv.with_data = false;
        } else {
            rmv.with_data = true;
        }
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::REFRESH_MATERIALIZED_VIEW;
    stmt->line = stmt_line;
    stmt->data = std::move(rmv);
    return stmt;
}

// ─── CREATE SAVED QUERY ───
ast::StmtPtr Parser::parse_create_saved_query(bool or_replace) {
    advance(); // SAVED
    match(TokenType::KW_QUERY); // optional QUERY keyword
    uint32_t stmt_line = current_.line;

    ast::CreateSavedQueryStmt sq;
    sq.or_replace = or_replace;

    std::string name = expect_identifier("saved query name").value;
    if (match(TokenType::DOT)) {
        sq.schema = name;
        sq.name = expect_identifier("saved query name").value;
    } else {
        sq.name = name;
    }

    // Optional parameter list: (param_name TYPE, ...)
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
            std::string pname = expect_identifier("parameter name").value;
            auto ptype = parse_data_type();
            sq.parameters.emplace_back(pname, std::move(ptype));
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, "parameter list");
    }

    expect(TokenType::KW_AS, "CREATE SAVED QUERY ... AS");
    sq.query = parse_select();

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_SAVED_QUERY;
    stmt->line = stmt_line;
    stmt->data = std::move(sq);
    return stmt;
}

// ─── CREATE TABLESPACE ───
ast::StmtPtr Parser::parse_create_tablespace() {
    advance(); // TABLESPACE
    uint32_t stmt_line = current_.line;

    ast::CreateTablespaceStmt ts;
    ts.name = expect_identifier("tablespace name").value;

    // LOCATION 'path'
    if (match(TokenType::KW_LOCATION)) {
        ts.location = expect(TokenType::STRING_LITERAL, "tablespace location").value;
    }

    // OWNER name
    if (match(TokenType::KW_OWNER)) {
        ts.owner = expect_identifier("owner name").value;
    }

    // WITH ( option = value, ... )
    if (check(TokenType::KW_WITH)) {
        advance();
        expect(TokenType::LPAREN, "tablespace options");
        while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
            std::string key = expect_identifier("option name").value;
            expect(TokenType::EQ, "option assignment");
            std::string val;
            if (check(TokenType::STRING_LITERAL)) {
                val = current_.value;
                advance();
            } else {
                val = expect_identifier("option value").value;
            }
            ts.options[key] = val;
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, "tablespace options");
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_TABLESPACE;
    stmt->line = stmt_line;
    stmt->data = std::move(ts);
    return stmt;
}

// ─── LOCK ───
ast::StmtPtr Parser::parse_lock() {
    advance(); // LOCK
    uint32_t stmt_line = current_.line;

    ast::LockStmt ls;

    // Optional TABLE keyword
    match(TokenType::KW_TABLE);

    std::string name = expect_identifier("table/object name").value;
    if (match(TokenType::DOT)) {
        ls.schema = name;
        ls.table = expect_identifier("table/object name").value;
    } else {
        ls.table = name;
    }

    // IN ... MODE
    if (match(TokenType::KW_IN)) {
        // Lock mode keywords
        if (match(TokenType::KW_ACCESS)) {
            if (match(TokenType::KW_SHARE)) ls.mode = ast::LockMode::ACCESS_SHARE;
            else { match(TokenType::KW_EXCLUSIVE_KW); ls.mode = ast::LockMode::ACCESS_EXCLUSIVE; }
        } else if (check(TokenType::KW_ROW)) {
            advance();
            if (match(TokenType::KW_SHARE)) ls.mode = ast::LockMode::ROW_SHARE;
            else { match(TokenType::KW_EXCLUSIVE_KW); ls.mode = ast::LockMode::ROW_EXCLUSIVE; }
        } else if (match(TokenType::KW_SHARE)) {
            if (check(TokenType::KW_ROW)) { advance(); match(TokenType::KW_EXCLUSIVE_KW); ls.mode = ast::LockMode::SHARE_ROW_EXCLUSIVE; }
            else if (match(TokenType::KW_UPDATE)) ls.mode = ast::LockMode::SHARE_UPDATE_EXCLUSIVE;
            else ls.mode = ast::LockMode::SHARE;
        } else if (match(TokenType::KW_EXCLUSIVE_KW)) {
            ls.mode = ast::LockMode::EXCLUSIVE;
        } else if (match(TokenType::KW_INTENT)) {
            if (match(TokenType::KW_SHARE)) ls.mode = ast::LockMode::INTENT_SHARED;
            else { match(TokenType::KW_EXCLUSIVE_KW); ls.mode = ast::LockMode::INTENT_EXCLUSIVE; }
        }
        // Consume optional MODE keyword
        match(TokenType::KW_MODE);
    }

    // Lock granularity
    if (match(TokenType::KW_OBJECT)) {
        ls.granularity = ast::LockGranularity::OBJECT;
        ls.object_name = expect_identifier("object name").value;
    } else if (match(TokenType::KW_ADVISORY)) {
        ls.granularity = ast::LockGranularity::ADVISORY;
    } else if (check(TokenType::KW_WHERE)) {
        // Row-level lock with WHERE clause
        ls.granularity = ast::LockGranularity::ROW;
        advance();
        ls.row_condition = parse_expr();
    }

    if (match(TokenType::KW_NOWAIT) || match(TokenType::KW_NOWAIT_KW)) {
        ls.nowait = true;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::LOCK_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(ls);
    return stmt;
}

ast::StmtPtr Parser::parse_unlock() {
    advance(); // UNLOCK
    uint32_t stmt_line = current_.line;
    match(TokenType::KW_TABLE);

    ast::UnlockStmt us;
    std::string name = expect_identifier("table name").value;
    if (match(TokenType::DOT)) {
        us.schema = name;
        us.table = expect_identifier("table name").value;
    } else {
        us.table = name;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::UNLOCK_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(us);
    return stmt;
}

// ─── LOGGING / AUDIT ───
ast::StmtPtr Parser::parse_logging() {
    uint32_t stmt_line = current_.line;
    ast::LoggingStmt ls;

    if (match(TokenType::KW_LOGGING)) {
        ls.action = ast::LoggingStmt::Action::ENABLE;
        if (match(TokenType::KW_LEVEL)) {
            ls.action = ast::LoggingStmt::Action::SET_LEVEL;
            ls.level = expect_identifier("log level").value;
        } else if (is_identifier_or_keyword()) {
            ls.target = current_.value;
            advance();
        }
    } else if (match(TokenType::KW_NOLOGGING)) {
        ls.action = ast::LoggingStmt::Action::DISABLE;
        if (is_identifier_or_keyword()) {
            ls.target = current_.value;
            advance();
        }
    } else if (match(TokenType::KW_AUDIT)) {
        ls.action = ast::LoggingStmt::Action::AUDIT_ENABLE;
        // AUDIT SELECT, INSERT, UPDATE ON table_name
        while (is_identifier_or_keyword() && !check(TokenType::KW_ON)) {
            ls.audit_events.push_back(current_.value);
            advance();
            match(TokenType::COMMA);
        }
        if (match(TokenType::KW_ON)) {
            ls.target = expect_identifier("table name").value;
        }
    } else if (match(TokenType::KW_NOAUDIT)) {
        ls.action = ast::LoggingStmt::Action::AUDIT_DISABLE;
        if (is_identifier_or_keyword()) {
            ls.target = current_.value;
            advance();
        }
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::LOGGING_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(ls);
    return stmt;
}

// ─── XPATH query ───
ast::StmtPtr Parser::parse_xpath_query() {
    advance(); // XPATH
    uint32_t stmt_line = current_.line;

    ast::XPathQueryStmt xp;
    xp.xpath_expr = expect(TokenType::STRING_LITERAL, "XPath expression").value;

    expect(TokenType::KW_ON, "XPATH ... ON");
    xp.source_table = expect_identifier("table name").value;
    expect(TokenType::DOT, "table.column");
    xp.source_column = expect_identifier("column name").value;

    if (match(TokenType::KW_AS)) {
        xp.alias = expect_identifier("alias").value;
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        xp.where_clause = parse_expr();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::XPATH_QUERY;
    stmt->line = stmt_line;
    stmt->data = std::move(xp);
    return stmt;
}

// ─── XQUERY ───
ast::StmtPtr Parser::parse_xquery() {
    advance(); // XQUERY
    uint32_t stmt_line = current_.line;

    ast::XQueryStmt xq;
    xq.xquery_expr = expect(TokenType::STRING_LITERAL, "XQuery expression").value;

    // PASSING clause: PASSING expr AS name, ...
    if (match(TokenType::KW_PASSING)) {
        do {
            auto val = parse_expr();
            expect(TokenType::KW_AS, "PASSING ... AS");
            std::string pname = expect_identifier("parameter name").value;
            xq.passing_params.emplace_back(pname, std::move(val));
        } while (match(TokenType::COMMA));
    }

    expect(TokenType::KW_ON, "XQUERY ... ON");
    xq.source_table = expect_identifier("table name").value;
    expect(TokenType::DOT, "table.column");
    xq.source_column = expect_identifier("column name").value;

    if (match(TokenType::KW_AS)) {
        xq.alias = expect_identifier("alias").value;
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        xq.where_clause = parse_expr();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::XQUERY_QUERY;
    stmt->line = stmt_line;
    stmt->data = std::move(xq);
    return stmt;
}

// ─── GRAPHQL ───
ast::StmtPtr Parser::parse_graphql_query() {
    advance(); // GRAPHQL
    uint32_t stmt_line = current_.line;

    ast::GraphQLQueryStmt gql;
    gql.graphql_query = expect(TokenType::STRING_LITERAL, "GraphQL query").value;

    expect(TokenType::KW_ON, "GRAPHQL ... ON");
    gql.source_table = expect_identifier("table name").value;
    expect(TokenType::DOT, "table.column");
    gql.source_column = expect_identifier("column name").value;

    if (match(TokenType::KW_AS)) {
        gql.alias = expect_identifier("alias").value;
    }

    // JOIN clause: JOIN table ON condition
    while (check(TokenType::KW_JOIN)) {
        advance();
        std::string join_table = expect_identifier("join table").value;
        expect(TokenType::KW_ON, "JOIN ... ON");
        // Read the join condition as a simple string (the next identifier expression)
        std::string join_cond;
        while (!check(TokenType::KW_JOIN) && !check(TokenType::KW_WHERE) &&
               !check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT)) {
            join_cond += current_.value + " ";
            advance();
        }
        gql.joins.emplace_back(join_table, join_cond);
    }

    if (check(TokenType::KW_WHERE)) {
        advance();
        gql.where_clause = parse_expr();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::GRAPHQL_QUERY;
    stmt->line = stmt_line;
    stmt->data = std::move(gql);
    return stmt;
}

// ═══════════════════════════════════════════════════════════════════════════
// v1.2.0: New statement parsers
// ═══════════════════════════════════════════════════════════════════════════

// ─── CREATE TRIGGER ───
// CREATE [OR REPLACE] TRIGGER name {BEFORE|AFTER} {INSERT|UPDATE|DELETE}
// ON table [FOR EACH ROW] [LANGUAGE lang] AS $$ body $$ | BEGIN body END
ast::StmtPtr Parser::parse_create_trigger(bool or_replace) {
    advance(); // TRIGGER
    uint32_t stmt_line = current_.line;
    ast::CreateTriggerStmt ct;
    ct.or_replace = or_replace;

    // IF NOT EXISTS
    if (check(TokenType::KW_IF)) {
        advance(); expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        ct.if_not_exists = true;
    }

    ct.name = expect_identifier("trigger name").value;

    // BEFORE | AFTER | INSTEAD OF
    if (match(TokenType::KW_BEFORE)) ct.timing = "BEFORE";
    else if (match(TokenType::KW_AFTER)) ct.timing = "AFTER";
    else if (match(TokenType::KW_INSTEAD)) {
        // Skip 'OF' if present (it may be an identifier or keyword)
        if (is_identifier_or_keyword() || check(TokenType::IDENTIFIER)) advance();
        ct.timing = "INSTEAD OF";
    } else {
        ct.timing = "AFTER"; // default
    }

    // INSERT | UPDATE | DELETE
    if (match(TokenType::KW_INSERT)) ct.event = "INSERT";
    else if (match(TokenType::KW_UPDATE)) ct.event = "UPDATE";
    else if (match(TokenType::KW_DELETE)) ct.event = "DELETE";
    else ct.event = "INSERT";

    expect(TokenType::KW_ON, "ON table_name");
    ct.table = expect_identifier("table name").value;

    // Optional FOR EACH ROW
    if (match(TokenType::KW_FOR)) {
        match(TokenType::KW_EACH);
        match(TokenType::KW_ROW);
        ct.for_each_row = true;
    }

    // Optional LANGUAGE
    ct.language = "plsql";
    if (check(TokenType::KW_LANGUAGE)) {
        advance();
        ct.language = current_.value;
        advance();
    }

    // Body: either AS $$ ... $$ or BEGIN ... END or AS 'source'
    if (match(TokenType::KW_AS)) {
        if (check(TokenType::STRING_LITERAL)) {
            ct.body = current_.value;
            advance();
        } else {
            // Consume everything until END; or semicolon
            std::string body;
            while (!check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT)) {
                body += current_.value + " ";
                advance();
            }
            ct.body = body;
        }
    } else if (match(TokenType::KW_BEGIN)) {
        // PL/SQL body
        std::string body;
        int depth = 1;
        while (depth > 0 && !check(TokenType::END_OF_INPUT)) {
            if (check(TokenType::KW_BEGIN)) depth++;
            if (check(TokenType::KW_END)) { depth--; if (depth == 0) break; }
            body += current_.value + " ";
            advance();
        }
        match(TokenType::KW_END);
        ct.body = body;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CREATE_TRIGGER;
    stmt->line = stmt_line;
    stmt->data = std::move(ct);
    return stmt;
}

// ─── CREATE PROCEDURE / FUNCTION ───
// CREATE [OR REPLACE] PROCEDURE|FUNCTION name (params) [RETURNS type]
// [LANGUAGE lang] AS $$ body $$ | BEGIN body END
ast::StmtPtr Parser::parse_create_procedure(bool or_replace, bool is_function) {
    advance(); // PROCEDURE or FUNCTION
    uint32_t stmt_line = current_.line;
    ast::CreateProcedureStmt cp;
    cp.or_replace = or_replace;
    cp.is_function = is_function;

    // IF NOT EXISTS
    if (check(TokenType::KW_IF)) {
        advance(); expect(TokenType::KW_NOT, "IF NOT EXISTS");
        expect(TokenType::KW_EXISTS_KW, "IF NOT EXISTS");
        cp.if_not_exists = true;
    }

    std::string nm = expect_identifier("procedure/function name").value;
    if (match(TokenType::DOT)) {
        cp.schema = nm;
        cp.name = expect_identifier("procedure/function name").value;
    } else {
        cp.name = nm;
    }

    // Parameters
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
            ast::PLParam p;
            // Optional IN/OUT/INOUT
            std::string upper_val = current_.value;
            for (auto &c : upper_val) c = (char)std::toupper((unsigned char)c);
            if (upper_val == "IN" || upper_val == "OUT" || upper_val == "INOUT") {
                p.mode = upper_val;
                advance();
            }
            p.name = expect_identifier("parameter name").value;
            p.type = parse_data_type();
            cp.params.push_back(std::move(p));
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, "parameter list");
    }

    // RETURNS type (for functions)
    if (is_function && (check(TokenType::KW_RETURNS) || check(TokenType::KW_RETURN))) {
        advance();
        cp.return_type = parse_data_type();
    }

    // DETERMINISTIC
    if (match(TokenType::KW_DETERMINISTIC)) cp.deterministic = true;

    // LANGUAGE
    cp.language = "plsql";
    if (check(TokenType::KW_LANGUAGE)) {
        advance();
        cp.language = current_.value;
        advance();
    }

    // Body
    if (match(TokenType::KW_AS)) {
        if (check(TokenType::STRING_LITERAL)) {
            cp.body = current_.value;
            advance();
        } else {
            // Consume until END or semicolon for dollar-quoted or block body
            std::string body;
            if (match(TokenType::KW_BEGIN)) {
                int depth = 1;
                while (depth > 0 && !check(TokenType::END_OF_INPUT)) {
                    if (check(TokenType::SEMICOLON)) { body += "; "; advance(); continue; }
                    if (check(TokenType::KW_BEGIN)) depth++;
                    if (check(TokenType::KW_END)) {
                        advance();
                        std::string nu = current_.value;
                        for (auto &ch : nu) ch = (char)std::toupper((unsigned char)ch);
                        if (nu == "IF" || nu == "LOOP") {
                            body += "END " + current_.value + "; ";
                            advance();
                            continue;
                        }
                        depth--;
                        if (depth == 0) break;
                        body += "END; ";
                        continue;
                    }
                    if (check(TokenType::STRING_LITERAL)) {
                        body += "'" + current_.value + "' ";
                    } else {
                        body += current_.value + " ";
                    }
                    advance();
                }
            } else {
                while (!check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT)) {
                    body += current_.value + " ";
                    advance();
                }
            }
            cp.body = body;
        }
    } else if (match(TokenType::KW_BEGIN)) {
        std::string body;
        int depth = 1;
        while (depth > 0 && !check(TokenType::END_OF_INPUT)) {
            // Preserve semicolons in PL/SQL body
            if (check(TokenType::SEMICOLON)) {
                body += "; ";
                advance();
                continue;
            }
            if (check(TokenType::KW_BEGIN)) depth++;
            if (check(TokenType::KW_END)) {
                advance(); // consume END
                std::string next_upper = current_.value;
                for (auto &ch : next_upper) ch = (char)std::toupper((unsigned char)ch);
                if (next_upper == "IF" || next_upper == "LOOP") {
                    body += "END " + current_.value + " ";
                    advance(); // consume IF/LOOP
                    continue;
                }
                // Standalone END — this closes a BEGIN block
                depth--;
                if (depth == 0) break;
                body += "END ";
                continue;
            }
            // Preserve string literals with quotes in body text
            if (check(TokenType::STRING_LITERAL)) {
                body += "'" + current_.value + "' ";
            } else {
                body += current_.value + " ";
            }
            advance();
        }
        cp.body = body;
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = is_function ? ast::StmtType::CREATE_FUNCTION : ast::StmtType::CREATE_PROCEDURE;
    stmt->line = stmt_line;
    stmt->data = std::move(cp);
    return stmt;
}

// ─── CALL procedure(args) ───
ast::StmtPtr Parser::parse_call() {
    advance(); // CALL
    uint32_t stmt_line = current_.line;
    ast::CallStmt cs;
    std::string nm = expect_identifier("procedure name").value;
    if (match(TokenType::DOT)) {
        cs.schema = nm;
        cs.name = expect_identifier("procedure name").value;
    } else {
        cs.name = nm;
    }
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
            cs.args.push_back(parse_expr());
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, "CALL arguments");
    }
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CALL_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(cs);
    return stmt;
}

// ─── GRANT / REVOKE ───
// GRANT priv[, ...] ON [object_type] object TO role [WITH GRANT OPTION]
// REVOKE priv[, ...] ON [object_type] object FROM role
ast::StmtPtr Parser::parse_grant(bool is_revoke) {
    advance(); // GRANT or REVOKE
    uint32_t stmt_line = current_.line;
    ast::GrantStmt gs;
    gs.is_revoke = is_revoke;

    // Privileges
    do {
        std::string priv = current_.value;
        for (auto &c : priv) c = (char)std::toupper((unsigned char)c);
        gs.privileges.push_back(priv);
        advance();
    } while (match(TokenType::COMMA));

    expect(TokenType::KW_ON, "ON");

    // Optional object type
    gs.object_type = "TABLE"; // default
    std::string upper = current_.value;
    for (auto &c : upper) c = (char)std::toupper((unsigned char)c);
    if (upper == "TABLE" || upper == "VIEW" || upper == "SEQUENCE" || upper == "SCHEMA" ||
        upper == "PROCEDURE" || upper == "FUNCTION") {
        gs.object_type = upper;
        advance();
    }

    gs.object_name = expect_identifier("object name").value;

    // TO / FROM
    if (is_revoke) {
        expect(TokenType::KW_FROM, "FROM role");
    } else {
        expect(TokenType::KW_TO, "TO role");
    }
    gs.grantee = expect_identifier("role/user name").value;

    // WITH GRANT OPTION
    if (!is_revoke && check(TokenType::KW_WITH)) {
        advance(); // WITH
        if (match(TokenType::KW_GRANT)) {
            match(TokenType::KW_OPTION);
            gs.with_grant_option = true;
        }
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = is_revoke ? ast::StmtType::REVOKE_STMT : ast::StmtType::GRANT_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(gs);
    return stmt;
}

// ─── PREPARE name AS sql ───
ast::StmtPtr Parser::parse_prepare() {
    advance(); // PREPARE
    uint32_t stmt_line = current_.line;
    ast::PrepareStmt ps;
    ps.name = expect_identifier("prepared statement name").value;
    expect(TokenType::KW_AS, "PREPARE name AS");

    // Capture the rest as SQL text
    std::string sql;
    while (!check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT)) {
        sql += current_.value + " ";
        advance();
    }
    ps.sql_text = sql;

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::PREPARE_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(ps);
    return stmt;
}

// ─── EXECUTE name [(params)] ───
ast::StmtPtr Parser::parse_execute() {
    advance(); // EXECUTE
    uint32_t stmt_line = current_.line;
    ast::ExecuteStmt es;
    es.name = expect_identifier("prepared statement name").value;
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !check(TokenType::END_OF_INPUT)) {
            es.params.push_back(parse_expr());
            if (!match(TokenType::COMMA)) break;
        }
        expect(TokenType::RPAREN, "EXECUTE params");
    }
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::EXECUTE_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(es);
    return stmt;
}

// ─── DEALLOCATE [ALL | name] ───
ast::StmtPtr Parser::parse_deallocate() {
    advance(); // DEALLOCATE
    uint32_t stmt_line = current_.line;
    ast::DeallocateStmt ds;
    if (check(TokenType::KW_ALL)) {
        advance();
        ds.all = true;
    } else if (check(TokenType::KW_PREPARE)) {
        advance(); // optional PREPARE keyword
        ds.name = expect_identifier("prepared statement name").value;
    } else {
        ds.name = expect_identifier("prepared statement name").value;
    }
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::DEALLOCATE_STMT;
    stmt->line = stmt_line;
    stmt->data = std::move(ds);
    return stmt;
}

// ─── DECLARE cursor_name [SCROLL] CURSOR [WITH HOLD] FOR select ───
ast::StmtPtr Parser::parse_declare_cursor() {
    advance(); // DECLARE
    uint32_t stmt_line = current_.line;
    ast::DeclareCursorStmt dc;
    dc.name = expect_identifier("cursor name").value;

    if (match(TokenType::KW_SCROLL)) dc.scroll = true;
    if (match(TokenType::KW_INSENSITIVE)) dc.insensitive = true;

    expect(TokenType::KW_CURSOR, "CURSOR");

    if (check(TokenType::KW_WITH)) {
        advance();
        if (match(TokenType::KW_HOLD)) dc.hold = true;
    }

    expect(TokenType::KW_FOR, "FOR select");
    auto sel = parse_select_stmt();
    dc.query = std::make_unique<ast::SelectStmt>(std::move(std::get<ast::SelectStmt>(sel->data)));

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::DECLARE_CURSOR;
    stmt->line = stmt_line;
    stmt->data = std::move(dc);
    return stmt;
}

// ─── OPEN cursor_name ───
ast::StmtPtr Parser::parse_open_cursor() {
    advance(); // OPEN
    uint32_t stmt_line = current_.line;
    ast::OpenCursorStmt oc;
    oc.name = expect_identifier("cursor name").value;
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::OPEN_CURSOR;
    stmt->line = stmt_line;
    stmt->data = std::move(oc);
    return stmt;
}

// ─── CLOSE cursor_name ───
ast::StmtPtr Parser::parse_close_cursor() {
    advance(); // CLOSE
    uint32_t stmt_line = current_.line;
    ast::CloseCursorStmt cc;
    cc.name = expect_identifier("cursor name").value;
    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::CLOSE_CURSOR;
    stmt->line = stmt_line;
    stmt->data = std::move(cc);
    return stmt;
}

// ─── FETCH [direction] [count] FROM cursor_name ───
ast::StmtPtr Parser::parse_fetch_cursor() {
    advance(); // FETCH
    uint32_t stmt_line = current_.line;
    ast::FetchCursorStmt fc;
    fc.direction = ast::FetchCursorStmt::Direction::NEXT;
    fc.count = 1;

    // Optional direction
    std::string dir = current_.value;
    for (auto &c : dir) c = (char)std::toupper((unsigned char)c);
    if (dir == "NEXT") { advance(); fc.direction = ast::FetchCursorStmt::Direction::NEXT; }
    else if (dir == "PRIOR") { advance(); fc.direction = ast::FetchCursorStmt::Direction::PRIOR; }
    else if (dir == "FIRST") { advance(); fc.direction = ast::FetchCursorStmt::Direction::FIRST; }
    else if (dir == "LAST") { advance(); fc.direction = ast::FetchCursorStmt::Direction::LAST; }
    else if (dir == "FORWARD") {
        advance(); fc.direction = ast::FetchCursorStmt::Direction::FORWARD;
        if (check(TokenType::INTEGER_LITERAL)) { fc.count = std::stoll(current_.value); advance(); }
    }
    else if (dir == "BACKWARD") {
        advance(); fc.direction = ast::FetchCursorStmt::Direction::BACKWARD;
        if (check(TokenType::INTEGER_LITERAL)) { fc.count = std::stoll(current_.value); advance(); }
    }

    // FROM cursor_name
    if (match(TokenType::KW_FROM)) {
        // explicit FROM
    } else if (match(TokenType::KW_IN)) {
        // PostgreSQL also accepts IN
    }
    fc.name = expect_identifier("cursor name").value;

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::FETCH_CURSOR;
    stmt->line = stmt_line;
    stmt->data = std::move(fc);
    return stmt;
}

// ─── MATCH graph pattern query (SQL/PGQ-inspired) ───
// MATCH (a:Table)-[r:Edge]->(b:Table) [WHERE ...] RETURN expr [, ...] [ORDER BY ...] [LIMIT n]
ast::StmtPtr Parser::parse_graph_match() {
    advance(); // MATCH
    uint32_t stmt_line = current_.line;
    ast::GraphMatchStmt gm;

    // Parse pattern: (alias:Label)-[alias:Label]->(alias:Label)
    // We parse a chain of nodes and edges
    int node_idx = 0;
    while (!check(TokenType::END_OF_INPUT)) {
        // Node: (alias:Label) or (alias)
        if (match(TokenType::LPAREN)) {
            ast::GraphNode node;
            node.alias = expect_identifier("node alias or label").value;
            if (match(TokenType::COLON)) {
                node.label = expect_identifier("node label").value;
            } else {
                node.label = node.alias; // alias IS the label
            }
            expect(TokenType::RPAREN, "node pattern");
            gm.nodes.push_back(std::move(node));
            node_idx = static_cast<int>(gm.nodes.size()) - 1;
        }

        // Edge: -[alias:Label]-> or <-[alias:Label]- or -[alias:Label]->
        if (check(TokenType::MINUS) || check(TokenType::LT) || check(TokenType::ARROW)) {
            ast::GraphEdge edge;
            bool left_arrow = false;
            bool right_arrow = false;

            if (match(TokenType::ARROW)) {
                // -> without bracket (simple directed edge)
                right_arrow = true;
            } else {
                left_arrow = match(TokenType::LT); // <-
                if (!left_arrow) match(TokenType::MINUS); // -

                if (match(TokenType::LBRACKET)) {
                    edge.alias = expect_identifier("edge alias or label").value;
                    if (match(TokenType::COLON)) {
                        edge.label = expect_identifier("edge label").value;
                    } else {
                        edge.label = edge.alias;
                    }
                    expect(TokenType::RBRACKET, "edge pattern");
                }

                // -> or - (end of edge)
                if (match(TokenType::ARROW)) {
                    right_arrow = true;
                } else {
                    match(TokenType::MINUS); // consume trailing -
                    if (match(TokenType::GT)) right_arrow = true;
                }
            }

            edge.from_node = node_idx;
            edge.to_node = node_idx + 1;
            edge.directed = left_arrow || right_arrow;
            if (left_arrow && !right_arrow) {
                std::swap(edge.from_node, edge.to_node);
            }
            gm.edges.push_back(std::move(edge));
            continue;
        }

        break; // No more pattern elements
    }

    // WHERE clause
    if (check(TokenType::KW_WHERE)) {
        advance();
        gm.where_clause = parse_expr();
    }

    // RETURN clause (required)
    if (check(TokenType::KW_RETURN)) {
        advance();
    } else {
        // Also accept SELECT-style
        if (check(TokenType::KW_SELECT)) advance();
    }

    // Parse return expressions
    if (!check(TokenType::SEMICOLON) && !check(TokenType::END_OF_INPUT) &&
        !check(TokenType::KW_ORDER) && !check(TokenType::KW_LIMIT)) {
        do {
            auto expr = parse_expr();
            std::string alias;
            if (match(TokenType::KW_AS)) {
                alias = expect_identifier("alias").value;
            } else if (expr->type == ast::ExprType::COLUMN_REF) {
                alias = std::get<ast::ColumnRef>(expr->data).column;
            } else {
                alias = "?column?";
            }
            gm.return_aliases.push_back(alias);
            gm.return_list.push_back(std::move(expr));
        } while (match(TokenType::COMMA));
    }

    // ORDER BY
    if (check(TokenType::KW_ORDER)) {
        advance(); expect(TokenType::KW_BY, "ORDER BY");
        do {
            auto expr = parse_expr();
            bool asc = true;
            if (match(TokenType::KW_DESC)) asc = false;
            else match(TokenType::KW_ASC);
            ast::OrderByItem obi;
            obi.expr = std::move(expr);
            obi.ascending = asc;
            gm.order_by.push_back(std::move(obi));
        } while (match(TokenType::COMMA));
    }

    // LIMIT
    if (match(TokenType::KW_LIMIT)) {
        gm.limit = parse_expr();
    }

    auto stmt = std::make_unique<ast::Statement>();
    stmt->type = ast::StmtType::GRAPH_MATCH;
    stmt->line = stmt_line;
    stmt->data = std::move(gm);
    return stmt;
}

} // namespace tdb::sql
