/*
** tdb_jit_sql.c — JIT front end for numeric SQL scalar expressions.
**
** Grammar (a strict subset, sufficient for WHERE/CASE/projection scalars):
**
**   expr     := or
**   or       := and ( ( 'OR' | '||' AND ) and )*   // '||' overloaded to OR
**   and      := not ( 'AND' not )*
**   not      := 'NOT' not | rel
**   rel      := add ( ( '=' | '==' | '<>' | '!=' | '<' | '<=' | '>' | '>=' ) add )?
**   add      := mul ( ( '+' | '-' ) mul )*
**   mul      := unary ( ( '*' | '/' | '%' ) unary )*
**   unary    := '-' unary | primary
**   primary  := number | identifier | identifier '(' args? ')' | '(' expr ')'
**   args     := expr ( ',' expr )*
**
** The single statement produced is `RETURN <expr>;`, matching the call
** signature `double f(const double *argv)`.
*/
#include "tdb_jit_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
  TK_EOF, TK_BAD, TK_NUM, TK_ID,
  TK_LP, TK_RP, TK_COMMA,
  TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
  TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
  TK_AND, TK_OR, TK_NOT
} tk_kind;

typedef struct {
  tk_kind     kind;
  const char *z; int n;
  double      num;
} tok;

typedef struct {
  tdb_arena  *a;
  const char *src;
  size_t      len, pos;
  tok         cur;
  int         err;
  char        msg[160];
  char      **names;
  int         nslots, cap;
  int         nparams;
} P;

static void perr(P *p, const char *m) {
  if (p->err) return;
  p->err = 1;
  snprintf(p->msg, sizeof(p->msg), "SQL: %s near \"%.*s\"", m,
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?");
}

static int at(P *p, size_t off) {
  return (p->pos + off < p->len) ? (unsigned char)p->src[p->pos + off] : 0;
}

static void lex(P *p) {
  tok *t = &p->cur;
  memset(t, 0, sizeof(*t));
  while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
  /* SQL line comment: -- ... */
  if (at(p, 0) == '-' && at(p, 1) == '-') {
    while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
    lex(p); return;
  }
  t->z = p->src + p->pos;
  if (p->pos >= p->len) { t->kind = TK_EOF; return; }
  int c = at(p, 0);
  if (isdigit(c) || (c == '.' && isdigit(at(p, 1)))) {
    char *end;
    t->num = strtod(p->src + p->pos, &end);
    t->n = (int)(end - (p->src + p->pos));
    p->pos += (size_t)t->n;
    t->kind = TK_NUM;
    return;
  }
  if (isalpha(c) || c == '_') {
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_'))
      p->pos++;
    t->n = (int)(p->pos - s);
    if (t->n == 3 && !strncasecmp(t->z, "AND", 3)) t->kind = TK_AND;
    else if (t->n == 2 && !strncasecmp(t->z, "OR", 2)) t->kind = TK_OR;
    else if (t->n == 3 && !strncasecmp(t->z, "NOT", 3)) t->kind = TK_NOT;
    else t->kind = TK_ID;
    return;
  }
  switch (c) {
    case '(': p->pos++; t->kind = TK_LP; t->n = 1; return;
    case ')': p->pos++; t->kind = TK_RP; t->n = 1; return;
    case ',': p->pos++; t->kind = TK_COMMA; t->n = 1; return;
    case '+': p->pos++; t->kind = TK_PLUS; t->n = 1; return;
    case '-': p->pos++; t->kind = TK_MINUS; t->n = 1; return;
    case '*': p->pos++; t->kind = TK_STAR; t->n = 1; return;
    case '/': p->pos++; t->kind = TK_SLASH; t->n = 1; return;
    case '%': p->pos++; t->kind = TK_PCT; t->n = 1; return;
    case '=':
      p->pos++; if (at(p, 0) == '=') { p->pos++; t->n = 2; } else t->n = 1;
      t->kind = TK_EQ; return;
    case '<':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->kind = TK_LE; t->n = 2; }
      else if (at(p, 0) == '>') { p->pos++; t->kind = TK_NE; t->n = 2; }
      else { t->kind = TK_LT; t->n = 1; }
      return;
    case '>':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->kind = TK_GE; t->n = 2; }
      else { t->kind = TK_GT; t->n = 1; }
      return;
    case '!':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->kind = TK_NE; t->n = 2; }
      else { t->kind = TK_BAD; t->n = 1; }
      return;
  }
  t->kind = TK_BAD; t->n = 1; p->pos++;
}

static int eat(P *p, tk_kind k) {
  if (p->cur.kind == k) { lex(p); return 1; }
  return 0;
}

static int slot_for(P *p, const char *name, int n) {
  for (int i = 0; i < p->nslots; i++)
    if ((int)strlen(p->names[i]) == n && !strncasecmp(p->names[i], name, (size_t)n))
      return i;
  if (p->nslots == p->cap) {
    int nc = p->cap ? p->cap * 2 : 8;
    char **g = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)nc);
    for (int i = 0; i < p->nslots; i++) g[i] = p->names[i];
    p->names = g; p->cap = nc;
  }
  p->names[p->nslots] = tdb_arena_strndup(p->a, name, (size_t)n);
  return p->nslots++;
}

static pl_expr *mke(P *p, pl_ekind k) {
  pl_expr *e = (pl_expr *)tdb_arena_alloc(p->a, sizeof(*e));
  memset(e, 0, sizeof(*e));
  e->kind = k;
  return e;
}

