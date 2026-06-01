/*
** transactiondb.h — public C API for TransactionDB.
**
** TransactionDB is a lightweight, embeddable, single-file SQL database engine
** written in C11. The API surface intentionally mirrors the ergonomics of
** SQLite (open / prepare / step / bind / column / finalize) so that it is
** familiar and easy to embed.
**
** All functions return an integer status code from `enum`-style TDB_* values
** unless otherwise noted. TDB_OK (0) indicates success.
*/
#ifndef TRANSACTIONDB_H
#define TRANSACTIONDB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */
#define TDB_VERSION_STRING "0.1.0"
#define TDB_VERSION_NUMBER 100   /* 0.1.0 -> 1_00 */

/* ------------------------------------------------------------------ */
/* Status / result codes                                              */
/* ------------------------------------------------------------------ */
typedef enum tdb_status {
  TDB_OK = 0,        /* successful result                       */
  TDB_ERROR,         /* generic error                           */
  TDB_BUSY,          /* resource is locked / conflict, retry    */
  TDB_LOCKED,        /* a table in the database is locked       */
  TDB_NOMEM,         /* a malloc() failed                       */
  TDB_READONLY,      /* attempt to write a readonly database    */
  TDB_IOERR,         /* some kind of disk I/O error occurred    */
  TDB_CORRUPT,       /* the database disk image is malformed    */
  TDB_NOTFOUND,      /* object not found                        */
  TDB_FULL,          /* database or disk is full                */
  TDB_CONSTRAINT,    /* abort due to constraint violation       */
  TDB_MISMATCH,      /* data type mismatch                      */
  TDB_MISUSE,        /* library used incorrectly                */
  TDB_RANGE,         /* bind/column index out of range          */
  TDB_ABORT,         /* operation aborted (e.g. by a hook)      */
  TDB_UNSUPPORTED,   /* feature recognized but not implemented  */
  TDB_DONE = 100,    /* tdb_step(): statement has finished      */
  TDB_ROW  = 101     /* tdb_step(): a row of data is ready      */
} tdb_status;

/* ------------------------------------------------------------------ */
/* Fundamental value types (column datatypes)                         */
/* ------------------------------------------------------------------ */
#define TDB_INTEGER 1
#define TDB_FLOAT   2
#define TDB_TEXT    3
#define TDB_BLOB    4
#define TDB_NULL    5

/* ------------------------------------------------------------------ */
/* Open flags                                                          */
/* ------------------------------------------------------------------ */
#define TDB_OPEN_READONLY   0x0001
#define TDB_OPEN_READWRITE  0x0002
#define TDB_OPEN_CREATE     0x0004
#define TDB_OPEN_MEMORY     0x0008  /* in-memory database (path ignored) */

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */
typedef struct tdb_db      tdb_db;      /* a database connection      */
typedef struct tdb_stmt    tdb_stmt;    /* a prepared statement       */
typedef struct tdb_value   tdb_value;   /* a dynamically typed value  */
typedef struct tdb_context tdb_context; /* context for a user function*/

/* ------------------------------------------------------------------ */
/* Connection lifecycle                                                */
/* ------------------------------------------------------------------ */
int  tdb_open(const char *filename, tdb_db **ppDb);
int  tdb_open_v2(const char *filename, tdb_db **ppDb, int flags);
int  tdb_close(tdb_db *db);

/* Human readable message for the most recent error on `db`. */
const char *tdb_errmsg(tdb_db *db);

/* Library-wide helpers. */
const char *tdb_libversion(void);
const char *tdb_status_str(int code);

/* ------------------------------------------------------------------ */
/* Connection statistics (SQLite-style)                                */
/* ------------------------------------------------------------------ */
/* Last rowid assigned by an INSERT on this connection (0 if none). */
int64_t tdb_last_insert_rowid(tdb_db *db);
/* Number of rows changed by the most recent successful DML statement.  */
int     tdb_changes(tdb_db *db);
/* Lifetime count of rows changed by DML since the connection opened.   */
int64_t tdb_total_changes(tdb_db *db);

/* ------------------------------------------------------------------ */
/* One-shot execution                                                  */
/* ------------------------------------------------------------------ */
/*
** Execute zero or more semicolon-separated SQL statements. For each result
** row the optional callback is invoked: (user, ncol, col_text[], col_name[]).
** A non-zero callback return aborts execution with TDB_ABORT.
*/
typedef int (*tdb_exec_cb)(void *user, int ncol, char **values, char **names);
int tdb_exec(tdb_db *db, const char *sql, tdb_exec_cb cb, void *user,
             char **errmsg);
