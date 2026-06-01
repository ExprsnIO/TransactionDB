/*
** tdb_jit_xpath.c — JIT front end for the numeric subset of XPath (1.0 and
** the value-comparison portion of 2.0/3.1).
**
** Grammar (numeric only — path expressions are not supported in the JIT
** because they would require a live XML tree, which TransactionDB does not
** model at the storage layer today):
**
**   Expr    := OrExpr
**   OrExpr  := AndExpr ('or' AndExpr)*
**   AndExpr := CmpExpr ('and' CmpExpr)*
**   CmpExpr := AddExpr (('=' | '!=' | '<' | '<=' | '>' | '>=' |
**                        'eq' | 'ne' | 'lt' | 'le' | 'gt' | 'ge') AddExpr)?
**   AddExpr := MulExpr (('+' | '-') MulExpr)*
**   MulExpr := UnaryExpr (('*' | 'div' | 'idiv' | 'mod') UnaryExpr)*
**   UnaryExpr := '-' UnaryExpr | 'not' UnaryExpr | Primary
**   Primary   := Number | '$' Name | Name '(' Args? ')' | '(' Expr ')'
**
** Notes:
**   * `div` and `idiv` both lower to fdiv (no integer-division semantics).
**   * `=` (general comparison) is treated as numeric equality.
**   * Function names go through the same intrinsic table as SQL/Lua, so
**     ceiling()/floor()/abs()/round()/min()/max()/number()/power() work.
*/
#include "tdb_jit_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
  X_EOF, X_BAD, X_NUM, X_NAME, X_VAR,
  X_LP, X_RP, X_COMMA,
  X_PLUS, X_MINUS, X_STAR, X_SLASH,
  X_EQ, X_NE, X_LT, X_LE, X_GT, X_GE,
  X_AND, X_OR, X_NOT, X_DIV, X_IDIV, X_MOD
} xk;

typedef struct { xk k; const char *z; int n; double num; } xt;

typedef struct {
  tdb_arena  *a;
  const char *src;
  size_t      len, pos;
  xt          cur;
  int         err;
  char        msg[160];
  char      **names; int nslots, cap;
  int         nparams;
  int         last_was_name;   /* enables operator-keyword disambiguation   */
} P;

