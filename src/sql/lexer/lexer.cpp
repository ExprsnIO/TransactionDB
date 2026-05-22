#include "tdb/sql/lexer.h"
#include <cctype>
#include <algorithm>

namespace tdb::sql {

const std::unordered_map<std::string, TokenType> Lexer::keywords_ = {
    // ─── DDL ───
    {"CREATE", TokenType::KW_CREATE}, {"ALTER", TokenType::KW_ALTER},
    {"DROP", TokenType::KW_DROP}, {"TRUNCATE", TokenType::KW_TRUNCATE},
    {"TABLE", TokenType::KW_TABLE}, {"INDEX", TokenType::KW_INDEX},
    {"VIEW", TokenType::KW_VIEW}, {"SEQUENCE", TokenType::KW_SEQUENCE},
    {"SCHEMA", TokenType::KW_SCHEMA}, {"DATABASE", TokenType::KW_DATABASE},
    {"TRIGGER", TokenType::KW_TRIGGER},
    {"COLUMN", TokenType::KW_COLUMN}, {"CONSTRAINT", TokenType::KW_CONSTRAINT},
    {"PRIMARY", TokenType::KW_PRIMARY}, {"FOREIGN", TokenType::KW_FOREIGN},
    {"KEY", TokenType::KW_KEY}, {"REFERENCES", TokenType::KW_REFERENCES},
    {"UNIQUE", TokenType::KW_UNIQUE}, {"CHECK", TokenType::KW_CHECK},
    {"DEFAULT", TokenType::KW_DEFAULT},
    {"NOT", TokenType::KW_NOT}, {"NULL", TokenType::KW_NULL_KW},
    {"AUTO_INCREMENT", TokenType::KW_AUTO_INCREMENT},
    {"IF", TokenType::KW_IF}, {"EXISTS", TokenType::KW_EXISTS_KW},
    {"CASCADE", TokenType::KW_CASCADE}, {"RESTRICT", TokenType::KW_RESTRICT},
    {"SET", TokenType::KW_SET},
    {"ADD", TokenType::KW_ADD}, {"RENAME", TokenType::KW_RENAME}, {"TO", TokenType::KW_TO},
    {"TEMPORARY", TokenType::KW_TEMPORARY}, {"TEMP", TokenType::KW_TEMP},
    {"USING", TokenType::KW_USING},
    {"ON", TokenType::KW_ON}, {"REPLACE", TokenType::KW_REPLACE},
    {"FUNCTION", TokenType::KW_FUNCTION}, {"PROCEDURE", TokenType::KW_PROCEDURE},
    {"RETURNS", TokenType::KW_RETURNS}, {"RETURN", TokenType::KW_RETURN},
    {"LANGUAGE", TokenType::KW_LANGUAGE}, {"CALLED", TokenType::KW_CALLED},
    {"DETERMINISTIC", TokenType::KW_DETERMINISTIC},
    {"DOMAIN", TokenType::KW_DOMAIN}, {"TYPE", TokenType::KW_TYPE},
    {"ROLE", TokenType::KW_ROLE}, {"USER", TokenType::KW_USER},
    {"AUTHORIZATION", TokenType::KW_AUTHORIZATION},
    {"GENERATED", TokenType::KW_GENERATED}, {"ALWAYS", TokenType::KW_ALWAYS},
    {"IDENTITY", TokenType::KW_IDENTITY},
    {"DEFERRED", TokenType::KW_DEFERRED}, {"IMMEDIATE", TokenType::KW_IMMEDIATE},
    {"INITIALLY", TokenType::KW_INITIALLY}, {"DEFERRABLE", TokenType::KW_DEFERRABLE},
    {"NO", TokenType::KW_NO}, {"ACTION", TokenType::KW_ACTION},
    {"PRESERVE", TokenType::KW_PRESERVE},
    {"GLOBAL", TokenType::KW_GLOBAL}, {"LOCAL", TokenType::KW_LOCAL},
    {"COMMENT", TokenType::KW_COMMENT},

    // ─── DML ───
    {"SELECT", TokenType::KW_SELECT}, {"INSERT", TokenType::KW_INSERT},
    {"UPDATE", TokenType::KW_UPDATE}, {"DELETE", TokenType::KW_DELETE},
    {"MERGE", TokenType::KW_MERGE},
    {"FROM", TokenType::KW_FROM}, {"WHERE", TokenType::KW_WHERE},
    {"GROUP", TokenType::KW_GROUP}, {"HAVING", TokenType::KW_HAVING},
    {"ORDER", TokenType::KW_ORDER},
    {"BY", TokenType::KW_BY}, {"ASC", TokenType::KW_ASC}, {"DESC", TokenType::KW_DESC},
    {"NULLS", TokenType::KW_NULLS}, {"FIRST", TokenType::KW_FIRST}, {"LAST", TokenType::KW_LAST},
    {"LIMIT", TokenType::KW_LIMIT}, {"OFFSET", TokenType::KW_OFFSET},
    {"FETCH", TokenType::KW_FETCH}, {"NEXT", TokenType::KW_NEXT},
    {"ROWS", TokenType::KW_ROWS}, {"ONLY", TokenType::KW_ONLY},
    {"INTO", TokenType::KW_INTO}, {"VALUES", TokenType::KW_VALUES},
    {"AS", TokenType::KW_AS}, {"ALL", TokenType::KW_ALL}, {"DISTINCT", TokenType::KW_DISTINCT},
    {"UNION", TokenType::KW_UNION}, {"INTERSECT", TokenType::KW_INTERSECT},
    {"EXCEPT", TokenType::KW_EXCEPT},
    {"JOIN", TokenType::KW_JOIN}, {"INNER", TokenType::KW_INNER},
    {"LEFT", TokenType::KW_LEFT}, {"RIGHT", TokenType::KW_RIGHT},
    {"FULL", TokenType::KW_FULL}, {"OUTER", TokenType::KW_OUTER},
    {"CROSS", TokenType::KW_CROSS}, {"NATURAL", TokenType::KW_NATURAL},
    {"WITH", TokenType::KW_WITH}, {"RECURSIVE", TokenType::KW_RECURSIVE},
    {"CASE", TokenType::KW_CASE}, {"WHEN", TokenType::KW_WHEN},
    {"THEN", TokenType::KW_THEN}, {"ELSE", TokenType::KW_ELSE},
    {"END", TokenType::KW_END},
    {"MATCHED", TokenType::KW_MATCHED}, {"TARGET", TokenType::KW_TARGET},
    {"SOURCE", TokenType::KW_SOURCE},
    {"RETURNING", TokenType::KW_RETURNING},
    {"CONFLICT", TokenType::KW_CONFLICT}, {"NOTHING", TokenType::KW_NOTHING},
    {"DO", TokenType::KW_DO},
    {"TIES", TokenType::KW_TIES},
    {"TABLESAMPLE", TokenType::KW_TABLESAMPLE},
    {"BERNOULLI", TokenType::KW_BERNOULLI},
    {"FOR", TokenType::KW_FOR},
    {"SHARE", TokenType::KW_SHARE},
    {"NOWAIT", TokenType::KW_NOWAIT},
    {"LOCK", TokenType::KW_LOCK_KW},
    {"LOCKED", TokenType::KW_LOCKED},
    {"EXCLUSIVE", TokenType::KW_EXCLUSIVE_KW},
    {"CORRESPONDING", TokenType::KW_CORRESPONDING},
    {"MATERIALIZED", TokenType::KW_MATERIALIZED},
    {"CALL", TokenType::KW_CALL},

    // ─── Logical / Predicates ───
    {"AND", TokenType::KW_AND}, {"OR", TokenType::KW_OR},
    {"IN", TokenType::KW_IN}, {"BETWEEN", TokenType::KW_BETWEEN},
    {"LIKE", TokenType::KW_LIKE}, {"ILIKE", TokenType::KW_ILIKE},
    {"IS", TokenType::KW_IS}, {"ANY", TokenType::KW_ANY},
    {"SOME", TokenType::KW_SOME}, {"EVERY", TokenType::KW_EVERY},
    {"SIMILAR", TokenType::KW_SIMILAR},
    {"OVERLAPS", TokenType::KW_OVERLAPS},
    {"UNKNOWN", TokenType::KW_UNKNOWN},
    {"SYMMETRIC", TokenType::KW_SYMMETRIC},
    {"ASYMMETRIC", TokenType::KW_ASYMMETRIC},

    // ─── Aggregate / Window ───
    {"COUNT", TokenType::KW_COUNT}, {"SUM", TokenType::KW_SUM},
    {"AVG", TokenType::KW_AVG}, {"MIN", TokenType::KW_MIN}, {"MAX", TokenType::KW_MAX},
    {"OVER", TokenType::KW_OVER}, {"PARTITION", TokenType::KW_PARTITION},
    {"WINDOW", TokenType::KW_WINDOW},
    {"RANGE", TokenType::KW_RANGE}, {"ROW", TokenType::KW_ROW},
    {"UNBOUNDED", TokenType::KW_UNBOUNDED},
    {"PRECEDING", TokenType::KW_PRECEDING}, {"FOLLOWING", TokenType::KW_FOLLOWING},
    {"CURRENT", TokenType::KW_CURRENT},
    {"GROUPS", TokenType::KW_GROUPS},
    {"EXCLUDE", TokenType::KW_EXCLUDE}, {"OTHERS", TokenType::KW_OTHERS},
    {"FILTER", TokenType::KW_FILTER},
    {"WITHIN", TokenType::KW_WITHIN},
    {"GROUPING", TokenType::KW_GROUPING}, {"SETS", TokenType::KW_SETS},
    {"CUBE", TokenType::KW_CUBE}, {"ROLLUP", TokenType::KW_ROLLUP},
    {"ROW_NUMBER", TokenType::KW_ROW_NUMBER}, {"RANK", TokenType::KW_RANK},
    {"DENSE_RANK", TokenType::KW_DENSE_RANK}, {"NTILE", TokenType::KW_NTILE},
    {"LAG", TokenType::KW_LAG}, {"LEAD", TokenType::KW_LEAD},
    {"FIRST_VALUE", TokenType::KW_FIRST_VALUE}, {"LAST_VALUE", TokenType::KW_LAST_VALUE},
    {"NTH_VALUE", TokenType::KW_NTH_VALUE},
    {"PERCENT_RANK", TokenType::KW_PERCENT_RANK}, {"CUME_DIST", TokenType::KW_CUME_DIST},
    {"LISTAGG", TokenType::KW_LISTAGG},
    {"PERCENTILE_CONT", TokenType::KW_PERCENTILE_CONT},
    {"PERCENTILE_DISC", TokenType::KW_PERCENTILE_DISC},
    {"MODE", TokenType::KW_MODE},

    // ─── Types ───
    {"INT", TokenType::KW_INT}, {"INTEGER", TokenType::KW_INTEGER},
    {"SMALLINT", TokenType::KW_SMALLINT}, {"BIGINT", TokenType::KW_BIGINT},
    {"TINYINT", TokenType::KW_TINYINT},
    {"FLOAT", TokenType::KW_FLOAT}, {"DOUBLE", TokenType::KW_DOUBLE},
    {"REAL", TokenType::KW_REAL}, {"DECIMAL", TokenType::KW_DECIMAL},
    {"NUMERIC", TokenType::KW_NUMERIC},
    {"PRECISION", TokenType::KW_PRECISION},
    {"CHAR", TokenType::KW_CHAR}, {"VARCHAR", TokenType::KW_VARCHAR},
    {"TEXT", TokenType::KW_TEXT},
    {"CHARACTER", TokenType::KW_CHARACTER}, {"VARYING", TokenType::KW_VARYING},
    {"NATIONAL", TokenType::KW_NATIONAL}, {"NCHAR", TokenType::KW_NCHAR},
    {"NVARCHAR", TokenType::KW_NVARCHAR}, {"NCLOB", TokenType::KW_NCLOB},
    {"BOOLEAN", TokenType::KW_BOOLEAN}, {"BOOL", TokenType::KW_BOOL},
    {"DATE", TokenType::KW_DATE}, {"TIME", TokenType::KW_TIME},
    {"TIMESTAMP", TokenType::KW_TIMESTAMP}, {"INTERVAL", TokenType::KW_INTERVAL},
    {"BLOB", TokenType::KW_BLOB}, {"CLOB", TokenType::KW_CLOB},
    {"BINARY", TokenType::KW_BINARY}, {"VARBINARY", TokenType::KW_VARBINARY},
    {"BIT", TokenType::KW_BIT},
    {"JSON", TokenType::KW_JSON}, {"JSONB", TokenType::KW_JSONB},
    {"XML", TokenType::KW_XML},
    {"ARRAY", TokenType::KW_ARRAY},
    {"MULTISET", TokenType::KW_MULTISET},
    {"GEOMETRY", TokenType::KW_GEOMETRY}, {"GEOGRAPHY", TokenType::KW_GEOGRAPHY},
    {"RASTER", TokenType::KW_RASTER},
    {"SERIAL", TokenType::KW_SERIAL}, {"BIGSERIAL", TokenType::KW_BIGSERIAL},
    {"UUID", TokenType::KW_UUID}, {"ENUM", TokenType::KW_ENUM},
    {"MONEY", TokenType::KW_MONEY},

    // ─── Date/Time ───
    {"ZONE", TokenType::KW_ZONE}, {"WITHOUT", TokenType::KW_WITHOUT},
    {"EXTRACT", TokenType::KW_EXTRACT},
    {"YEAR", TokenType::KW_YEAR}, {"MONTH", TokenType::KW_MONTH},
    {"DAY", TokenType::KW_DAY},
    {"HOUR", TokenType::KW_HOUR}, {"MINUTE", TokenType::KW_MINUTE},
    {"SECOND", TokenType::KW_SECOND},
    {"EPOCH", TokenType::KW_EPOCH},
    {"WEEK", TokenType::KW_WEEK}, {"QUARTER", TokenType::KW_QUARTER},
    {"DOW", TokenType::KW_DOW}, {"DOY", TokenType::KW_DOY},
    {"CURRENT_DATE", TokenType::KW_CURRENT_DATE},
    {"CURRENT_TIME", TokenType::KW_CURRENT_TIME},
    {"CURRENT_TIMESTAMP", TokenType::KW_CURRENT_TIMESTAMP},
    {"LOCALTIME", TokenType::KW_LOCALTIME},
    {"LOCALTIMESTAMP", TokenType::KW_LOCALTIMESTAMP},
    {"AT", TokenType::KW_AT_KW},

    // ─── String functions ───
    {"SUBSTRING", TokenType::KW_SUBSTRING},
    {"TRIM", TokenType::KW_TRIM},
    {"LEADING", TokenType::KW_LEADING}, {"TRAILING", TokenType::KW_TRAILING},
    {"BOTH", TokenType::KW_BOTH},
    {"POSITION", TokenType::KW_POSITION},
    {"OVERLAY", TokenType::KW_OVERLAY}, {"PLACING", TokenType::KW_PLACING},
    {"UPPER", TokenType::KW_UPPER}, {"LOWER", TokenType::KW_LOWER},
    {"NORMALIZE", TokenType::KW_NORMALIZE},
    {"TRANSLATE", TokenType::KW_TRANSLATE}, {"CONVERT", TokenType::KW_CONVERT},
    {"COALESCE", TokenType::KW_COALESCE}, {"NULLIF", TokenType::KW_NULLIF},
    {"GREATEST", TokenType::KW_GREATEST}, {"LEAST", TokenType::KW_LEAST},

    // ─── Transaction ───
    {"BEGIN", TokenType::KW_BEGIN}, {"COMMIT", TokenType::KW_COMMIT},
    {"ROLLBACK", TokenType::KW_ROLLBACK}, {"SAVEPOINT", TokenType::KW_SAVEPOINT},
    {"RELEASE", TokenType::KW_RELEASE},
    {"TRANSACTION", TokenType::KW_TRANSACTION}, {"ISOLATION", TokenType::KW_ISOLATION},
    {"LEVEL", TokenType::KW_LEVEL},
    {"READ", TokenType::KW_READ}, {"WRITE", TokenType::KW_WRITE},
    {"COMMITTED", TokenType::KW_COMMITTED}, {"UNCOMMITTED", TokenType::KW_UNCOMMITTED},
    {"REPEATABLE", TokenType::KW_REPEATABLE}, {"SERIALIZABLE", TokenType::KW_SERIALIZABLE},
    {"SNAPSHOT", TokenType::KW_SNAPSHOT},
    {"START", TokenType::KW_START}, {"WORK", TokenType::KW_WORK},
    {"CONSTRAINTS", TokenType::KW_CONSTRAINTS},

    // ─── Subquery ───
    {"LATERAL", TokenType::KW_LATERAL},
    {"UNNEST", TokenType::KW_UNNEST},

    // ─── Access Control ───
    {"GRANT", TokenType::KW_GRANT}, {"REVOKE", TokenType::KW_REVOKE},
    {"PRIVILEGES", TokenType::KW_PRIVILEGES},
    {"EXECUTE", TokenType::KW_EXECUTE}, {"USAGE", TokenType::KW_USAGE},
    {"OPTION", TokenType::KW_OPTION}, {"PUBLIC", TokenType::KW_PUBLIC},

    // ─── Misc ───
    {"EXPLAIN", TokenType::KW_EXPLAIN}, {"ANALYZE", TokenType::KW_ANALYZE},
    {"VACUUM", TokenType::KW_VACUUM},
    {"CAST", TokenType::KW_CAST}, {"COLLATE", TokenType::KW_COLLATE},
    {"ESCAPE", TokenType::KW_ESCAPE},
    {"TRUE", TokenType::KW_TRUE}, {"FALSE", TokenType::KW_FALSE},
    {"SHOW", TokenType::KW_SHOW}, {"DESCRIBE", TokenType::KW_DESCRIBE},
    {"PREPARE", TokenType::KW_PREPARE}, {"DEALLOCATE", TokenType::KW_DEALLOCATE},
    {"DECLARE", TokenType::KW_DECLARE}, {"CURSOR", TokenType::KW_CURSOR},
    {"OPEN", TokenType::KW_OPEN}, {"CLOSE", TokenType::KW_CLOSE},
    {"SCROLL", TokenType::KW_SCROLL}, {"INSENSITIVE", TokenType::KW_INSENSITIVE},
    {"SENSITIVE", TokenType::KW_SENSITIVE},
    {"HOLD", TokenType::KW_HOLD},
    {"VERBOSE", TokenType::KW_VERBOSE}, {"FORMAT", TokenType::KW_FORMAT},
    {"COPY", TokenType::KW_COPY},

    // ─── Encryption ───
    {"ENCRYPT", TokenType::KW_ENCRYPT}, {"ENCRYPTED", TokenType::KW_ENCRYPTED},
    {"DECRYPT", TokenType::KW_DECRYPT},

    // ─── Trigger ───
    {"BEFORE", TokenType::KW_BEFORE}, {"AFTER", TokenType::KW_AFTER},
    {"INSTEAD", TokenType::KW_INSTEAD},
    {"EACH", TokenType::KW_EACH},
    {"NEW", TokenType::KW_NEW}, {"OLD", TokenType::KW_OLD},
    {"REFERENCING", TokenType::KW_REFERENCING},
    {"STATEMENT", TokenType::KW_STATEMENT},

    // ─── PSM ───
    {"ATOMIC", TokenType::KW_ATOMIC},
    {"ELSEIF", TokenType::KW_ELSEIF}, {"WHILE", TokenType::KW_WHILE},
    {"LOOP", TokenType::KW_LOOP}, {"REPEAT", TokenType::KW_REPEAT},
    {"UNTIL", TokenType::KW_UNTIL}, {"LEAVE", TokenType::KW_LEAVE},
    {"ITERATE", TokenType::KW_ITERATE},
    {"SIGNAL", TokenType::KW_SIGNAL}, {"RESIGNAL", TokenType::KW_RESIGNAL},
    {"HANDLER", TokenType::KW_HANDLER}, {"CONDITION", TokenType::KW_CONDITION},
    {"SQLSTATE", TokenType::KW_SQLSTATE},
    {"SQLEXCEPTION", TokenType::KW_SQLEXCEPTION},
    {"SQLWARNING", TokenType::KW_SQLWARNING},
    {"INOUT", TokenType::KW_INOUT}, {"OUT", TokenType::KW_OUT},

    // ─── JSON (SQL:2016) ───
    {"JSON_OBJECT", TokenType::KW_JSON_OBJECT}, {"JSON_ARRAY", TokenType::KW_JSON_ARRAY},
    {"JSON_VALUE", TokenType::KW_JSON_VALUE}, {"JSON_QUERY", TokenType::KW_JSON_QUERY},
    {"JSON_TABLE", TokenType::KW_JSON_TABLE}, {"JSON_EXISTS", TokenType::KW_JSON_EXISTS},

    // ─── XML (SQL:2003) ───
    {"XMLELEMENT", TokenType::KW_XMLELEMENT}, {"XMLFOREST", TokenType::KW_XMLFOREST},
    {"XMLAGG", TokenType::KW_XMLAGG}, {"XMLPARSE", TokenType::KW_XMLPARSE},
    {"XMLSERIALIZE", TokenType::KW_XMLSERIALIZE},
    {"XMLNAMESPACES", TokenType::KW_XMLNAMESPACES},

    // ─── Views / Materialized Views ───
    {"REFRESH", TokenType::KW_REFRESH},
    {"CONCURRENTLY", TokenType::KW_CONCURRENTLY},
    {"WRITABLE", TokenType::KW_WRITABLE},

    // ─── Saved Queries ───
    {"SAVE", TokenType::KW_SAVE}, {"SAVED", TokenType::KW_SAVED},
    {"QUERY", TokenType::KW_QUERY},

    // ─── Tablespaces ───
    {"TABLESPACE", TokenType::KW_TABLESPACE},
    {"LOCATION", TokenType::KW_LOCATION},
    {"OWNER", TokenType::KW_OWNER},
    {"MOVE", TokenType::KW_MOVE},

    // ─── Logging ───
    {"LOG", TokenType::KW_LOG}, {"LOGGING", TokenType::KW_LOGGING},
    {"NOLOGGING", TokenType::KW_NOLOGGING},
    {"AUDIT", TokenType::KW_AUDIT}, {"NOAUDIT", TokenType::KW_NOAUDIT},

    // ─── Document Query ───
    {"XPATH", TokenType::KW_XPATH}, {"XQUERY", TokenType::KW_XQUERY},
    {"GRAPHQL", TokenType::KW_GRAPHQL},
    {"DOCUMENT", TokenType::KW_DOCUMENT}, {"CONTENT", TokenType::KW_CONTENT},
    {"PATH", TokenType::KW_PATH},
    {"PASSING", TokenType::KW_PASSING},

    // ─── Locking ───
    {"OBJECT", TokenType::KW_OBJECT},
    {"ADVISORY", TokenType::KW_ADVISORY},
    {"INTENT", TokenType::KW_INTENT},
    {"ACCESS", TokenType::KW_ACCESS},
    {"WAIT", TokenType::KW_WAIT},

    // ─── Partitioning ───
    {"HASH", TokenType::KW_HASH},
    {"LIST", TokenType::KW_LIST},
    {"LESS", TokenType::KW_LESS},
    {"THAN", TokenType::KW_THAN},
    {"MODULUS", TokenType::KW_MODULUS},
    {"REMAINDER", TokenType::KW_REMAINDER},
    {"MAXVALUE", TokenType::KW_MAXVALUE},
    {"MINVALUE", TokenType::KW_MINVALUE},

    // ─── Columnar / Federation ───
    {"COLUMNAR", TokenType::KW_COLUMNAR},
    {"MATCH", TokenType::KW_MATCH},
    {"GRAPH", TokenType::KW_GRAPH},
    {"VERTEX", TokenType::KW_VERTEX},
    {"EDGE", TokenType::KW_EDGE},
    {"EXTERNAL", TokenType::KW_EXTERNAL_KW},
    {"SERVER", TokenType::KW_SERVER},
    {"WRAPPER", TokenType::KW_WRAPPER},
    {"OPTIONS", TokenType::KW_OPTIONS},
    {"MAPPING", TokenType::KW_MAPPING},
};

Lexer::Lexer(const std::string &input)
    : input_(input), pos_(0), line_(1), col_(1) {}

char Lexer::peek() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char Lexer::peek_ahead(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= input_.size()) return '\0';
    return input_[idx];
}

