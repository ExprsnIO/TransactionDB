/* tdb_lexer.c — SQL tokenizer. */
#include "tdb_lexer.h"
#include "transactiondb.h"   /* TDB_OK / TDB_RANGE for the keyword API */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

void tdb_lex_init(tdb_lexer *lx, const char *sql, size_t len) {
  lx->z = sql;
  lx->len = len ? len : (sql ? strlen(sql) : 0);
  lx->pos = 0;
}

/* ----------------------------- keywords ------------------------------- */

typedef struct { const char *name; tdb_token_kind kind; } kw;
static const kw k_keywords[] = {
  {"SELECT",TK_SELECT},{"FROM",TK_FROM},{"WHERE",TK_WHERE},{"GROUP",TK_GROUP},
  {"BY",TK_BY},{"HAVING",TK_HAVING},{"ORDER",TK_ORDER},{"ASC",TK_ASC},
  {"DESC",TK_DESC},{"LIMIT",TK_LIMIT},{"OFFSET",TK_OFFSET},{"DISTINCT",TK_DISTINCT},
  {"ALL",TK_ALL},{"AS",TK_AS},{"JOIN",TK_JOIN},{"INNER",TK_INNER},{"LEFT",TK_LEFT},
  {"RIGHT",TK_RIGHT},{"FULL",TK_FULL},{"OUTER",TK_OUTER},{"CROSS",TK_CROSS},
  {"ON",TK_ON},{"USING",TK_USING},{"AND",TK_AND},{"OR",TK_OR},{"NOT",TK_NOT},
  {"NULL",TK_NULL},{"IS",TK_IS},{"IN",TK_IN},{"LIKE",TK_LIKE},{"BETWEEN",TK_BETWEEN},
  {"EXISTS",TK_EXISTS},{"CASE",TK_CASE},{"WHEN",TK_WHEN},{"THEN",TK_THEN},
  {"ELSE",TK_ELSE},{"END",TK_END},{"CAST",TK_CAST},{"TRUE",TK_TRUE},{"FALSE",TK_FALSE},
  {"INSERT",TK_INSERT},{"INTO",TK_INTO},{"VALUES",TK_VALUES},{"UPDATE",TK_UPDATE},
  {"SET",TK_SET},{"DELETE",TK_DELETE},{"CREATE",TK_CREATE},{"TABLE",TK_TABLE},
  {"INDEX",TK_INDEX},{"UNIQUE",TK_UNIQUE},{"VIEW",TK_VIEW},{"MATERIALIZED",TK_MATERIALIZED},
  {"DROP",TK_DROP},{"IF",TK_IF},{"TEMP",TK_TEMP},{"TEMPORARY",TK_TEMP},
  {"PRIMARY",TK_PRIMARY},{"KEY",TK_KEY},{"FOREIGN",TK_FOREIGN},{"REFERENCES",TK_REFERENCES},
  {"DEFAULT",TK_DEFAULT},{"CHECK",TK_CHECK},{"COLLATE",TK_COLLATE},
  {"CONSTRAINT",TK_CONSTRAINT},{"AUTOINCREMENT",TK_AUTOINCREMENT},
  {"GENERATED",TK_GENERATED},{"ALWAYS",TK_ALWAYS},{"STORED",TK_STORED},{"VIRTUAL",TK_VIRTUAL},
  {"WITH",TK_WITH},{"SYSTEM",TK_SYSTEM},{"VERSIONING",TK_VERSIONING},{"PERIOD",TK_PERIOD},
  {"FOR",TK_FOR},{"BEGIN",TK_BEGIN},{"COMMIT",TK_COMMIT},{"ROLLBACK",TK_ROLLBACK},
  {"TRANSACTION",TK_TRANSACTION},{"SAVEPOINT",TK_SAVEPOINT},{"RELEASE",TK_RELEASE},
  {"TO",TK_TO},{"FUNCTION",TK_FUNCTION},{"PROCEDURE",TK_PROCEDURE},{"LANGUAGE",TK_LANGUAGE},
  {"RETURNS",TK_RETURNS},{"CALL",TK_CALL},{"PREPARE",TK_PREPARE},{"ALTER",TK_ALTER},
  {"ADD",TK_ADD},{"COLUMN",TK_COLUMN},{"RENAME",TK_RENAME},
  {"UNION",TK_UNION},{"EXCEPT",TK_EXCEPT},{"INTERSECT",TK_INTERSECT},
  {"EXPLAIN",TK_EXPLAIN},{"VACUUM",TK_VACUUM},{"RETURNING",TK_RETURNING},
  {"GLOB",TK_GLOB},{"ESCAPE",TK_ESCAPE},
};