static void perr(P *p, const char *m) {
  if (p->err) return;
  p->err = 1;
  snprintf(p->msg, sizeof(p->msg), "XPath: %s near \"%.*s\"", m,
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?");
}

static int at(P *p, size_t off) {
  return (p->pos + off < p->len) ? (unsigned char)p->src[p->pos + off] : 0;
}

/* Operator keywords ('and', 'or', 'div', ...) are also valid NCNames in
** XPath; the disambiguation rule (XPath 2.0 §3.7) is that they are only
** recognized as operators when they immediately follow another operator
** or open-paren. We carry the "last token was a name/number/close-paren"
** state in `last_was_name`. */
static int can_be_op(P *p) { return p->last_was_name; }

static void lex(P *p) {
  xt *t = &p->cur;
  memset(t, 0, sizeof(*t));
  /* XPath ignores XQuery comments (: ... :); skip them too. */
  for (;;) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
    if (at(p, 0) == '(' && at(p, 1) == ':') {
      int depth = 1; p->pos += 2;
      while (p->pos + 1 < p->len && depth) {
        if (p->src[p->pos] == '(' && p->src[p->pos + 1] == ':') { depth++; p->pos += 2; }
        else if (p->src[p->pos] == ':' && p->src[p->pos + 1] == ')') { depth--; p->pos += 2; }
        else p->pos++;
      }
      continue;
    }
    break;
  }
  t->z = p->src + p->pos;
  if (p->pos >= p->len) { t->k = X_EOF; return; }
  int c = at(p, 0);
  if (isdigit(c) || (c == '.' && isdigit(at(p, 1)))) {
    char *end;
    t->num = strtod(p->src + p->pos, &end);
    t->n = (int)(end - (p->src + p->pos));
    p->pos += (size_t)t->n;
    t->k = X_NUM;
    return;
  }
  if (c == '$') {
    p->pos++;
    if (!isalpha(at(p, 0)) && at(p, 0) != '_') { t->k = X_BAD; t->n = 1; return; }
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) ||
            p->src[p->pos] == '_' || p->src[p->pos] == '-'))
      p->pos++;
    t->z = p->src + s;
    t->n = (int)(p->pos - s);
    t->k = X_VAR;
    return;
  }
  if (isalpha(c) || c == '_') {
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) ||
            p->src[p->pos] == '_' || p->src[p->pos] == '-'))
      p->pos++;
    t->n = (int)(p->pos - s);
    int op_ok = can_be_op(p);
    if (op_ok) {
      if (t->n == 3 && !strncmp(t->z, "and", 3)) { t->k = X_AND; return; }
      if (t->n == 2 && !strncmp(t->z, "or",  2)) { t->k = X_OR;  return; }
      if (t->n == 3 && !strncmp(t->z, "div", 3)) { t->k = X_DIV; return; }
      if (t->n == 4 && !strncmp(t->z, "idiv",4)) { t->k = X_IDIV;return; }
      if (t->n == 3 && !strncmp(t->z, "mod", 3)) { t->k = X_MOD; return; }
      if (t->n == 2 && (!strncmp(t->z, "eq", 2) || !strncmp(t->z, "ne", 2) ||
                        !strncmp(t->z, "lt", 2) || !strncmp(t->z, "le", 2) ||
                        !strncmp(t->z, "gt", 2) || !strncmp(t->z, "ge", 2))) {
        t->k = (!strncmp(t->z, "eq", 2)) ? X_EQ :
               (!strncmp(t->z, "ne", 2)) ? X_NE :
               (!strncmp(t->z, "lt", 2)) ? X_LT :
               (!strncmp(t->z, "le", 2)) ? X_LE :
               (!strncmp(t->z, "gt", 2)) ? X_GT : X_GE;
        return;
      }
    }
    if (t->n == 3 && !strncmp(t->z, "not", 3) && at(p, 0) == '(') {
      /* `not(...)` — treat as the not operator only when followed by '('  */
      t->k = X_NOT; return;
    }
    t->k = X_NAME;
    return;
  }
  switch (c) {
    case '(': p->pos++; t->k = X_LP; t->n = 1; return;
    case ')': p->pos++; t->k = X_RP; t->n = 1; return;
    case ',': p->pos++; t->k = X_COMMA; t->n = 1; return;
    case '+': p->pos++; t->k = X_PLUS; t->n = 1; return;
    case '-': p->pos++; t->k = X_MINUS; t->n = 1; return;
    case '*': p->pos++; t->k = X_STAR; t->n = 1; return;
    case '/': p->pos++; t->k = X_SLASH; t->n = 1; return;  /* used as div op only after operand */
    case '=': p->pos++; t->k = X_EQ; t->n = 1; return;
    case '!':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = X_NE; t->n = 2; return; }
      t->k = X_BAD; t->n = 1; return;
    case '<':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = X_LE; t->n = 2; return; }
      t->k = X_LT; t->n = 1; return;
    case '>':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = X_GE; t->n = 2; return; }
      t->k = X_GT; t->n = 1; return;
  }
  t->k = X_BAD; t->n = 1; p->pos++;
}

/* Wrap lex() so we can maintain the disambiguation context. After we
** consume a name/number/$var/close-paren the next bare keyword can be an
** operator; immediately after an operator/open-paren/start-of-input it
** cannot. */
static void advance(P *p) {
  xk prev = p->cur.k;
  p->last_was_name = (prev == X_NUM || prev == X_VAR || prev == X_NAME ||
                      prev == X_RP);
  lex(p);
}

static int eat(P *p, xk k) {
  if (p->cur.k == k) { advance(p); return 1; }
  return 0;
}

