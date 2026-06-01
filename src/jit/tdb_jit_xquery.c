/*
** tdb_jit_xquery.c — JIT front end for a numeric FLWOR subset of XQuery.
**
** Grammar:
**
**   FLWOR := ('let' '$' Name ':=' Expr)* 'return' Expr
**          | Expr                                     ; bare expression
**
**   Expr  := OrExpr                                   ; same as XPath
**
** The expression sub-language is identical to the XPath front end (numeric
** literals, $variables, arithmetic, comparisons, the and/or/not/div/mod
** operator keywords and function calls). The only additions are:
**
**   * Multiple `let $name := expr` bindings before `return`. Each binding
**     allocates a fresh slot; subsequent expressions may reference it.
**   * A required `return expr` that supplies the IR's RETURN statement.
**
** Bindings desugar to PL_S_ASSIGN statements followed by a final
** PL_S_RETURN, so the shared IR emitter handles them with no special case.
*/
#include "tdb_jit_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
  Q_EOF, Q_BAD, Q_NUM, Q_NAME, Q_VAR,
  Q_LP, Q_RP, Q_COMMA, Q_ASSIGN,
  Q_PLUS, Q_MINUS, Q_STAR, Q_SLASH,
  Q_EQ, Q_NE, Q_LT, Q_LE, Q_GT, Q_GE,
  Q_AND, Q_OR, Q_NOT, Q_DIV, Q_IDIV, Q_MOD,
  Q_LET, Q_RETURN
} qk;

typedef struct { qk k; const char *z; int n; double num; } qt;

typedef struct {
  tdb_arena  *a;
  const char *src;
  size_t      len, pos;
  qt          cur;
  int         err;
  char        msg[160];
  char      **names; int nslots, cap;
  int         nparams;
  int         last_was_name;
} P;

static void perr(P *p, const char *m) {
  if (p->err) return;
  p->err = 1;
  snprintf(p->msg, sizeof(p->msg), "XQuery: %s near \"%.*s\"", m,
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?");
}

static int at(P *p, size_t off) {
  return (p->pos + off < p->len) ? (unsigned char)p->src[p->pos + off] : 0;
}

static int can_be_op(P *p) { return p->last_was_name; }

static void lex(P *p) {
  qt *t = &p->cur;
  memset(t, 0, sizeof(*t));
  for (;;) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
    if (at(p, 0) == '(' && at(p, 1) == ':') {  /* XQuery comment */
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
  if (p->pos >= p->len) { t->k = Q_EOF; return; }
  int c = at(p, 0);
  if (isdigit(c) || (c == '.' && isdigit(at(p, 1)))) {
    char *end;
    t->num = strtod(p->src + p->pos, &end);
    t->n = (int)(end - (p->src + p->pos));
    p->pos += (size_t)t->n;
    t->k = Q_NUM;
    return;
  }
  if (c == '$') {
    p->pos++;
    if (!isalpha(at(p, 0)) && at(p, 0) != '_') { t->k = Q_BAD; t->n = 1; return; }
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) ||
            p->src[p->pos] == '_' || p->src[p->pos] == '-'))
      p->pos++;
    t->z = p->src + s;
    t->n = (int)(p->pos - s);
    t->k = Q_VAR;
    return;
  }
  if (isalpha(c) || c == '_') {
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) ||
            p->src[p->pos] == '_' || p->src[p->pos] == '-'))
      p->pos++;
    t->n = (int)(p->pos - s);
    /* Clause keywords (always recognized — they are statement-level). */
    if (t->n == 3 && !strncmp(t->z, "let",    3)) { t->k = Q_LET;    return; }
    if (t->n == 6 && !strncmp(t->z, "return", 6)) { t->k = Q_RETURN; return; }
    int op_ok = can_be_op(p);
    if (op_ok) {
      if (t->n == 3 && !strncmp(t->z, "and", 3)) { t->k = Q_AND; return; }
      if (t->n == 2 && !strncmp(t->z, "or",  2)) { t->k = Q_OR;  return; }
      if (t->n == 3 && !strncmp(t->z, "div", 3)) { t->k = Q_DIV; return; }
      if (t->n == 4 && !strncmp(t->z, "idiv",4)) { t->k = Q_IDIV;return; }
      if (t->n == 3 && !strncmp(t->z, "mod", 3)) { t->k = Q_MOD; return; }
      if (t->n == 2 && (!strncmp(t->z, "eq", 2) || !strncmp(t->z, "ne", 2) ||
                        !strncmp(t->z, "lt", 2) || !strncmp(t->z, "le", 2) ||
                        !strncmp(t->z, "gt", 2) || !strncmp(t->z, "ge", 2))) {
        t->k = (!strncmp(t->z, "eq", 2)) ? Q_EQ :
               (!strncmp(t->z, "ne", 2)) ? Q_NE :
               (!strncmp(t->z, "lt", 2)) ? Q_LT :
               (!strncmp(t->z, "le", 2)) ? Q_LE :
               (!strncmp(t->z, "gt", 2)) ? Q_GT : Q_GE;
        return;
      }
    }
    if (t->n == 3 && !strncmp(t->z, "not", 3) && at(p, 0) == '(') {
      t->k = Q_NOT; return;
    }
    t->k = Q_NAME;
    return;
  }
  switch (c) {
    case '(': p->pos++; t->k = Q_LP; t->n = 1; return;
    case ')': p->pos++; t->k = Q_RP; t->n = 1; return;
    case ',': p->pos++; t->k = Q_COMMA; t->n = 1; return;
    case '+': p->pos++; t->k = Q_PLUS; t->n = 1; return;
    case '-': p->pos++; t->k = Q_MINUS; t->n = 1; return;
    case '*': p->pos++; t->k = Q_STAR; t->n = 1; return;
    case '/': p->pos++; t->k = Q_SLASH; t->n = 1; return;
    case ':':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = Q_ASSIGN; t->n = 2; return; }
      t->k = Q_BAD; t->n = 1; return;
    case '=': p->pos++; t->k = Q_EQ; t->n = 1; return;
    case '!':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = Q_NE; t->n = 2; return; }
      t->k = Q_BAD; t->n = 1; return;
    case '<':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = Q_LE; t->n = 2; return; }
      t->k = Q_LT; t->n = 1; return;
    case '>':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = Q_GE; t->n = 2; return; }
      t->k = Q_GT; t->n = 1; return;
  }
  t->k = Q_BAD; t->n = 1; p->pos++;
}

