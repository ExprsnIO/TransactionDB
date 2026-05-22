#ifndef TDB_SQL_TOKEN_H
#define TDB_SQL_TOKEN_H

#include <string>
#include <cstdint>

namespace tdb::sql {

enum class TokenType {
    // ─── Literals ───
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    BLOB_LITERAL,
    BOOLEAN_LITERAL,
    NULL_LITERAL,

    // ─── Identifiers ───
    IDENTIFIER,
    QUOTED_IDENTIFIER,

    // ─── Operators / Punctuation ───
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, NEQ, LT, GT, LTE, GTE,
    CONCAT,           // ||
    DOT, COMMA, SEMICOLON,
    LPAREN, RPAREN,
    LBRACKET, RBRACKET,
    COLON, DOUBLE_COLON,  // :: (cast)
    QUESTION_MARK,        // ? (parameter)
    AT_SIGN,              // @ (variable)
    // Bitwise operators
    AMPERSAND,            // &
    PIPE,                 // | (single)
    CARET,                // ^
    TILDE,                // ~
    SHIFT_LEFT,           // <<
    SHIFT_RIGHT,          // >>
    // JSON operators
    ARROW,                // ->
    DOUBLE_ARROW,         // ->>
    // Assignment
    COLON_EQ,             // :=

    // ─── Keywords — DDL ───
    KW_CREATE, KW_ALTER, KW_DROP, KW_TRUNCATE,
    KW_TABLE, KW_INDEX, KW_VIEW, KW_SEQUENCE,
    KW_SCHEMA, KW_DATABASE, KW_TRIGGER,
    KW_COLUMN, KW_CONSTRAINT,
    KW_PRIMARY, KW_FOREIGN, KW_KEY, KW_REFERENCES,
    KW_UNIQUE, KW_CHECK, KW_DEFAULT,
    KW_NOT, KW_NULL_KW,
    KW_AUTO_INCREMENT,
    KW_IF, KW_EXISTS,
    KW_CASCADE, KW_RESTRICT, KW_SET,
    KW_ADD, KW_RENAME, KW_TO,
    KW_TEMPORARY, KW_TEMP,
    KW_USING,
    KW_ON, KW_REPLACE,
    KW_FUNCTION, KW_PROCEDURE, KW_RETURNS, KW_RETURN,
    KW_LANGUAGE, KW_CALLED, KW_DETERMINISTIC,
    KW_DOMAIN, KW_TYPE,
    KW_ROLE, KW_USER,
    KW_AUTHORIZATION,
    KW_GENERATED, KW_ALWAYS, KW_IDENTITY,
    KW_DEFERRED, KW_IMMEDIATE, KW_INITIALLY,
    KW_DEFERRABLE,
    KW_NO, KW_ACTION,
    KW_PRESERVE, KW_DELETE_KW,  // ON COMMIT DELETE/PRESERVE ROWS
    KW_GLOBAL, KW_LOCAL,
    KW_COMMENT,

    // ─── Keywords — DML ───
    KW_SELECT, KW_INSERT, KW_UPDATE, KW_DELETE, KW_MERGE,
    KW_FROM, KW_WHERE, KW_GROUP, KW_HAVING, KW_ORDER,
    KW_BY, KW_ASC, KW_DESC, KW_NULLS, KW_FIRST, KW_LAST,
    KW_LIMIT, KW_OFFSET, KW_FETCH, KW_NEXT, KW_ROWS, KW_ONLY,
    KW_INTO, KW_VALUES, KW_AS, KW_ALL, KW_DISTINCT,
    KW_UNION, KW_INTERSECT, KW_EXCEPT,
    KW_JOIN, KW_INNER, KW_LEFT, KW_RIGHT, KW_FULL, KW_OUTER, KW_CROSS, KW_NATURAL,
    KW_WITH, KW_RECURSIVE,
    KW_CASE, KW_WHEN, KW_THEN, KW_ELSE, KW_END,
    KW_MATCHED, KW_TARGET, KW_SOURCE,
    KW_RETURNING,
    KW_CONFLICT,       // ON CONFLICT
    KW_NOTHING,        // DO NOTHING
    KW_DO,
    KW_TIES,           // WITH TIES
    KW_TABLESAMPLE,
    KW_BERNOULLI,
    KW_SYSTEM_KW,      // TABLESAMPLE SYSTEM
    KW_FOR,
    KW_SHARE,
    KW_NOWAIT,
    KW_SKIP_KW,        // SKIP LOCKED
    KW_LOCKED,
    KW_CORRESPONDING,
    KW_MATERIALIZED,
    KW_CALL,

    // ─── Keywords — Logical / Predicates ───
    KW_AND, KW_OR, KW_IN, KW_BETWEEN, KW_LIKE, KW_ILIKE,
    KW_IS, KW_ANY, KW_SOME, KW_EVERY,
    KW_SIMILAR,        // SIMILAR TO
    KW_OVERLAPS,       // temporal OVERLAPS
    KW_UNKNOWN,        // IS UNKNOWN
    KW_SYMMETRIC,      // BETWEEN SYMMETRIC
    KW_ASYMMETRIC,     // BETWEEN ASYMMETRIC