tdb_token_kind tdb_keyword_lookup(const char *z, int n) {
  for (size_t i = 0; i < sizeof(k_keywords) / sizeof(k_keywords[0]); i++) {
    if ((int)strlen(k_keywords[i].name) == n &&
        strncasecmp(z, k_keywords[i].name, (size_t)n) == 0)
      return k_keywords[i].kind;
  }
  return TK_ID;
}

/* ------------------------- keyword introspection ---------------------- */

/*
** The complete set of SQL keywords TransactionDB recognizes, comparable to
** SQLite's keyword list. Most are "non-reserved": the recursive-descent parser
** accepts them as identifiers where a name is expected (matching them by text),
** so they can still be used as table/column/function names. This table backs
** the SQLite-style introspection API (tdb_keyword_count/name/check) and is kept
** sorted for readability; lookup is a linear scan (the set is small).
*/
static const char *const k_all_keywords[] = {
  "ABORT","ACTION","ADD","AFTER","ALL","ALTER","ALWAYS","ANALYZE","AND","AS",
  "ASC","ATTACH","AUTOINCREMENT","BEFORE","BEGIN","BETWEEN","BY","CALL",
  "CASCADE","CASE","CAST","CHECK","COLLATE","COLUMN","COMMIT","CONFLICT",
  "CONSTRAINT","CREATE","CROSS","CURRENT","CURRENT_DATE","CURRENT_TIME",
  "CURRENT_TIMESTAMP","DATABASE","DEFAULT","DEFERRABLE","DEFERRED","DELETE",
  "DESC","DETACH","DISTINCT","DO","DROP","EACH","ELSE","ELSIF","END","ESCAPE",
  "EXCEPT","EXCLUDE","EXCLUSIVE","EXISTS","EXPLAIN","FAIL","FILTER","FIRST",
  "FOLLOWING","FOR","FOREIGN","FROM","FULL","FUNCTION","GENERATED","GLOB",
  "GROUP","GROUPS","HAVING","IF","IGNORE","IMMEDIATE","IN","INDEX","INDEXED",
  "INITIALLY","INNER","INSERT","INSTEAD","INTERSECT","INTO","IS","ISNULL",
  "JOIN","KEY","LANGUAGE","LAST","LEFT","LIKE","LIMIT","LOOP","MATCH",
  "MATERIALIZED","NATURAL","NO","NOT","NOTHING","NOTNULL","NULL","NULLS","OF",
  "OFFSET","ON","OR","ORDER","OTHERS","OUTER","OVER","PARTITION","PERIOD",
  "PLAN","PRAGMA","PRECEDING","PREPARE","PRIMARY","PROCEDURE","QUERY","RAISE",
  "RANGE","RECURSIVE","REFERENCES","REGEXP","REINDEX","RELEASE","RENAME",
  "REPLACE","RESTRICT","RETURNING","RETURNS","RIGHT","ROLLBACK","ROW","ROWS",
  "SAVEPOINT","SELECT","SEQUENCE","SET","STORED","SYSTEM","TABLE","TEMP",
  "TEMPORARY","THEN","TIES","TO","TRANSACTION","TRIGGER","UNBOUNDED","UNION",
  "UNIQUE","UPDATE","USING","VACUUM","VALUES","VERSIONING","VIEW","VIRTUAL",
  "WHEN","WHERE","WHILE","WINDOW","WITH","WITHOUT",
};