void tdb_free(void *p); /* free strings returned via errmsg / column_text dup */

/* ------------------------------------------------------------------ */
/* Prepared statements                                                 */
/* ------------------------------------------------------------------ */
int tdb_prepare_v2(tdb_db *db, const char *sql, int nbyte,
                   tdb_stmt **ppStmt, const char **pzTail);
int tdb_step(tdb_stmt *stmt);     /* TDB_ROW / TDB_DONE / error */
int tdb_reset(tdb_stmt *stmt);
int tdb_finalize(tdb_stmt *stmt);

/* Parameter binding (1-based index). */
int tdb_bind_int64(tdb_stmt *stmt, int idx, int64_t v);
int tdb_bind_int(tdb_stmt *stmt, int idx, int v);
int tdb_bind_double(tdb_stmt *stmt, int idx, double v);
int tdb_bind_text(tdb_stmt *stmt, int idx, const char *v, int n);
int tdb_bind_blob(tdb_stmt *stmt, int idx, const void *v, int n);
int tdb_bind_null(tdb_stmt *stmt, int idx);
int tdb_bind_parameter_count(tdb_stmt *stmt);

/* Result columns (0-based index), valid after tdb_step() returns TDB_ROW. */
int         tdb_column_count(tdb_stmt *stmt);
int         tdb_column_type(tdb_stmt *stmt, int icol);
int64_t     tdb_column_int64(tdb_stmt *stmt, int icol);
int         tdb_column_int(tdb_stmt *stmt, int icol);
double      tdb_column_double(tdb_stmt *stmt, int icol);
const char *tdb_column_text(tdb_stmt *stmt, int icol);
const void *tdb_column_blob(tdb_stmt *stmt, int icol);
int         tdb_column_bytes(tdb_stmt *stmt, int icol);
const char *tdb_column_name(tdb_stmt *stmt, int icol);

/* ------------------------------------------------------------------ */
/* User-defined scalar functions (C)                                   */
/* ------------------------------------------------------------------ */
typedef void (*tdb_func)(tdb_context *ctx, int argc, tdb_value **argv);
int tdb_create_function(tdb_db *db, const char *name, int nArg,
                        tdb_func xFunc, void *pApp);

/* Argument accessors (inside an xFunc). */
int         tdb_value_type(tdb_value *v);
int64_t     tdb_value_int64(tdb_value *v);
double      tdb_value_double(tdb_value *v);
const char *tdb_value_text(tdb_value *v);
const void *tdb_value_blob(tdb_value *v);
int         tdb_value_bytes(tdb_value *v);

/* Result setters (inside an xFunc). */
void tdb_result_int64(tdb_context *ctx, int64_t v);
void tdb_result_double(tdb_context *ctx, double v);
void tdb_result_text(tdb_context *ctx, const char *v, int n);
void tdb_result_blob(tdb_context *ctx, const void *v, int n);
void tdb_result_null(tdb_context *ctx);
void tdb_result_error(tdb_context *ctx, const char *msg);
void *tdb_user_data(tdb_context *ctx);

/* ------------------------------------------------------------------ */
/* SQL keyword introspection (mirrors SQLite's keyword API)            */
/* ------------------------------------------------------------------ */
/* The number of SQL keywords TransactionDB recognizes. */
int tdb_keyword_count(void);
/* The i-th keyword (0-based): *pzName points at a static, NUL-terminated,
** uppercase name and *pnName its length. Returns TDB_OK or TDB_RANGE. */
int tdb_keyword_name(int i, const char **pzName, int *pnName);
/* Non-zero if the n-byte (n<0 => strlen) string is a keyword (case-insensitive). */
int tdb_keyword_check(const char *z, int n);

/* ------------------------------------------------------------------ */
/* C/C++ plugin engine                                                 */
/* ------------------------------------------------------------------ */
/* A loadable extension exposes an entry point of this type. It registers
** functions via tdb_create_function() and returns TDB_OK on success (on
** failure it may set *errmsg to a tdb_malloc'd string the caller frees). */
typedef int (*tdb_ext_init_fn)(tdb_db *db, char **errmsg);

/* Load a shared object and invoke its entry point (default name
** "tdb_extension_init"). The library handle is held until the connection
** closes. Returns TDB_OK or an error; *errmsg (if non-NULL) receives detail. */
int tdb_load_extension(tdb_db *db, const char *path, const char *entry,
                       char **errmsg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRANSACTIONDB_H */