static void advance(P *p) {
  qk prev = p->cur.k;
  p->last_was_name = (prev == Q_NUM || prev == Q_VAR || prev == Q_NAME ||
                      prev == Q_RP);
  lex(p);
}

static int eat(P *p, qk k) {
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
static pl_stmt *mks(P *p, pl_skind k) {
  pl_stmt *s = (pl_stmt *)tdb_arena_alloc(p->a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->kind = k;
  return s;
}

static pl_expr *expr(P *p);

static pl_expr *primary(P *p) {
  qt t = p->cur;
  if (t.k == Q_NUM) { advance(p);
    pl_expr *e = mke(p, PL_E_NUM); e->num = t.num; return e; }
  if (t.k == Q_VAR) { advance(p);
    pl_expr *e = mke(p, PL_E_VAR); e->slot = slot_for(p, t.z, t.n); return e; }
  if (t.k == Q_LP) { advance(p);
    pl_expr *e = expr(p);
    if (!eat(p, Q_RP)) perr(p, "expected ')'");
    return e; }
  if (t.k == Q_NAME || t.k == Q_NOT) {
    advance(p);
    if (p->cur.k != Q_LP) { perr(p, "bare name unsupported (use $var or fn(...))"); return mke(p, PL_E_NUM); }
    advance(p);
    pl_expr *e = mke(p, PL_E_CALL);
    e->fname = tdb_arena_strndup(p->a, t.z, (size_t)t.n);
    int cap = 0;
    if (p->cur.k != Q_RP) {
      for (;;) {
        if (e->nargs == cap) {
          int nc = cap ? cap * 2 : 4;
          pl_expr **g = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * (size_t)nc);
          for (int i = 0; i < e->nargs; i++) g[i] = e->args[i];
          e->args = g; cap = nc;
        }
        e->args[e->nargs++] = expr(p);
        if (!eat(p, Q_COMMA)) break;
      }
    }
    if (!eat(p, Q_RP)) perr(p, "expected ')'");
    return e;
  }
  perr(p, "expected expression");
  return mke(p, PL_E_NUM);
}

static pl_expr *unary(P *p) {
  if (eat(p, Q_MINUS)) {
    pl_expr *e = mke(p, PL_E_UNARY); e->op = PL_OP_NEG; e->l = unary(p);
    return e;
  }
  return primary(p);
}

static pl_expr *mul(P *p) {
  pl_expr *l = unary(p);
  while (p->cur.k == Q_STAR || p->cur.k == Q_DIV ||
         p->cur.k == Q_IDIV || p->cur.k == Q_MOD) {
    pl_op op = (p->cur.k == Q_STAR) ? PL_OP_MUL
             : (p->cur.k == Q_MOD)  ? PL_OP_MOD : PL_OP_DIV;
    advance(p);
    pl_expr *r = unary(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *addx(P *p) {
  pl_expr *l = mul(p);
  while (p->cur.k == Q_PLUS || p->cur.k == Q_MINUS) {
    pl_op op = p->cur.k == Q_PLUS ? PL_OP_ADD : PL_OP_SUB;
    advance(p);
    pl_expr *r = mul(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *cmp(P *p) {
  pl_expr *l = addx(p);
  qk k = p->cur.k;
  if (k == Q_EQ || k == Q_NE || k == Q_LT || k == Q_LE || k == Q_GT || k == Q_GE) {
    pl_op op = k == Q_EQ ? PL_OP_EQ : k == Q_NE ? PL_OP_NE
             : k == Q_LT ? PL_OP_LT : k == Q_LE ? PL_OP_LE
             : k == Q_GT ? PL_OP_GT : PL_OP_GE;
    advance(p);
    pl_expr *r = addx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    return e;
  }
  return l;
}

static pl_expr *andx(P *p) {
  pl_expr *l = cmp(p);
  while (eat(p, Q_AND)) {
    pl_expr *r = cmp(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_AND; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *orx(P *p) {
  pl_expr *l = andx(p);
  while (eat(p, Q_OR)) {
    pl_expr *r = andx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_OR; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *expr(P *p) { return orx(p); }

int tdb_jit_xquery_parse(const char *src, const char *const *params, int nparams,
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

  /* Collect zero or more `let $name := expr` then a single `return expr`. */
  pl_stmt **stmts = NULL; int nstmt = 0, cap = 0;
  while (eat(&p, Q_LET)) {
    if (p.cur.k != Q_VAR) { perr(&p, "expected $variable after 'let'"); break; }
    qt name = p.cur;
    advance(&p);
    if (!eat(&p, Q_ASSIGN)) { perr(&p, "expected ':=' after let binding"); break; }
    pl_expr *e = expr(&p);
    int slot = slot_for(&p, name.z, name.n);
    pl_stmt *s = mks(&p, PL_S_ASSIGN);
    s->slot = slot; s->e1 = e;
    if (nstmt == cap) {
      int nc = cap ? cap * 2 : 4;
      pl_stmt **g = (pl_stmt **)tdb_arena_alloc(a, sizeof(pl_stmt *) * (size_t)nc);
      for (int i = 0; i < nstmt; i++) g[i] = stmts[i];
      stmts = g; cap = nc;
    }
    stmts[nstmt++] = s;
  }

  /* The trailing expression — either after `return` or implicit. */
  pl_expr *re;
  if (eat(&p, Q_RETURN)) re = expr(&p);
  else if (nstmt == 0)   re = expr(&p);
  else { perr(&p, "expected 'return' after let bindings"); re = mke(&p, PL_E_NUM); }

  if (!p.err && p.cur.k != Q_EOF) perr(&p, "trailing tokens");
  if (p.err) {
    if (errbuf && errlen) snprintf(errbuf, (size_t)errlen, "%s", p.msg);
    tdb_arena_free(a);
    return TDB_ERROR;
  }

  pl_stmt *ret = mks(&p, PL_S_RETURN); ret->e1 = re;
  if (nstmt == cap) {
    int nc = cap + 1;
    pl_stmt **g = (pl_stmt **)tdb_arena_alloc(a, sizeof(pl_stmt *) * (size_t)nc);
    for (int i = 0; i < nstmt; i++) g[i] = stmts[i];
    stmts = g;
  }
  stmts[nstmt++] = ret;

  tdb_plsql_proc *proc = (tdb_plsql_proc *)tdb_arena_alloc(a, sizeof(*proc));
  memset(proc, 0, sizeof(*proc));
  proc->a = a;
  proc->nslots = p.nslots; proc->slotnames = p.names;
  proc->nparams = p.nparams;
  proc->stmts = stmts; proc->nstmt = nstmt;
  *out = proc;
  return TDB_OK;
}
