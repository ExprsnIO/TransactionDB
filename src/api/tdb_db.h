/*
** tdb_db.h — internal definitions of the database connection and prepared
** statement, shared between the executor (tdb_exec.c) and the public API
** (tdb_api.c).
**
** The executor currently MATERIALIZES result sets: a SELECT is run to
** completion into an array of rows that tdb_step() then walks. This keeps the
** first end-to-end implementation simple and correct; a lazy volcano pipeline
** can replace it behind the same tdb_step() surface later.
*/
#ifndef TDB_DB_H
#define TDB_DB_H

#include "transactiondb.h"
#include "../storage/tdb_storage.h"
#include "../catalog/tdb_catalog.h"
#include "../txn/tdb_txn.h"
#include "../txn/tdb_lock.h"
#include "../sql/tdb_ast.h"
#include "../value/tdb_value.h"
#include "../common/tdb_mutex.h"

struct tdb_db {
  tdb_pager   *pager;
  tdb_catalog *cat;
  tdb_lockmgr *lm;
  tdb_txnmgr  *tm;
  tdb_storage *engine;
  tdb_lua     *lua;        /* embedded Lua state (NULL if built without Lua) */
  tdb_txn     *txn;        /* the current transaction (auto or explicit) */
  int          autocommit; /* 1 = no explicit BEGIN in effect */
  int          flags;
  char        *path;
  char         errmsg[256];
  tdb_mutex   *mu;         /* serializes API calls: makes one tdb_db thread-safe */
};

struct tdb_stmt {
  tdb_db       *db;
  tdb_arena    *arena;     /* AST + analysis */
  tdb_ast_stmt *ast;

  /* bound parameters (1-based externally, 0-based here) */
  tdb_value    *params;
  int           nparams;

  /* result set (SELECT) */
  int           is_select;
  int           ncol;
  char        **colnames;  /* malloc'd */
  tdb_value   **rows;      /* nrows arrays of ncol values (malloc'd) */
  int           nrows, caprows;
  int           cur;       /* current row index, -1 before first step */
  int           executed;
  int64_t       changes;

  /* innermost in-scope WITH common-table-expression frame (executor-internal),
  ** a linked stack walked outward; NULL when no CTEs are in scope */
  struct cte_scope *cte_scope;
};

/* Set the connection error message. */
void tdb_db_seterr(tdb_db *db, const char *fmt, ...);

/* Execute a prepared statement: run DDL/DML to completion, or materialize a
** SELECT result set into stmt->rows. Implemented in tdb_exec.c. */
int  tdb_stmt_execute(tdb_stmt *stmt);

/* Free a materialized result set (rows + column names). */
void tdb_stmt_clear_results(tdb_stmt *stmt);

#endif /* TDB_DB_H */