char Lexer::advance() {
    char c = input_[pos_++];
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

void Lexer::skip_whitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(peek()))) {
        advance();
    }
}

void Lexer::skip_line_comment() {
    while (pos_ < input_.size() && peek() != '\n') {
        advance();
    }
}

void Lexer::skip_block_comment() {
    advance(); // '/'
    advance(); // '*'
    while (pos_ < input_.size()) {
        if (peek() == '*' && peek_ahead(1) == '/') {
            advance();
            advance();
            return;
        }
        advance();
    }
}

Token Lexer::read_number() {
    uint32_t start_line = line_, start_col = col_;
    std::string num;
    bool is_float = false;

    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
        num += advance();
    }

    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_ahead(1)))) {
        is_float = true;
        num += advance(); // '.'
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
            num += advance();
        }
    }

    if (peek() == 'e' || peek() == 'E') {
        is_float = true;
        num += advance();
        if (peek() == '+' || peek() == '-') {
            num += advance();
        }
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
            num += advance();
        }
    }

    return Token(is_float ? TokenType::FLOAT_LITERAL : TokenType::INTEGER_LITERAL,
                 num, start_line, start_col);
}

Token Lexer::read_string() {
    uint32_t start_line = line_, start_col = col_;
    advance(); // opening quote
    std::string str;

    while (pos_ < input_.size()) {
        char c = peek();
        if (c == '\'') {
            advance();
            if (peek() == '\'') {
                str += '\'';
                advance();
            } else {
                return Token(TokenType::STRING_LITERAL, str, start_line, start_col);
            }
        } else {
            str += advance();
        }
    }

    return Token(TokenType::INVALID, "unterminated string", start_line, start_col);
}

