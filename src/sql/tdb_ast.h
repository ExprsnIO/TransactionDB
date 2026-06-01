/*
** tdb_ast.h — abstract syntax tree produced by the parser.
**
** Every node is allocated from a tdb_arena owned by the caller; freeing the
** arena frees the whole tree (no per-node destructors). Literal values reuse
** tdb_value; declared column types reuse tdb_typespec (by value). DDL nodes
** capture expression/SELECT bodies as arena-copied source strings so the
** Phase-7 analyzer can drop them straight into the schema's *_sql fields.
*/
#ifndef TDB_AST_H
#define TDB_AST_H

#include "../common/tdb_internal.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_buf.h"
#include "../value/tdb_value.h"
#include "../value/tdb_sqltype.h"
#include "../value/tdb_type.h"
#include "../catalog/tdb_schema.h"   /* tdb_generated_kind, tdb_collation */
#include "tdb_token.h"

/* ------------------------------ expressions --------------------------- */

typedef enum tdb_expr_kind {
  EX_LITERAL, EX_NULL, EX_COLUMN, EX_PARAM, EX_UNARY, EX_BINARY,
  EX_FUNC, EX_AGG, EX_CASE, EX_CAST, EX_IN, EX_BETWEEN, EX_EXISTS,
  EX_SUBQUERY, EX_STAR,
  /* Window function call: function name + args (like EX_FUNC) plus a window
  ** spec (PARTITION BY / ORDER BY) on `win`. */
  EX_WINDOW
} tdb_expr_kind;

typedef struct tdb_expr     tdb_expr;
typedef struct tdb_exprlist tdb_exprlist;
typedef struct tdb_select   tdb_select;
typedef struct tdb_orderby  tdb_orderby;

/* Window spec: PARTITION BY <exprs> ORDER BY <exprs>. Frame clauses are
** unsupported; the executor implicitly uses "ROWS BETWEEN UNBOUNDED PRECEDING
** AND CURRENT ROW" for running aggregates, and the whole partition for
** ranking functions (ROW_NUMBER/RANK/DENSE_RANK). */
typedef struct tdb_window {
  tdb_exprlist *partition;       /* PARTITION BY ..., or NULL */
  tdb_orderby  *order;           /* ORDER BY ..., or NULL */
} tdb_window;

struct tdb_exprlist {
  tdb_expr **items;
  char     **aliases;   /* per-item alias or NULL */
  int        n, cap;
};

struct tdb_expr {
  tdb_expr_kind kind;
  int           op;       /* token kind for UNARY/BINARY */
  tdb_value     lit;      /* EX_LITERAL */
  char         *name;     /* column / function name */
  char         *table;    /* qualifier for EX_COLUMN (t.col) */
  char         *param;    /* EX_PARAM text */
  tdb_typespec  cast;     /* EX_CAST target type */
  int           distinct; /* EX_AGG: DISTINCT */
  int           negated;  /* EX_IN / EX_BETWEEN / EX_LIKE: NOT ... */
  int           col_index;/* analyzer: resolved column slot, or -1 */
  int           outer_level;/* 0 = local; >0 = correlated ref N scopes outward */
  int           agg_index;/* analyzer: aggregate slot, or -1 */
  int           fn_id;    /* analyzer: resolved builtin/aggregate id */
  int           win_index;/* analyzer: resolved window slot, or -1 */
  tdb_expr     *left;
  tdb_expr     *right;
  tdb_exprlist *args;     /* func args / IN list / CASE when-then pairs */
  tdb_select   *subquery; /* EX_SUBQUERY / EX_EXISTS / EX_IN (subquery form) */
  tdb_window   *win;      /* EX_WINDOW: OVER (PARTITION BY ... ORDER BY ...) */
};

/* ------------------------------- SELECT ------------------------------- */

typedef enum tdb_join_kind {
  JOIN_NONE = 0, JOIN_INNER, JOIN_LEFT, JOIN_RIGHT, JOIN_FULL, JOIN_CROSS
} tdb_join_kind;

typedef struct tdb_src {
  char          *table;     /* base table name (NULL if subquery) */
  char          *alias;
  tdb_select    *subquery;  /* derived table */
  tdb_join_kind  join;      /* join to the PRECEDING source */
  tdb_expr      *on;        /* ON condition */
  tdb_expr      *as_of;     /* FOR SYSTEM_TIME AS OF <expr>, or NULL */
  struct tdb_src *next;
} tdb_src;