    // ─── Keywords — Aggregate / Window ───
    KW_COUNT, KW_SUM, KW_AVG, KW_MIN, KW_MAX,
    KW_OVER, KW_PARTITION, KW_WINDOW,
    KW_RANGE, KW_ROW, KW_UNBOUNDED, KW_PRECEDING, KW_FOLLOWING, KW_CURRENT,
    KW_GROUPS,         // window frame GROUPS mode
    KW_EXCLUDE,        // EXCLUDE CURRENT ROW / GROUP / TIES / NO OTHERS
    KW_OTHERS,
    KW_FILTER,         // aggregate FILTER (WHERE ...)
    KW_WITHIN,         // WITHIN GROUP
    KW_GROUPING,       // GROUPING(), GROUPING SETS
    KW_SETS,
    KW_CUBE,
    KW_ROLLUP,
    // Named window functions (recognized as keywords for clarity)
    KW_ROW_NUMBER, KW_RANK, KW_DENSE_RANK, KW_NTILE,
    KW_LAG, KW_LEAD, KW_FIRST_VALUE, KW_LAST_VALUE, KW_NTH_VALUE,
    KW_PERCENT_RANK, KW_CUME_DIST,
    KW_LISTAGG,
    KW_PERCENTILE_CONT, KW_PERCENTILE_DISC, KW_MODE,

    // ─── Keywords — Types ───
    KW_INT, KW_INTEGER, KW_SMALLINT, KW_BIGINT, KW_TINYINT,
    KW_FLOAT, KW_DOUBLE, KW_REAL, KW_DECIMAL, KW_NUMERIC,
    KW_PRECISION,      // DOUBLE PRECISION
    KW_CHAR, KW_VARCHAR, KW_TEXT,
    KW_CHARACTER,       // CHARACTER / CHARACTER VARYING
    KW_VARYING,         // CHARACTER VARYING
    KW_NATIONAL, KW_NCHAR, KW_NVARCHAR, KW_NCLOB,
    KW_BOOLEAN, KW_BOOL,
    KW_DATE, KW_TIME, KW_TIMESTAMP, KW_INTERVAL,
    KW_BLOB, KW_CLOB,
    KW_BINARY, KW_VARBINARY,
    KW_BIT,
    KW_JSON, KW_JSONB, KW_XML,
    KW_ARRAY,
    KW_MULTISET,
    KW_GEOMETRY, KW_GEOGRAPHY, KW_RASTER,
    KW_SERIAL, KW_BIGSERIAL,
    KW_UUID,
    KW_ENUM,
    KW_MONEY,

    // ─── Keywords — Date/Time ───
    KW_ZONE,           // TIME ZONE
    KW_WITHOUT,        // WITHOUT TIME ZONE
    KW_EXTRACT,        // EXTRACT(field FROM expr)
    KW_YEAR, KW_MONTH, KW_DAY,
    KW_HOUR, KW_MINUTE, KW_SECOND,
    KW_EPOCH,
    KW_WEEK, KW_QUARTER,
    KW_DOW, KW_DOY,
    KW_CURRENT_DATE, KW_CURRENT_TIME, KW_CURRENT_TIMESTAMP,
    KW_LOCALTIME, KW_LOCALTIMESTAMP,
    KW_AT_KW,          // AT TIME ZONE (keyword AT distinct from @ operator)

    // ─── Keywords — String functions (special syntax) ───
    KW_SUBSTRING,      // SUBSTRING(str FROM n FOR n)
    KW_TRIM,           // TRIM(LEADING|TRAILING|BOTH x FROM str)
    KW_LEADING, KW_TRAILING, KW_BOTH,
    KW_POSITION,       // POSITION(substr IN str)
    KW_OVERLAY,        // OVERLAY(str PLACING repl FROM n FOR n)
    KW_PLACING,
    KW_UPPER, KW_LOWER,
    KW_NORMALIZE,
    KW_TRANSLATE,
    KW_CONVERT,
    KW_COALESCE,       // COALESCE(a, b, ...)
    KW_NULLIF,         // NULLIF(a, b)
    KW_GREATEST, KW_LEAST,
    KW_CONCAT,         // CONCAT function

    // ─── Keywords — Transaction ───
    KW_BEGIN, KW_COMMIT, KW_ROLLBACK, KW_SAVEPOINT, KW_RELEASE,
    KW_TRANSACTION, KW_ISOLATION, KW_LEVEL,
    KW_READ, KW_WRITE, KW_COMMITTED, KW_UNCOMMITTED,
    KW_REPEATABLE, KW_SERIALIZABLE, KW_SNAPSHOT,
    KW_START,          // START TRANSACTION
    KW_WORK,           // COMMIT WORK
    KW_CONSTRAINTS,    // SET CONSTRAINTS

    // ─── Keywords — Subquery / Exists / Lateral ───
    KW_EXISTS_KW, KW_LATERAL,
    KW_UNNEST,         // UNNEST(array)