Token Lexer::read_quoted_identifier() {
    uint32_t start_line = line_, start_col = col_;
    advance(); // opening "
    std::string id;

    while (pos_ < input_.size()) {
        char c = peek();
        if (c == '"') {
            advance();
            if (peek() == '"') {
                id += '"';
                advance();
            } else {
                return Token(TokenType::QUOTED_IDENTIFIER, id, start_line, start_col);
            }
        } else {
            id += advance();
        }
    }

    return Token(TokenType::INVALID, "unterminated identifier", start_line, start_col);
}

Token Lexer::read_identifier_or_keyword() {
    uint32_t start_line = line_, start_col = col_;
    std::string id;

    while (pos_ < input_.size() &&
           (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        id += advance();
    }

    // Case-insensitive keyword lookup
    std::string upper = id;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    auto it = keywords_.find(upper);
    if (it != keywords_.end()) {
        // TRUE/FALSE are boolean literals
        if (it->second == TokenType::KW_TRUE || it->second == TokenType::KW_FALSE) {
            return Token(TokenType::BOOLEAN_LITERAL, upper, start_line, start_col);
        }
        return Token(it->second, upper, start_line, start_col);
    }

    return Token(TokenType::IDENTIFIER, id, start_line, start_col);
}

Token Lexer::read_blob_literal() {
    uint32_t start_line = line_, start_col = col_;
    advance(); // 'X' or 'x'
    if (peek() != '\'') {
        return Token(TokenType::IDENTIFIER, "X", start_line, start_col);
    }
    advance(); // opening quote
    std::string hex;
    while (pos_ < input_.size() && peek() != '\'') {
        hex += advance();
    }
    if (peek() == '\'') {
        advance();
        return Token(TokenType::BLOB_LITERAL, hex, start_line, start_col);
    }
    return Token(TokenType::INVALID, "unterminated blob literal", start_line, start_col);
}

Token Lexer::next_token() {
    while (pos_ < input_.size()) {
        skip_whitespace();
        if (pos_ >= input_.size()) break;

        uint32_t start_line = line_, start_col = col_;
        char c = peek();

        // Comments
        if (c == '-' && peek_ahead(1) == '-') {
            skip_line_comment();
            continue;
        }
        if (c == '/' && peek_ahead(1) == '*') {
            skip_block_comment();
            continue;
        }

        // Numbers
        if (std::isdigit(static_cast<unsigned char>(c))) {
            return read_number();
        }

        // Strings
        if (c == '\'') return read_string();

        // Quoted identifiers
        if (c == '"') return read_quoted_identifier();

        // Blob literals: X'...'
        if ((c == 'X' || c == 'x') && peek_ahead(1) == '\'') {
            return read_blob_literal();
        }

        // Identifiers / keywords
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            return read_identifier_or_keyword();
        }

        // Operators and punctuation
        advance();
        switch (c) {
            case '+': return Token(TokenType::PLUS, "+", start_line, start_col);
            case '*': return Token(TokenType::STAR, "*", start_line, start_col);
            case '/': return Token(TokenType::SLASH, "/", start_line, start_col);
            case '%': return Token(TokenType::PERCENT, "%", start_line, start_col);
            case '(': return Token(TokenType::LPAREN, "(", start_line, start_col);
            case ')': return Token(TokenType::RPAREN, ")", start_line, start_col);
            case '[': return Token(TokenType::LBRACKET, "[", start_line, start_col);
            case ']': return Token(TokenType::RBRACKET, "]", start_line, start_col);
            case ',': return Token(TokenType::COMMA, ",", start_line, start_col);
            case ';': return Token(TokenType::SEMICOLON, ";", start_line, start_col);
            case '.': return Token(TokenType::DOT, ".", start_line, start_col);
            case '?': return Token(TokenType::QUESTION_MARK, "?", start_line, start_col);
            case '@': return Token(TokenType::AT_SIGN, "@", start_line, start_col);
            case '~': return Token(TokenType::TILDE, "~", start_line, start_col);
            case '^': return Token(TokenType::CARET, "^", start_line, start_col);
            case '&': return Token(TokenType::AMPERSAND, "&", start_line, start_col);
            case '=': return Token(TokenType::EQ, "=", start_line, start_col);
            case '-':
                if (peek() == '>') {
                    advance();
                    if (peek() == '>') { advance(); return Token(TokenType::DOUBLE_ARROW, "->>", start_line, start_col); }
                    return Token(TokenType::ARROW, "->", start_line, start_col);
                }
                return Token(TokenType::MINUS, "-", start_line, start_col);
            case '<':
                if (peek() == '=') { advance(); return Token(TokenType::LTE, "<=", start_line, start_col); }
                if (peek() == '>') { advance(); return Token(TokenType::NEQ, "<>", start_line, start_col); }
                if (peek() == '<') { advance(); return Token(TokenType::SHIFT_LEFT, "<<", start_line, start_col); }
                return Token(TokenType::LT, "<", start_line, start_col);
            case '>':
                if (peek() == '=') { advance(); return Token(TokenType::GTE, ">=", start_line, start_col); }
                if (peek() == '>') { advance(); return Token(TokenType::SHIFT_RIGHT, ">>", start_line, start_col); }
                return Token(TokenType::GT, ">", start_line, start_col);
            case '!':
                if (peek() == '=') { advance(); return Token(TokenType::NEQ, "!=", start_line, start_col); }
                return Token(TokenType::INVALID, "!", start_line, start_col);
            case '|':
                if (peek() == '|') { advance(); return Token(TokenType::CONCAT, "||", start_line, start_col); }
                return Token(TokenType::PIPE, "|", start_line, start_col);
            case ':':
                if (peek() == ':') { advance(); return Token(TokenType::DOUBLE_COLON, "::", start_line, start_col); }
                if (peek() == '=') { advance(); return Token(TokenType::COLON_EQ, ":=", start_line, start_col); }
                return Token(TokenType::COLON, ":", start_line, start_col);
            default:
                return Token(TokenType::INVALID, std::string(1, c), start_line, start_col);
        }
    }

    return Token(TokenType::END_OF_INPUT, "", line_, col_);
}

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> tokens;
    while (true) {
        Token t = next_token();
        tokens.push_back(t);
        if (t.type == TokenType::END_OF_INPUT || t.type == TokenType::INVALID) break;
    }
    return tokens;
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL: return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL";
        case TokenType::BLOB_LITERAL: return "BLOB_LITERAL";
        case TokenType::BOOLEAN_LITERAL: return "BOOLEAN_LITERAL";
        case TokenType::NULL_LITERAL: return "NULL_LITERAL";
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::QUOTED_IDENTIFIER: return "QUOTED_IDENTIFIER";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::EQ: return "EQ";
        case TokenType::NEQ: return "NEQ";
        case TokenType::LT: return "LT";
        case TokenType::GT: return "GT";
        case TokenType::LTE: return "LTE";
        case TokenType::GTE: return "GTE";
        case TokenType::CONCAT: return "CONCAT";
        case TokenType::DOT: return "DOT";
        case TokenType::COMMA: return "COMMA";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::COLON: return "COLON";
        case TokenType::DOUBLE_COLON: return "DOUBLE_COLON";
        case TokenType::QUESTION_MARK: return "QUESTION_MARK";
        case TokenType::AT_SIGN: return "AT_SIGN";
        case TokenType::AMPERSAND: return "AMPERSAND";
        case TokenType::PIPE: return "PIPE";
        case TokenType::CARET: return "CARET";
        case TokenType::TILDE: return "TILDE";
        case TokenType::SHIFT_LEFT: return "SHIFT_LEFT";
        case TokenType::SHIFT_RIGHT: return "SHIFT_RIGHT";
        case TokenType::ARROW: return "ARROW";
        case TokenType::DOUBLE_ARROW: return "DOUBLE_ARROW";
        case TokenType::COLON_EQ: return "COLON_EQ";
        case TokenType::END_OF_INPUT: return "END_OF_INPUT";
        case TokenType::INVALID: return "INVALID";
        default: return "KEYWORD";
    }
}

} // namespace tdb::sql
