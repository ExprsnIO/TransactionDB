/*
** tdb_lexer.h — SQL tokenizer.
**
** Produces a stream of tokens from a NUL-terminated (or length-bounded) SQL
** string. Token text (`z`,`n`) points into the source buffer (zero-copy);
** numeric literals are pre-parsed into `ival`/`rval`. The lexer is purely
** lexical: it does not allocate.
*/
#ifndef TDB_LEXER_H
#define TDB_LEXER_H

#include "tdb_token.h"
#include <stdint.h>
#include <stddef.h>

typedef struct tdb_token {
  tdb_token_kind kind;
  const char    *z;   /* start of token text in the source */
  int            n;   /* length (for strings, the decoded-content length) */
  int64_t        ival;
  double         rval;
} tdb_token;

typedef struct tdb_lexer {
  const char *z;      /* source */
  size_t      len;    /* source length */
  size_t      pos;    /* current offset */
} tdb_lexer;

void tdb_lex_init(tdb_lexer *lx, const char *sql, size_t len);

/* Fill *out with the next token. Returns the token kind (TK_EOF at end). */
int  tdb_lex_next(tdb_lexer *lx, tdb_token *out);

/* Map an identifier slice to a keyword kind, or TK_ID if not a keyword. */
tdb_token_kind tdb_keyword_lookup(const char *z, int n);

/* Human-readable token-kind name (for diagnostics/tests). */
const char *tdb_token_name(tdb_token_kind k);

#endif /* TDB_LEXER_H */
