/*
** tdb_analyze.h — convert DDL AST nodes into heap schema objects.
**
** (DML/SELECT name resolution is performed inline by the executor against the
** catalog; this module handles the AST→catalog-object translation for DDL.)
*/
#ifndef TDB_ANALYZE_H
#define TDB_ANALYZE_H

#include "tdb_ast.h"
#include "../catalog/tdb_schema.h"

/* Build a heap tdb_table from a parsed CREATE TABLE. Caller owns the result
** (hand it to the catalog, or tdb_table_free it). Returns NULL + *err on
** error. */
tdb_table *tdb_ast_to_table(const tdb_create_table *ct, char **err);

#endif /* TDB_ANALYZE_H */