typedef struct tdb_orderby {
  tdb_expr **exprs;
  uint8_t   *desc;
  int        n, cap;
} tdb_orderby;

/* A single common-table expression: `name [(col, ...)] AS (select)`.
** `active` is runtime state (set while the body is being materialized) that
** guards against a CTE referencing itself — non-recursive CTEs only. */
typedef struct tdb_cte {
  char       *name;
  char      **colnames;   /* optional column aliases, or NULL */
  int         ncolname;
  tdb_select *select;
  int         active;
  /* Iterative materialization for WITH RECURSIVE: the executor swaps a
  ** pre-computed rowset (the most recent iteration's rows) into `rows`
  ** before re-running the recursive arm. `rows` is a `tdb_stmt *` borrowed
  ** view, declared opaquely here to avoid pulling the executor types into
  ** the AST header. NULL means "evaluate `select` normally". */
  void       *rows;
} tdb_cte;

typedef struct tdb_ctelist {
  tdb_cte *items;
  int      n, cap;
  int      recursive;       /* WITH RECURSIVE ... */
} tdb_ctelist;

struct tdb_select {
  tdb_ctelist  *with;       /* WITH common table expressions, or NULL */
  tdb_exprlist *cols;       /* projection (EX_STAR for *) */
  int           distinct;
  tdb_src      *from;       /* may be NULL (SELECT <exprs>) */
  tdb_expr     *where;
  tdb_exprlist *group;
  tdb_expr     *having;
  tdb_orderby  *order;
  int           has_limit;
  tdb_expr     *limit;
  tdb_expr     *offset;
  /* set operation combining this SELECT with `setop_next` (TK_UNION /
  ** TK_EXCEPT / TK_INTERSECT, 0 = none); setop_all = UNION ALL */
  int           setop;
  int           setop_all;
  struct tdb_select *setop_next;
};

/* --------------------------- DDL: column defs ------------------------- */

typedef struct tdb_coldef {
  char         *name;
  tdb_typespec  type;
  int           notnull, pk, unique, autoinc, hidden;
  tdb_collation coll;
  char         *default_sql;    /* source text or NULL */
  char         *check_sql;
  tdb_generated_kind generated;
  char         *generated_sql;
} tdb_coldef;

/* PARTITION BY <strategy> (col, ...) — strategy values match the tokens. */
typedef enum tdb_partition_kind {
  TDB_PART_NONE = 0,
  TDB_PART_RANGE,
  TDB_PART_LIST,
  TDB_PART_HASH
} tdb_partition_kind;

typedef struct tdb_create_table {
  char       *name;
  int         if_not_exists;
  int         temp;
  tdb_coldef *cols;
  int         ncol;
  char      **pk_cols;  int npk;    /* table-level PRIMARY KEY (...) */
  char       *check_sql;            /* table-level CHECK */
  int         system_versioning;
  int         columnar;             /* WITH COLUMNAR */
  char       *period_start;         /* PERIOD FOR SYSTEM_TIME (start,end) */
  char       *period_end;

  /* Phase 11: placement and partitioning */
  char       *tablespace;           /* TABLESPACE <name>, or NULL */
  char       *compression;          /* WITH COMPRESSION=<name>, or NULL */
  tdb_partition_kind partition_kind;
  char      **partition_cols;       /* PARTITION BY <kind>(col, ...) */
  int         npart_col;
} tdb_create_table;

typedef struct tdb_create_index {
  char  *name;
  char  *table;
  int    unique;
  int    if_not_exists;
  char **cols;
  uint8_t *desc;
  int    ncol;
  int    kind;          /* tdb_index_kind: 0 = B-tree, 1 = R-tree (USING RTREE) */
} tdb_create_index;

/* ------------------------------ statements ---------------------------- */

typedef enum tdb_stmt_kind {
  ST_SELECT, ST_INSERT, ST_UPDATE, ST_DELETE,
  ST_CREATE_TABLE, ST_DROP_TABLE, ST_CREATE_INDEX, ST_DROP_INDEX,
  ST_CREATE_VIEW, ST_DROP_VIEW, ST_CREATE_ROUTINE, ST_DROP_ROUTINE, ST_CALL,
  ST_BEGIN, ST_COMMIT, ST_ROLLBACK, ST_SAVEPOINT, ST_RELEASE, ST_ROLLBACK_TO,
  ST_PREPARE, ST_ALTER_TABLE, ST_EXPLAIN, ST_VACUUM,
  /* Phase 11: housekeeping / namespace / authorization */
  ST_TRUNCATE, ST_ANALYZE, ST_REINDEX, ST_COMMENT,
  ST_CREATE_TABLESPACE, ST_DROP_TABLESPACE,
  ST_GRANT, ST_REVOKE, ST_ATTACH, ST_DETACH, ST_LOCK_TABLE
} tdb_stmt_kind;