static pl_expr *expr(P *p);

static pl_expr *primary(P *p) {
  tok t = p->cur;
  if (t.kind == TK_NUM) {
    lex(p);
    pl_expr *e = mke(p, PL_E_NUM);
    e->num = t.num; e->is_int = (t.num == (double)(long long)t.num);
    return e;
  }
  if (t.kind == TK_ID) {
    lex(p);
    if (p->cur.kind == TK_LP) {
      lex(p);
      pl_expr *e = mke(p, PL_E_CALL);
      e->fname = tdb_arena_strndup(p->a, t.z, (size_t)t.n);
      int cap = 0;
      if (p->cur.kind != TK_RP) {
        for (;;) {
          if (e->nargs == cap) {
            int nc = cap ? cap * 2 : 4;
            pl_expr **g = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * (size_t)nc);
            for (int i = 0; i < e->nargs; i++) g[i] = e->args[i];
            e->args = g; cap = nc;
          }
          e->args[e->nargs++] = expr(p);
          if (!eat(p, TK_COMMA)) break;
        }
      }
      if (!eat(p, TK_RP)) perr(p, "expected ')'");
      return e;
    }
    pl_expr *e = mke(p, PL_E_VAR);
    e->slot = slot_for(p, t.z, t.n);
    return e;
  }
  if (t.kind == TK_LP) {
    lex(p);
    pl_expr *e = expr(p);
    if (!eat(p, TK_RP)) perr(p, "expected ')'");
    return e;
  }
  if (t.kind == TK_MINUS) {
    lex(p);
    pl_expr *e = mke(p, PL_E_UNARY);
    e->op = PL_OP_NEG; e->l = primary(p);
    return e;
  }
  perr(p, "expected expression");
  return mke(p, PL_E_NUM);
}

static pl_expr *mul(P *p) {
  pl_expr *l = primary(p);
  while (p->cur.kind == TK_STAR || p->cur.kind == TK_SLASH || p->cur.kind == TK_PCT) {
    pl_op op = p->cur.kind == TK_STAR  ? PL_OP_MUL
             : p->cur.kind == TK_SLASH ? PL_OP_DIV
             : PL_OP_MOD;
    lex(p);
    pl_expr *r = primary(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *add(P *p) {
  pl_expr *l = mul(p);
  while (p->cur.kind == TK_PLUS || p->cur.kind == TK_MINUS) {
    pl_op op = p->cur.kind == TK_PLUS ? PL_OP_ADD : PL_OP_SUB;
    lex(p);
    pl_expr *r = mul(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *rel(P *p) {
  pl_expr *l = add(p);
  tk_kind k = p->cur.kind;
  if (k == TK_EQ || k == TK_NE || k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE) {
    pl_op op = k == TK_EQ ? PL_OP_EQ : k == TK_NE ? PL_OP_NE
             : k == TK_LT ? PL_OP_LT : k == TK_LE ? PL_OP_LE
             : k == TK_GT ? PL_OP_GT : PL_OP_GE;
    lex(p);
    pl_expr *r = add(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    return e;
  }
  return l;
}

static pl_expr *not_(P *p) {
  if (eat(p, TK_NOT)) {
    pl_expr *e = mke(p, PL_E_UNARY); e->op = PL_OP_NOT; e->l = not_(p);
    return e;
  }
  return rel(p);
}

static pl_expr *and_(P *p) {
  pl_expr *l = not_(p);
  while (eat(p, TK_AND)) {
    pl_expr *r = not_(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_AND; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *or_(P *p) {
  pl_expr *l = and_(p);
  while (eat(p, TK_OR)) {
    pl_expr *r = and_(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_OR; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *expr(P *p) { return or_(p); }

int tdb_jit_sql_parse(const char *src, const char *const *params, int nparams,
                      tdb_plsql_proc **out, char *errbuf, int errlen) {
  if (!src || !out) return TDB_MISUSE;
  tdb_arena *a = tdb_arena_new(4096);
  if (!a) return TDB_NOMEM;

  P p; memset(&p, 0, sizeof(p));
  p.a = a; p.src = src; p.len = strlen(src);
  for (int i = 0; i < nparams; i++)
    slot_for(&p, params[i], (int)strlen(params[i]));
  p.nparams = nparams;

  lex(&p);
  pl_expr *e = expr(&p);
  if (!p.err && p.cur.kind != TK_EOF) perr(&p, "trailing tokens");
  if (p.err) {
    if (errbuf && errlen) snprintf(errbuf, (size_t)errlen, "%s", p.msg);
    tdb_arena_free(a);
    return TDB_ERROR;
  }
  pl_stmt *ret = (pl_stmt *)tdb_arena_alloc(a, sizeof(*ret));
  memset(ret, 0, sizeof(*ret));
  ret->kind = PL_S_RETURN; ret->e1 = e;
  pl_stmt **stmts = (pl_stmt **)tdb_arena_alloc(a, sizeof(pl_stmt *));
  stmts[0] = ret;

  tdb_plsql_proc *proc = (tdb_plsql_proc *)tdb_arena_alloc(a, sizeof(*proc));
  memset(proc, 0, sizeof(*proc));
  proc->a = a;
  proc->nslots = p.nslots; proc->slotnames = p.names;
  proc->nparams = p.nparams;
  proc->stmts = stmts; proc->nstmt = 1;
  *out = proc;
  return TDB_OK;
}