    // ─── Keywords — Access Control ───
    KW_GRANT, KW_REVOKE, KW_PRIVILEGES,
    KW_EXECUTE,
    KW_USAGE,
    KW_OPTION,         // WITH GRANT OPTION
    KW_PUBLIC,

    // ─── Keywords — Misc ───
    KW_EXPLAIN, KW_ANALYZE, KW_VACUUM,
    KW_CAST, KW_COLLATE, KW_ESCAPE,
    KW_TRUE, KW_FALSE,
    KW_SHOW,
    KW_DESCRIBE,
    KW_PREPARE, KW_DEALLOCATE,
    KW_DECLARE, KW_CURSOR, KW_OPEN, KW_CLOSE,
    KW_SCROLL, KW_INSENSITIVE, KW_SENSITIVE,
    KW_HOLD,           // WITH HOLD
    KW_LOCK_KW,        // LOCK TABLE
    KW_COPY,
    KW_VERBOSE,
    KW_FORMAT,
    KW_ISNULL, KW_NOTNULL,  // PostgreSQL shorthand
    KW_BETWEEN_SYMMETRIC,   // parsed combination

    // ─── Keywords — ENCRYPTED / TDE ───
    KW_ENCRYPT, KW_ENCRYPTED, KW_DECRYPT,

    // ─── Keywords — Trigger specifics ───
    KW_BEFORE, KW_AFTER, KW_INSTEAD,
    KW_EACH,           // FOR EACH ROW
    KW_NEW, KW_OLD,
    KW_REFERENCING,
    KW_STATEMENT,

    // ─── Keywords — PSM / Procedural ───
    KW_ATOMIC,
    KW_IF_KW,          // IF in PSM (distinct from DDL IF)
    KW_ELSEIF, KW_WHILE, KW_LOOP, KW_REPEAT, KW_UNTIL, KW_LEAVE, KW_ITERATE,
    KW_SIGNAL, KW_RESIGNAL,
    KW_HANDLER,
    KW_CONDITION,
    KW_SQLSTATE,
    KW_SQLEXCEPTION, KW_SQLWARNING,
    KW_INOUT, KW_OUT,

    // ─── Keywords — JSON (SQL:2016) ───
    KW_JSON_OBJECT, KW_JSON_ARRAY, KW_JSON_VALUE, KW_JSON_QUERY,
    KW_JSON_TABLE, KW_JSON_EXISTS,

    // ─── Keywords — XML (SQL:2003) ───
    KW_XMLELEMENT, KW_XMLFOREST, KW_XMLAGG, KW_XMLPARSE,
    KW_XMLSERIALIZE, KW_XMLNAMESPACES,

    // ─── Keywords — Views / Materialized Views ───
    KW_REFRESH,
    KW_CONCURRENTLY,
    KW_WRITABLE,

    // ─── Keywords — Saved Queries ───
    KW_SAVE, KW_SAVED, KW_QUERY,

    // ─── Keywords — Tablespaces ───
    KW_TABLESPACE,
    KW_LOCATION,
    KW_OWNER,
    KW_MOVE,

    // ─── Keywords — Logging ───
    KW_LOG, KW_LOGGING, KW_NOLOGGING,
    KW_AUDIT, KW_NOAUDIT,

    // ─── Keywords — Document Query (XPath / XQuery / GraphQL) ───
    KW_XPATH, KW_XQUERY,
    KW_GRAPHQL,
    KW_DOCUMENT, KW_CONTENT, KW_PATH,
    KW_PASSING, KW_RETURNING_KW,
    KW_COLUMNS_KW,

    // ─── Keywords — Locking ───
    KW_OBJECT,
    KW_ADVISORY,
    KW_INTENT,
    KW_ACCESS,
    KW_EXCLUSIVE_KW,
    KW_ROW_EXCLUSIVE,
    KW_SHARE_UPDATE,
    KW_SHARE_ROW,
    KW_NOWAIT_KW,
    KW_WAIT,

    // ─── Keywords — Partitioning ───
    KW_HASH,
    KW_LIST,
    KW_LESS,
    KW_THAN,
    KW_MODULUS,
    KW_REMAINDER,
    KW_MAXVALUE,
    KW_MINVALUE,

    // ─── Keywords — Columnar / Federation ───
    KW_COLUMNAR,
    KW_EXTERNAL_KW,
    KW_FOREIGN_TABLE,
    KW_SERVER,
    KW_WRAPPER,
    KW_OPTIONS,
    KW_MAPPING,

    // ─── Graph queries ───
    KW_MATCH,
    KW_GRAPH,
    KW_VERTEX,
    KW_EDGE,

    // ─── Special ───
    END_OF_INPUT,
    INVALID,
};

struct Token {
    TokenType   type;
    std::string value;
    uint32_t    line;
    uint32_t    col;

    Token() : type(TokenType::INVALID), line(0), col(0) {}
    Token(TokenType t, std::string v, uint32_t l, uint32_t c)
        : type(t), value(std::move(v)), line(l), col(c) {}
};

const char *token_type_name(TokenType type);

} // namespace tdb::sql

#endif // TDB_SQL_TOKEN_H