int tdb_keyword_count(void) {
  return (int)(sizeof(k_all_keywords) / sizeof(k_all_keywords[0]));
}

int tdb_keyword_name(int i, const char **pzName, int *pnName) {
  if (i < 0 || i >= tdb_keyword_count()) return TDB_RANGE;
  if (pzName) *pzName = k_all_keywords[i];
  if (pnName) *pnName = (int)strlen(k_all_keywords[i]);
  return TDB_OK;
}

int tdb_keyword_check(const char *z, int n) {
  if (!z) return 0;
  if (n < 0) n = (int)strlen(z);
  for (int i = 0; i < tdb_keyword_count(); i++) {
    if ((int)strlen(k_all_keywords[i]) == n &&
        strncasecmp(z, k_all_keywords[i], (size_t)n) == 0)
      return 1;
  }
  return 0;
}

/* ------------------------------- helpers ------------------------------ */

static int is_id_start(int c) { return isalpha(c) || c == '_'; }
static int is_id_char(int c) { return isalnum(c) || c == '_' || c == '$'; }

static void skip_trivia(tdb_lexer *lx) {
  for (;;) {
    while (lx->pos < lx->len && isspace((unsigned char)lx->z[lx->pos])) lx->pos++;
    if (lx->pos + 1 < lx->len && lx->z[lx->pos] == '-' && lx->z[lx->pos + 1] == '-') {
      lx->pos += 2;
      while (lx->pos < lx->len && lx->z[lx->pos] != '\n') lx->pos++;
      continue;
    }
    if (lx->pos + 1 < lx->len && lx->z[lx->pos] == '/' && lx->z[lx->pos + 1] == '*') {
      lx->pos += 2;
      while (lx->pos + 1 < lx->len &&
             !(lx->z[lx->pos] == '*' && lx->z[lx->pos + 1] == '/'))
        lx->pos++;
      if (lx->pos + 1 < lx->len) lx->pos += 2;
      else lx->pos = lx->len;
      continue;
    }
    break;
  }
}

static int at(tdb_lexer *lx, size_t off) {
  return (lx->pos + off < lx->len) ? (unsigned char)lx->z[lx->pos + off] : 0;
}

/* Scan a '...'-delimited string (delim ' or ", with doubled-delim escaping). */
static void scan_quoted(tdb_lexer *lx, char delim, tdb_token *out, tdb_token_kind k) {
  lx->pos++; /* opening delim */
  out->z = lx->z + lx->pos;
  size_t start = lx->pos;
  for (;;) {
    if (lx->pos >= lx->len) break;
    if (lx->z[lx->pos] == delim) {
      if (at(lx, 1) == (unsigned char)delim) { lx->pos += 2; continue; } /* escaped */
      break;
    }
    lx->pos++;
  }
  out->n = (int)(lx->pos - start);
  out->kind = k;
  if (lx->pos < lx->len) lx->pos++; /* closing delim */
}

static void parse_number(const char *z, int n, tdb_token *out) {
  char buf[80];
  int m = n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1;
  memcpy(buf, z, (size_t)m);
  buf[m] = '\0';
  if (out->kind == TK_INTEGER) {
    out->ival = (int64_t)strtoll(buf, NULL, 0); /* base 0 handles 0x */
    out->rval = (double)out->ival;
  } else {
    out->rval = strtod(buf, NULL);
    out->ival = (int64_t)out->rval;
  }
}

