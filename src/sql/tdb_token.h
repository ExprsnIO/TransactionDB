/*
** tdb_token.h — token kinds produced by the lexer.
**
** The set covers literals, punctuation/operators, and one code per SQL keyword
** recognized by the parser. Type names (INTEGER, VARCHAR, …) are NOT keywords:
** they arrive as TK_ID and are interpreted by tdb_typespec_parse().
*/
#ifndef TDB_TOKEN_H
#define TDB_TOKEN_H

typedef enum tdb_token_kind {
  TK_EOF = 0,
  TK_ILLEGAL,

  /* literals / names */
  TK_ID,        /* identifier (possibly quoted) */
  TK_STRING,    /* 'text' or $$dollar quoted$$ */
  TK_INTEGER,   /* 123, 0xFF */
  TK_FLOAT,     /* 1.5, 1e9 */
  TK_BLOB,      /* x'AABB' */
  TK_PARAM,     /* ?, ?N, :name, $name */

  /* punctuation */
  TK_LP, TK_RP, TK_COMMA, TK_SEMI, TK_DOT, TK_STAR,

  /* operators */
  TK_PLUS, TK_MINUS, TK_SLASH, TK_PERCENT,
  TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_CONCAT,
  TK_BITAND, TK_BITOR, TK_BITNOT, TK_SHL, TK_SHR,

  /* keywords (kept alphabetical-ish; values are opaque) */
  TK_SELECT, TK_FROM, TK_WHERE, TK_GROUP, TK_BY, TK_HAVING, TK_ORDER,
  TK_ASC, TK_DESC, TK_LIMIT, TK_OFFSET, TK_DISTINCT, TK_ALL, TK_AS,
  TK_JOIN, TK_INNER, TK_LEFT, TK_RIGHT, TK_FULL, TK_OUTER, TK_CROSS,
  TK_ON, TK_USING,
  TK_AND, TK_OR, TK_NOT, TK_NULL, TK_IS, TK_IN, TK_LIKE, TK_BETWEEN,
  TK_EXISTS, TK_CASE, TK_WHEN, TK_THEN, TK_ELSE, TK_END, TK_CAST,
  TK_TRUE, TK_FALSE,
  TK_INSERT, TK_INTO, TK_VALUES, TK_UPDATE, TK_SET, TK_DELETE,
  TK_CREATE, TK_TABLE, TK_INDEX, TK_UNIQUE, TK_VIEW, TK_MATERIALIZED,
  TK_DROP, TK_IF, TK_TEMP,
  TK_PRIMARY, TK_KEY, TK_FOREIGN, TK_REFERENCES, TK_DEFAULT, TK_CHECK,
  TK_COLLATE, TK_CONSTRAINT, TK_AUTOINCREMENT,
  TK_GENERATED, TK_ALWAYS, TK_STORED, TK_VIRTUAL,
  TK_WITH, TK_SYSTEM, TK_VERSIONING, TK_PERIOD, TK_FOR,
  TK_BEGIN, TK_COMMIT, TK_ROLLBACK, TK_TRANSACTION, TK_SAVEPOINT,
  TK_RELEASE, TK_TO,
  TK_FUNCTION, TK_PROCEDURE, TK_LANGUAGE, TK_RETURNS, TK_CALL,
  TK_PREPARE, TK_ALTER, TK_ADD, TK_COLUMN, TK_RENAME,
  TK_UNION, TK_EXCEPT, TK_INTERSECT, TK_EXPLAIN, TK_VACUUM,
  TK_RETURNING, TK_GLOB, TK_ESCAPE,
  /* tablespace / partition / housekeeping / authorization (Phase 11) */
  TK_TABLESPACE, TK_PARTITION, TK_RANGE, TK_LIST, TK_HASH,
  TK_TRUNCATE, TK_ANALYZE, TK_REINDEX, TK_COMMENT, TK_COMPRESSION,
  TK_GRANT, TK_REVOKE, TK_PRIVILEGES, TK_ROLE, TK_USER, TK_OWNED,
  TK_ATTACH, TK_DETACH, TK_DATABASE, TK_LOCK, TK_TYPE, TK_LOCATION,

  TK_MAX
} tdb_token_kind;

#endif /* TDB_TOKEN_H */
