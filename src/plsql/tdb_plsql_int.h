/*
** tdb_plsql_int.h — internal AST shared by the PL/SQL parser, interpreter and
** the LLVM IR emitter. Not a public header.
*/
#ifndef TDB_PLSQL_INT_H
#define TDB_PLSQL_INT_H

#include "tdb_plsql.h"
#include "../common/tdb_mem.h"

/* Operators (kept independent of the SQL lexer's token ids). */
typedef enum pl_op {
  PL_OP_ADD, PL_OP_SUB, PL_OP_MUL, PL_OP_DIV, PL_OP_MOD,
  PL_OP_EQ, PL_OP_NE, PL_OP_LT, PL_OP_LE, PL_OP_GT, PL_OP_GE,
  PL_OP_AND, PL_OP_OR, PL_OP_NEG, PL_OP_NOT, PL_OP_CONCAT
} pl_op;

typedef enum pl_ekind {
  PL_E_NUM,    /* numeric literal (is_int distinguishes integer vs real)   */
  PL_E_STR,    /* string literal                                           */
  PL_E_VAR,    /* variable / parameter reference by slot                   */
  PL_E_UNARY,  /* op in {NEG, NOT}                                         */
  PL_E_BINARY, /* binary op                                                */
  PL_E_CALL    /* builtin numeric function call                            */
} pl_ekind;

typedef struct pl_expr {
  pl_ekind kind;
  /* literal */
  double  num;
  int     is_int;
  char   *str;
  /* variable */
  int     slot;
  /* operator */
  pl_op   op;
  struct pl_expr *l, *r;
  /* call */
  char   *fname;
  struct pl_expr **args;
  int     nargs;
} pl_expr;

typedef enum pl_skind {
  PL_S_ASSIGN, PL_S_IF, PL_S_WHILE, PL_S_FOR, PL_S_RETURN, PL_S_NULL
} pl_skind;

/* One (condition, body) arm of an IF/ELSIF chain. */
typedef struct pl_clause {
  pl_expr        *cond;
  struct pl_stmt **body;
  int             nbody;
} pl_clause;

typedef struct pl_stmt {
  pl_skind kind;
  int      slot;          /* ASSIGN / FOR loop variable slot */
  pl_expr *e1, *e2;       /* ASSIGN: e1; RETURN: e1; FOR: lo=e1, hi=e2 */
  struct pl_stmt **body;  /* WHILE / FOR body */
  int      nbody;
  pl_clause *clauses;     /* IF: if-arm followed by elsif-arms */
  int      nclause;
  struct pl_stmt **elsebody;
  int      nelse;
} pl_stmt;

struct tdb_plsql_proc {
  tdb_arena *a;
  int        nslots;       /* total variable slots (params first) */
  char     **slotnames;    /* nslots entries */
  int        nparams;
  pl_stmt  **stmts;        /* top-level statement block */
  int        nstmt;
};

#endif /* TDB_PLSQL_INT_H */