int tdb_lex_next(tdb_lexer *lx, tdb_token *out) {
  memset(out, 0, sizeof(*out));
  skip_trivia(lx);
  if (lx->pos >= lx->len) { out->kind = TK_EOF; out->z = lx->z + lx->pos; return TK_EOF; }

  const char *start = lx->z + lx->pos;
  int c = at(lx, 0);
  out->z = start;

  /* x'AABB' blob literal — must be checked before the identifier branch so
  ** the leading 'x' is not consumed as a name. */
  if ((c == 'x' || c == 'X') && at(lx, 1) == '\'') {
    lx->pos += 2;
    out->z = lx->z + lx->pos;
    size_t s = lx->pos;
    while (lx->pos < lx->len && lx->z[lx->pos] != '\'') lx->pos++;
    out->n = (int)(lx->pos - s);
    if (lx->pos < lx->len) lx->pos++;
    out->kind = TK_BLOB;
    return out->kind;
  }

  /* identifiers / keywords */
  if (is_id_start(c)) {
    size_t s = lx->pos;
    while (lx->pos < lx->len && is_id_char((unsigned char)lx->z[lx->pos])) lx->pos++;
    out->n = (int)(lx->pos - s);
    out->kind = tdb_keyword_lookup(out->z, out->n);
    return out->kind;
  }

  /* numbers */
  if (isdigit(c) || (c == '.' && isdigit(at(lx, 1)))) {
    size_t s = lx->pos;
    int isfloat = 0;
    if (c == '0' && (at(lx, 1) == 'x' || at(lx, 1) == 'X')) {
      lx->pos += 2;
      while (lx->pos < lx->len && isxdigit((unsigned char)lx->z[lx->pos])) lx->pos++;
    } else {
      while (lx->pos < lx->len && isdigit((unsigned char)lx->z[lx->pos])) lx->pos++;
      if (lx->pos < lx->len && lx->z[lx->pos] == '.') {
        isfloat = 1; lx->pos++;
        while (lx->pos < lx->len && isdigit((unsigned char)lx->z[lx->pos])) lx->pos++;
      }
      if (lx->pos < lx->len && (lx->z[lx->pos] == 'e' || lx->z[lx->pos] == 'E')) {
        isfloat = 1; lx->pos++;
        if (lx->pos < lx->len && (lx->z[lx->pos] == '+' || lx->z[lx->pos] == '-')) lx->pos++;
        while (lx->pos < lx->len && isdigit((unsigned char)lx->z[lx->pos])) lx->pos++;
      }
    }
    out->n = (int)(lx->pos - s);
    out->kind = isfloat ? TK_FLOAT : TK_INTEGER;
    parse_number(out->z, out->n, out);
    return out->kind;
  }

  /* string / quoted identifiers */
  if (c == '\'') { scan_quoted(lx, '\'', out, TK_STRING); return out->kind; }
  if (c == '"')  { scan_quoted(lx, '"',  out, TK_ID);     return out->kind; }
  if (c == '`')  { scan_quoted(lx, '`',  out, TK_ID);     return out->kind; }
  if (c == '[') {
    lx->pos++; out->z = lx->z + lx->pos; size_t s = lx->pos;
    while (lx->pos < lx->len && lx->z[lx->pos] != ']') lx->pos++;
    out->n = (int)(lx->pos - s);
    if (lx->pos < lx->len) lx->pos++;
    out->kind = TK_ID;
    return out->kind;
  }

  /* dollar-quote ($$..$$ / $tag$..$tag$) or $-parameter */
  if (c == '$') {
    size_t t = lx->pos + 1;
    while (t < lx->len && is_id_char((unsigned char)lx->z[t]) && lx->z[t] != '$') t++;
    if (t < lx->len && lx->z[t] == '$') {
      /* dollar-quoted string: tag is z[pos+1 .. t) */
      size_t taglen = t - (lx->pos + 1);
      const char *tag = lx->z + lx->pos + 1;
      lx->pos = t + 1;
      out->z = lx->z + lx->pos;
      size_t s = lx->pos;
      while (lx->pos < lx->len) {
        if (lx->z[lx->pos] == '$' &&
            lx->pos + taglen + 1 <= lx->len &&
            strncmp(lx->z + lx->pos + 1, tag, taglen) == 0 &&
            lx->z[lx->pos + 1 + taglen] == '$') {
          break;
        }
        lx->pos++;
      }
      out->n = (int)(lx->pos - s);
      out->kind = TK_STRING;
      if (lx->pos < lx->len) lx->pos += taglen + 2; /* closing $tag$ */
      return out->kind;
    }
    /* otherwise a parameter $name / $1 */
    lx->pos++;
    while (lx->pos < lx->len && is_id_char((unsigned char)lx->z[lx->pos])) lx->pos++;
    out->n = (int)(lx->z + lx->pos - start);
    out->kind = TK_PARAM;
    return out->kind;
  }
  if (c == '?' || c == ':' || c == '@') {
    lx->pos++;
    while (lx->pos < lx->len && is_id_char((unsigned char)lx->z[lx->pos])) lx->pos++;
    out->n = (int)(lx->z + lx->pos - start);
    out->kind = TK_PARAM;
    return out->kind;
  }

  /* operators / punctuation */
  lx->pos++;
  out->n = 1;
  switch (c) {
    case '(': out->kind = TK_LP; break;
    case ')': out->kind = TK_RP; break;
    case ',': out->kind = TK_COMMA; break;
    case ';': out->kind = TK_SEMI; break;
    case '.': out->kind = TK_DOT; break;
    case '*': out->kind = TK_STAR; break;
    case '+': out->kind = TK_PLUS; break;
    case '-': out->kind = TK_MINUS; break;
    case '/': out->kind = TK_SLASH; break;
    case '%': out->kind = TK_PERCENT; break;
    case '=': if (at(lx, 0) == '=') { lx->pos++; out->n = 2; } out->kind = TK_EQ; break;
    case '<':
      if (at(lx, 0) == '=') { lx->pos++; out->n = 2; out->kind = TK_LE; }
      else if (at(lx, 0) == '>') { lx->pos++; out->n = 2; out->kind = TK_NE; }
      else if (at(lx, 0) == '<') { lx->pos++; out->n = 2; out->kind = TK_SHL; }
      else out->kind = TK_LT;
      break;
    case '>':
      if (at(lx, 0) == '=') { lx->pos++; out->n = 2; out->kind = TK_GE; }
      else if (at(lx, 0) == '>') { lx->pos++; out->n = 2; out->kind = TK_SHR; }
      else out->kind = TK_GT;
      break;
    case '!':
      if (at(lx, 0) == '=') { lx->pos++; out->n = 2; out->kind = TK_NE; }
      else out->kind = TK_ILLEGAL;
      break;
    case '|':
      if (at(lx, 0) == '|') { lx->pos++; out->n = 2; out->kind = TK_CONCAT; }
      else out->kind = TK_BITOR;
      break;
    case '&': out->kind = TK_BITAND; break;
    case '~': out->kind = TK_BITNOT; break;
    default: out->kind = TK_ILLEGAL; break;
  }
  return out->kind;
}