typedef struct tdb_ast_stmt {
  tdb_stmt_kind kind;
  tdb_exprlist *returning;   /* RETURNING projection for INSERT/UPDATE/DELETE, or NULL */
  union {
    tdb_select *select;

    struct {
      char         *table;
      char        **cols;  int ncol;        /* optional column list */
      tdb_exprlist **rows; int nrows;        /* VALUES (...) , (...) */
      tdb_select   *select;                  /* INSERT ... SELECT */
      int           or_action;               /* 0 abort, 1 ignore, 2 replace */
      int           has_upsert;              /* ON CONFLICT ... present */
      int           upsert_nothing;          /* ON CONFLICT DO NOTHING */
      char        **up_cols;                 /* ON CONFLICT DO UPDATE SET ... */
      tdb_expr    **up_vals;
      int           up_nset;
      tdb_expr     *up_where;
    } insert;

    struct {
      char       *table;
      char      **set_cols;
      tdb_expr  **set_vals;
      int         nset;
      tdb_expr   *where;
    } update;

    struct { char *table; tdb_expr *where; } del;

    tdb_create_table *create_table;
    tdb_create_index *create_index;

    struct { char *name; int if_exists; } drop;   /* table/index/view */

    struct {
      char *name; int materialized; int if_not_exists; char *select_sql;
    } create_view;

    struct {
      char *name; int is_function; int or_replace;
      char **params; int nparams; char *lua_src; char *lang;
    } create_routine;

    struct { char *name; tdb_exprlist *args; } call;

    struct { char *name; } savepoint;              /* SAVEPOINT/RELEASE/ROLLBACK TO */

    struct { char *name; char *sql; } prepare;

    struct { char *table; int action; char *col; char *newname; tdb_coldef *add; } alter;
    struct { struct tdb_ast_stmt *inner; } explain;

    /* Phase 11 — simple "name + optional target" statements. */
    struct { char *name;        /* TRUNCATE TABLE <name> */
             int   restart;     /* RESTART IDENTITY */
             int   cascade;     /* CASCADE */
    } truncate;
    struct { char *name; } analyze;     /* ANALYZE [table]; name NULL => all */
    struct { char *name;        /* REINDEX [TABLE|INDEX] name; */
             int   is_index;
    } reindex;
    struct { int   on_kind;     /* 1 table, 2 column, 3 index, 4 view, 5 db */
             char *target;      /* "tbl" or "tbl.col" */
             char *body;        /* comment text */
    } comment_on;
    struct { char *name; char *location; int if_not_exists; } create_tablespace;
    struct { char *name; int   if_exists; } drop_tablespace;
    struct { char *privs;       /* comma-separated privilege keywords */
             char *object;      /* "TABLE foo" etc. */
             char *grantee;     /* role / user name */
             int   revoke;      /* 1 if REVOKE, 0 if GRANT */
    } grant;
    struct { char *name; char *path; int read_only; } attach;
    struct { char *name; } detach;
    struct { char *table; int exclusive; } lock_tbl;
  } u;
} tdb_ast_stmt;

/* INSERT OR <action> */
#define TDB_OR_ABORT   0
#define TDB_OR_IGNORE  1
#define TDB_OR_REPLACE 2

/* alter actions */
#define TDB_ALTER_ADD_COLUMN    1
#define TDB_ALTER_DROP_COLUMN   2
#define TDB_ALTER_RENAME_COLUMN 3
#define TDB_ALTER_RENAME_TABLE  4

/* ----------------------------- constructors --------------------------- */

tdb_expr     *tdb_expr_new(tdb_arena *a, tdb_expr_kind k);
tdb_exprlist *tdb_exprlist_new(tdb_arena *a);
void          tdb_exprlist_add(tdb_arena *a, tdb_exprlist *l, tdb_expr *e, char *alias);
tdb_orderby  *tdb_orderby_new(tdb_arena *a);
void          tdb_orderby_add(tdb_arena *a, tdb_orderby *o, tdb_expr *e, int desc);

/* Fully-parenthesized debug rendering of an expression (for tests). */
void tdb_expr_debug(tdb_buf *out, const tdb_expr *e);

#endif /* TDB_AST_H */