static int slot_for(P *p, const char *name, int n) {
  for (int i = 0; i < p->nslots; i++)
    if ((int)strlen(p->names[i]) == n && !strncmp(p->names[i], name, (size_t)n))
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
  xt t = p->cur;
  if (t.k == X_NUM) { advance(p);
    pl_expr *e = mke(p, PL_E_NUM); e->num = t.num; return e; }
  if (t.k == X_VAR) { advance(p);
    pl_expr *e = mke(p, PL_E_VAR); e->slot = slot_for(p, t.z, t.n); return e; }
  if (t.k == X_LP) { advance(p);
    pl_expr *e = expr(p);
    if (!eat(p, X_RP)) perr(p, "expected ')'");
    return e; }
  if (t.k == X_NAME || t.k == X_NOT) {
    advance(p);
    if (p->cur.k != X_LP) {
      /* Bare names that are not function calls are unsupported in this
      ** numeric subset (they would be element-name node tests). */
      perr(p, "bare name unsupported (use $var or fn(...))");
      return mke(p, PL_E_NUM);
    }
    advance(p);
    pl_expr *e = mke(p, PL_E_CALL);
    e->fname = tdb_arena_strndup(p->a, t.z, (size_t)t.n);
    int cap = 0;
    if (p->cur.k != X_RP) {
      for (;;) {
        if (e->nargs == cap) {
          int nc = cap ? cap * 2 : 4;
          pl_expr **g = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * (size_t)nc);
          for (int i = 0; i < e->nargs; i++) g[i] = e->args[i];
          e->args = g; cap = nc;
        }
        e->args[e->nargs++] = expr(p);
        if (!eat(p, X_COMMA)) break;
      }
    }
    if (!eat(p, X_RP)) perr(p, "expected ')'");
    return e;
  }
  perr(p, "expected expression");
  return mke(p, PL_E_NUM);
}

static pl_expr *unary(P *p) {
  if (eat(p, X_MINUS)) {
    pl_expr *e = mke(p, PL_E_UNARY); e->op = PL_OP_NEG; e->l = unary(p);
    return e;
  }
  return primary(p);
}

static pl_expr *mul(P *p) {
  pl_expr *l = unary(p);
  while (p->cur.k == X_STAR || p->cur.k == X_DIV ||
         p->cur.k == X_IDIV || p->cur.k == X_MOD) {
    pl_op op = (p->cur.k == X_STAR) ? PL_OP_MUL
             : (p->cur.k == X_MOD)  ? PL_OP_MOD
             : PL_OP_DIV;  /* div / idiv both → fdiv */
    advance(p);
    pl_expr *r = unary(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *addx(P *p) {
  pl_expr *l = mul(p);
  while (p->cur.k == X_PLUS || p->cur.k == X_MINUS) {
    pl_op op = p->cur.k == X_PLUS ? PL_OP_ADD : PL_OP_SUB;
    advance(p);
    pl_expr *r = mul(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *cmp(P *p) {
  pl_expr *l = addx(p);
  xk k = p->cur.k;
  if (k == X_EQ || k == X_NE || k == X_LT || k == X_LE || k == X_GT || k == X_GE) {
    pl_op op = k == X_EQ ? PL_OP_EQ : k == X_NE ? PL_OP_NE
             : k == X_LT ? PL_OP_LT : k == X_LE ? PL_OP_LE
             : k == X_GT ? PL_OP_GT : PL_OP_GE;
    advance(p);
    pl_expr *r = addx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    return e;
  }
  return l;
}

static pl_expr *andx(P *p) {
  pl_expr *l = cmp(p);
  while (eat(p, X_AND)) {
    pl_expr *r = cmp(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_AND; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *orx(P *p) {
  pl_expr *l = andx(p);
  while (eat(p, X_OR)) {
    pl_expr *r = andx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_OR; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *expr(P *p) { return orx(p); }

int tdb_jit_xpath_parse(const char *src, const char *const *params, int nparams,
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
  if (!p.err && p.cur.k != X_EOF) perr(&p, "trailing tokens");
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