const char *tdb_token_name(tdb_token_kind k) {
  switch (k) {
    case TK_EOF: return "EOF";
    case TK_ID: return "ID";
    case TK_STRING: return "STRING";
    case TK_INTEGER: return "INTEGER";
    case TK_FLOAT: return "FLOAT";
    case TK_BLOB: return "BLOB";
    case TK_PARAM: return "PARAM";
    case TK_LP: return "("; case TK_RP: return ")"; case TK_COMMA: return ",";
    case TK_SEMI: return ";"; case TK_DOT: return "."; case TK_STAR: return "*";
    case TK_PLUS: return "+"; case TK_MINUS: return "-"; case TK_SLASH: return "/";
    case TK_PERCENT: return "%"; case TK_EQ: return "="; case TK_NE: return "!=";
    case TK_LT: return "<"; case TK_LE: return "<="; case TK_GT: return ">";
    case TK_GE: return ">="; case TK_CONCAT: return "||";
    case TK_BITAND: return "&"; case TK_BITOR: return "|"; case TK_BITNOT: return "~";
    case TK_SHL: return "<<"; case TK_SHR: return ">>";
    case TK_AND: return "AND"; case TK_OR: return "OR"; case TK_NOT: return "NOT";
    case TK_IS: return "IS"; case TK_IN: return "IN"; case TK_LIKE: return "LIKE";
    case TK_BETWEEN: return "BETWEEN";
    case TK_ILLEGAL: return "ILLEGAL";
    default: return "KEYWORD";
  }
}
