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

struct tdb_env;            /* shared environment (tdb_env.h) */

/* A registered user-defined scalar function (C/C++ plugin entry). */
typedef struct tdb_func_entry {
  char                  *name;
  int                    nArg;   /* required arity, or -1 for variadic */
  tdb_func               fn;
  void                  *pApp;
  struct tdb_func_entry *next;
} tdb_func_entry;

/* A dynamically loaded extension shared object, kept so it can be closed. */
typedef struct tdb_ext_handle {
  void                  *dl;     /* dlopen handle */
  struct tdb_ext_handle *next;
} tdb_ext_handle;

struct tdb_plsql_proc;           /* opaque compiled PL/SQL routine (tdb_plsql.h) */

/* A stored PL/SQL routine (LANGUAGE PLSQL) compiled on this connection. */
typedef struct tdb_plsql_routine {
  char                     *name;
  struct tdb_plsql_proc    *proc;
  int                       is_function;
  int                       nparams;
  struct tdb_plsql_routine *next;
} tdb_plsql_routine;

/* A named sequence generator (nextval/currval/setval). */
typedef struct tdb_sequence {
  char                *name;
  int64_t              cur;
  int64_t              inc;
  int                  has_cur;     /* 0 until the first nextval/setval */
  struct tdb_sequence *next;
} tdb_sequence;

/* Context passed to a user function: it reads pApp and writes the result. */
struct tdb_context {
  tdb_db    *db;
  void      *pApp;
  tdb_value *out;      /* result destination (borrowed) */
  int        is_error;
  char       errmsg[256];
};

struct tdb_db {
  struct tdb_env *env;     /* shared resources (pager/catalog/locks/txn/engine) */

  /* Borrowed from `env` (not owned): kept as direct fields so the executor and
  ** the rest of the engine reach them unchanged. */
  tdb_pager   *pager;
  tdb_catalog *cat;
  tdb_lockmgr *lm;
  tdb_txnmgr  *tm;
  tdb_storage *engine;

  tdb_func_entry *funcs;   /* registered user-defined functions (C/C++ plugins) */
  tdb_ext_handle *exts;    /* loaded extension shared objects */
  tdb_plsql_routine *plroutines; /* stored LANGUAGE PLSQL routines */
  tdb_sequence   *seqs;    /* named sequence generators */

  tdb_lua     *lua;        /* embedded Lua state (NULL if built without Lua); per-connection */
  tdb_txn     *txn;        /* the current transaction (auto or explicit); per-connection */
  int          autocommit; /* 1 = no explicit BEGIN in effect */
  int          flags;
  char        *path;
  char         errmsg[256];
  tdb_mutex   *mu;         /* serializes API calls: makes one tdb_db thread-safe */

  /* SQLite-style statistics (per connection) updated by DML statements. */
  int64_t      last_insert_rowid;
  int64_t      changes;        /* rows changed by the most recent successful DML */
  int64_t      total_changes;  /* lifetime row-change count since open */
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

  /* lazy (volcano) streaming: when `plan` is set, tdb_step() pulls one row at a
  ** time through the operator tree instead of reading the materialized `rows`.
  ** `stream_row` holds the current output row (ncol values); `own_txn` is a
  ** statement-held read snapshot kept open across steps (NULL if the scan rides
  ** an explicit connection transaction); `stream_done` latches end-of-stream. */
  void         *plan;       /* tdb_op* operator-tree root, or NULL */
  tdb_value    *stream_row;
  tdb_txn      *own_txn;
  int           streaming;
  int           stream_done;

  /* innermost in-scope WITH common-table-expression frame (executor-internal),
  ** a linked stack walked outward; NULL when no CTEs are in scope */
  struct cte_scope *cte_scope;
};

/* Set the connection error message. */
void tdb_db_seterr(tdb_db *db, const char *fmt, ...);

/* Look up a registered user function by name and arity (NULL if none). */
const tdb_func_entry *tdb_db_find_function(tdb_db *db, const char *name, int argc);

/* Register / find a stored PL/SQL routine (proc ownership transfers to db). */
int  tdb_db_add_plsql(tdb_db *db, const char *name, struct tdb_plsql_proc *proc,
                      int is_function, int nparams);
const tdb_plsql_routine *tdb_db_find_plsql(tdb_db *db, const char *name);

/* Find (optionally creating with start=1,inc=1) a named sequence. */
tdb_sequence *tdb_db_seq_find(tdb_db *db, const char *name, int create);

/* Execute a prepared statement: run DDL/DML to completion, or prepare a SELECT
** result — either materialized into stmt->rows or, for a streamable scan, armed
** as a lazy operator tree pulled by tdb_step(). Implemented in tdb_exec.c. */
int  tdb_stmt_execute(tdb_stmt *stmt);

/* Free a materialized result set (rows + column names). */
void tdb_stmt_clear_results(tdb_stmt *stmt);

/* Lazy streaming (volcano) hooks, implemented in tdb_exec.c. When a SELECT is
** compiled to an operator tree, tdb_step() pulls one row per call:
**   tdb_stmt_stream_next advances the tree, leaving the current row in
**   stmt->stream_row and returning TDB_ROW / TDB_DONE / an error code;
**   tdb_stmt_stream_close tears down the tree and releases the read snapshot. */
int  tdb_stmt_stream_next(tdb_stmt *stmt);
void tdb_stmt_stream_close(tdb_stmt *stmt);

#endif /* TDB_DB_H */
