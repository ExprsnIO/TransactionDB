/*
** tdb_schema.h — in-memory schema object model.
**
** These structures describe tables, columns, indexes, views and routines as
** held in memory after being loaded from the catalog. They are defined here
** (ahead of the catalog/executor) so the design is fixed: strict typing,
** calculated (generated) columns, column references, and BOTH temporal
** (system-versioned) and schema/DDL versioning are first-class.
**
** Expression-valued fields (defaults, generated/computed expressions, CHECK
** clauses, view bodies, routine bodies) are stored as their SQL/Lua source
** text; the analyzer compiles them to an AST/plan when a statement runs.
*/
#ifndef TDB_SCHEMA_H
#define TDB_SCHEMA_H

#include "../common/tdb_internal.h"
#include "../value/tdb_sqltype.h"
#include "../value/tdb_type.h"

/* A reference from one column to another (used inside generated-column and
** CHECK expressions, and by foreign keys). */
typedef struct tdb_colref {
  char *table;   /* referenced table, NULL = same table */
  char *column;  /* referenced column name */
} tdb_colref;

/* How a calculated/generated column is materialized. */
typedef enum tdb_generated_kind {
  TDB_GEN_NONE = 0,
  TDB_GEN_VIRTUAL,   /* computed on read, not stored */
  TDB_GEN_STORED     /* computed on write, persisted */
} tdb_generated_kind;

typedef struct tdb_column {
  char         *name;
  tdb_typespec  type;          /* strict declared type */
  tdb_collation coll;
  int           notnull;
  int           pk;            /* member of the primary key */
  int           unique;
  int           hidden;        /* hidden system column (e.g. temporal bounds) */
  char         *default_sql;   /* DEFAULT expression source, or NULL */
  char         *check_sql;     /* column-level CHECK source, or NULL */

  /* calculated / generated column */
  tdb_generated_kind generated;
  char         *generated_sql; /* SQL expression, e.g. "qty * price" */
  char         *lua_compute;   /* alternative: a Lua expression body */
  tdb_colref   *refs;          /* columns referenced by the expression */
  int           nref;
} tdb_column;

/* Foreign-key reference. */
typedef struct tdb_fkey {
  char  *name;
  int    ncol;
  char **cols;        /* local columns */
  char  *ref_table;
  char **ref_cols;    /* referenced columns */
  int    on_delete;   /* action code (NO ACTION/CASCADE/SET NULL/RESTRICT) */
  int    on_update;
} tdb_fkey;

typedef struct tdb_index {
  char     *name;
  tdb_pgno  root;
  int       unique;
  int       ncol;
  int      *col_idx;  /* indices into the table's column array */
  uint8_t  *desc;     /* per-column descending flag */
} tdb_index;

/* A snapshot of the column layout at a particular schema version. Retained so
** rows written under an older layout can still be decoded after ALTER. */
typedef struct tdb_collayout {
  uint32_t      schema_version;
  int           ncol;
  char        **names;
  tdb_typespec *types;
} tdb_collayout;

typedef enum tdb_versioning {
  TDB_VERSIONING_NONE = 0,
  TDB_VERSIONING_SYSTEM   /* system-versioned temporal table (SQL:2011) */
} tdb_versioning;

typedef struct tdb_table {
  char        *name;
  tdb_pgno     root;        /* current data B-tree root page */
  int          ncol;
  tdb_column  *cols;

  tdb_index   *indexes;  int nindex;
  tdb_fkey    *fkeys;    int nfkey;
  char        *check_sql;   /* table-level CHECK */

  /* schema / DDL versioning: bumped on every ALTER; prior layouts retained */
  uint32_t        schema_version;
  tdb_collayout  *history;
  int             nhistory;

  /* temporal / system versioning */
  tdb_versioning  versioning;
  int             row_start_col;  /* hidden valid-from column index, or -1 */
  int             row_end_col;    /* hidden valid-to column index, or -1   */
  tdb_pgno        history_root;   /* B-tree root for superseded row versions */

  /* table-level Lua hooks (source text), invoked by the executor */
  char *before_insert_lua;
  char *before_update_lua;
  char *before_delete_lua;
} tdb_table;

typedef struct tdb_view {
  char     *name;
  char     *select_sql;
  int       materialized;
  tdb_pgno  root;     /* storage for a materialized view */
  int       stale;    /* materialized view needs refresh */
} tdb_view;

typedef struct tdb_routine {
  char *name;
  char *lua_src;
  int   is_function;  /* 1 = scalar function, 0 = procedure */
  int   nargs;        /* -1 = variadic */
} tdb_routine;

/* A saved/named query (prepared and reusable by name). */
typedef struct tdb_saved_query {
  char *name;
  char *sql;
} tdb_saved_query;

/* ------------------------- construction helpers ----------------------- */
tdb_table  *tdb_table_new(const char *name);
int         tdb_table_add_column(tdb_table *t, const tdb_column *col);
int         tdb_table_find_column(const tdb_table *t, const char *name);
void        tdb_table_free(tdb_table *t);

void        tdb_column_init(tdb_column *c, const char *name, tdb_typespec ts);
void        tdb_column_free(tdb_column *c);

void        tdb_view_free(tdb_view *v);
void        tdb_routine_free(tdb_routine *r);

#endif /* TDB_SCHEMA_H */
