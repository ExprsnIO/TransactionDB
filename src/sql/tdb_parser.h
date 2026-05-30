/*
** tdb_parser.h — recursive-descent SQL parser producing a tdb_stmt AST.
*/
#ifndef TDB_PARSER_H
#define TDB_PARSER_H

#include "tdb_ast.h"

/*
** Parse a single statement from `sql`. On success returns TDB_OK and sets
** *out to an arena-allocated tdb_stmt; *tail (if non-NULL) points at the
** remaining input after the statement and any trailing ';'. On failure
** returns TDB_ERROR and sets *errmsg (arena-allocated) to a description.
** All AST memory lives in `a`.
*/
int tdb_parse(tdb_arena *a, const char *sql, tdb_stmt **out,
              char **errmsg, const char **tail);

#endif /* TDB_PARSER_H */
