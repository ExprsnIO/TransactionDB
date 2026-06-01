/*
** tdb_jit_lua.c — JIT front end for a numeric subset of Lua 5.x.
**
** Supported (numeric-only) constructs:
**
**   stmt   := 'local' Name '=' expr
**           | Name '=' expr
**           | 'if' expr 'then' block ('elseif' expr 'then' block)* ('else' block)? 'end'
**           | 'while' expr 'do' block 'end'
**           | 'for' Name '=' expr ',' expr 'do' block 'end'
**           | 'return' expr
**           | ';'
**
**   expr   := orx
**   orx    := andx ('or' andx)*
**   andx   := relx ('and' relx)*
**   relx   := addx (('<' | '<=' | '>' | '>=' | '==' | '~=') addx)?
**   addx   := mulx (('+' | '-') mulx)*
**   mulx   := powx (('*' | '/' | '%') powx)*
**   powx   := unaryx ('^' powx)?              ; right-associative
**   unaryx := ('-' | 'not') unaryx | primary
**   primary:= Number
**           | Name ('.' Name)? ('(' args? ')')?       ; lib.fn(args), x, x.y
**           | '(' expr ')'
**
** `^` lowers to llvm.pow.f64. `math.<name>` and bare `<name>` look up the
** same intrinsic table the SQL/XPath/XQuery front ends use. The IR call
** signature is the standard `double f(const double *argv)`.
*/
#include "tdb_jit_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
  T_EOF, T_BAD, T_NUM, T_ID,
  T_LP, T_RP, T_COMMA, T_DOT, T_SEMI, T_ASSIGN,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT, T_CARET,
  T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
  T_AND, T_OR, T_NOT,
  T_LOCAL, T_IF, T_THEN, T_ELSEIF, T_ELSE, T_END,
  T_WHILE, T_DO, T_FOR, T_RETURN
} tk;

typedef struct { tk k; const char *z; int n; double num; } tok;

typedef struct {
  tdb_arena  *a;
  const char *src;
  size_t      len, pos;
  tok         cur;
  int         err;
  char        msg[160];
  char      **names; int nslots, cap;
  int         nparams;
} P;

static void perr(P *p, const char *m) {
  if (p->err) return;
  p->err = 1;
  snprintf(p->msg, sizeof(p->msg), "Lua: %s near \"%.*s\"", m,
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?");
}

static int at(P *p, size_t off) {
  return (p->pos + off < p->len) ? (unsigned char)p->src[p->pos + off] : 0;
}

static tk kw_for(const char *z, int n) {
  if (n == 5 && !strncmp(z, "local",  5)) return T_LOCAL;
  if (n == 2 && !strncmp(z, "if",     2)) return T_IF;
  if (n == 4 && !strncmp(z, "then",   4)) return T_THEN;
  if (n == 6 && !strncmp(z, "elseif", 6)) return T_ELSEIF;
  if (n == 4 && !strncmp(z, "else",   4)) return T_ELSE;
  if (n == 3 && !strncmp(z, "end",    3)) return T_END;
  if (n == 5 && !strncmp(z, "while",  5)) return T_WHILE;
  if (n == 2 && !strncmp(z, "do",     2)) return T_DO;
  if (n == 3 && !strncmp(z, "for",    3)) return T_FOR;
  if (n == 6 && !strncmp(z, "return", 6)) return T_RETURN;
  if (n == 3 && !strncmp(z, "and",    3)) return T_AND;
  if (n == 2 && !strncmp(z, "or",     2)) return T_OR;
  if (n == 3 && !strncmp(z, "not",    3)) return T_NOT;
  return T_ID;
}

static void lex(P *p) {
  tok *t = &p->cur;
  memset(t, 0, sizeof(*t));
  for (;;) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
    /* Lua line comment: -- ...; block comment: --[[ ... ]] */
    if (at(p, 0) == '-' && at(p, 1) == '-') {
      if (at(p, 2) == '[' && at(p, 3) == '[') {
        p->pos += 4;
        while (p->pos + 1 < p->len &&
               !(p->src[p->pos] == ']' && p->src[p->pos + 1] == ']')) p->pos++;
        p->pos = (p->pos + 1 < p->len) ? p->pos + 2 : p->len;
      } else {
        while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
      }
      continue;
    }
    break;
  }
  t->z = p->src + p->pos;
  if (p->pos >= p->len) { t->k = T_EOF; return; }
  int c = at(p, 0);
  if (isdigit(c) || (c == '.' && isdigit(at(p, 1)))) {
    char *end;
    t->num = strtod(p->src + p->pos, &end);
    t->n = (int)(end - (p->src + p->pos));
    p->pos += (size_t)t->n;
    t->k = T_NUM;
    return;
  }
  if (isalpha(c) || c == '_') {
    size_t s = p->pos;
    while (p->pos < p->len &&
           (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_'))
      p->pos++;
    t->n = (int)(p->pos - s);
    t->k = kw_for(t->z, t->n);
    return;
  }
  switch (c) {
    case '(': p->pos++; t->k = T_LP; t->n = 1; return;
    case ')': p->pos++; t->k = T_RP; t->n = 1; return;
    case ',': p->pos++; t->k = T_COMMA; t->n = 1; return;
    case '.': p->pos++; t->k = T_DOT; t->n = 1; return;
    case ';': p->pos++; t->k = T_SEMI; t->n = 1; return;
    case '+': p->pos++; t->k = T_PLUS; t->n = 1; return;
    case '-': p->pos++; t->k = T_MINUS; t->n = 1; return;
    case '*': p->pos++; t->k = T_STAR; t->n = 1; return;
    case '/': p->pos++; t->k = T_SLASH; t->n = 1; return;
    case '%': p->pos++; t->k = T_PCT; t->n = 1; return;
    case '^': p->pos++; t->k = T_CARET; t->n = 1; return;
    case '=':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = T_EQ; t->n = 2; }
      else { t->k = T_ASSIGN; t->n = 1; }
      return;
    case '~':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = T_NE; t->n = 2; }
      else { t->k = T_BAD; t->n = 1; }
      return;
    case '<':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = T_LE; t->n = 2; }
      else { t->k = T_LT; t->n = 1; }
      return;
    case '>':
      p->pos++;
      if (at(p, 0) == '=') { p->pos++; t->k = T_GE; t->n = 2; }
      else { t->k = T_GT; t->n = 1; }
      return;
  }
  t->k = T_BAD; t->n = 1; p->pos++;
}

static int eat(P *p, tk k) { if (p->cur.k == k) { lex(p); return 1; } return 0; }

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
static pl_stmt *stmt(P *p);

static pl_expr *primary(P *p) {
  tok t = p->cur;
  if (t.k == T_NUM) {
    lex(p);
    pl_expr *e = mke(p, PL_E_NUM);
    e->num = t.num; e->is_int = (t.num == (double)(long long)t.num);
    return e;
  }
  if (t.k == T_LP) {
    lex(p);
    pl_expr *e = expr(p);
    if (!eat(p, T_RP)) perr(p, "expected ')'");
    return e;
  }
  if (t.k == T_ID) {
    lex(p);
    /* lib.fn(...) — only the leaf name is significant for our intrinsic
    ** lookup; `math.sqrt(x)` and `sqrt(x)` both lower to llvm.sqrt.f64. */
    const char *fname_z = t.z; int fname_n = t.n;
    if (eat(p, T_DOT)) {
      if (p->cur.k != T_ID) { perr(p, "expected identifier after '.'"); return mke(p, PL_E_NUM); }
      fname_z = p->cur.z; fname_n = p->cur.n;
      lex(p);
    }
    if (p->cur.k == T_LP) {
      lex(p);
      pl_expr *e = mke(p, PL_E_CALL);
      e->fname = tdb_arena_strndup(p->a, fname_z, (size_t)fname_n);
      int cap = 0;
      if (p->cur.k != T_RP) {
        for (;;) {
          if (e->nargs == cap) {
            int nc = cap ? cap * 2 : 4;
            pl_expr **g = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * (size_t)nc);
            for (int i = 0; i < e->nargs; i++) g[i] = e->args[i];
            e->args = g; cap = nc;
          }
          e->args[e->nargs++] = expr(p);
          if (!eat(p, T_COMMA)) break;
        }
      }
      if (!eat(p, T_RP)) perr(p, "expected ')'");
      return e;
    }
    /* Plain variable reference. */
    pl_expr *e = mke(p, PL_E_VAR);
    e->slot = slot_for(p, t.z, t.n);
    return e;
  }
  perr(p, "expected expression");
  return mke(p, PL_E_NUM);
}

static pl_expr *unary(P *p) {
  if (eat(p, T_MINUS)) {
    pl_expr *e = mke(p, PL_E_UNARY); e->op = PL_OP_NEG; e->l = unary(p);
    return e;
  }
  if (eat(p, T_NOT)) {
    pl_expr *e = mke(p, PL_E_UNARY); e->op = PL_OP_NOT; e->l = unary(p);
    return e;
  }
  return primary(p);
}

static pl_expr *powx(P *p) {
  pl_expr *l = unary(p);
  if (eat(p, T_CARET)) {
    pl_expr *r = powx(p);  /* right-assoc */
    pl_expr *e = mke(p, PL_E_CALL);
    e->fname = tdb_arena_strndup(p->a, "pow", 3);
    e->args = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * 2);
    e->args[0] = l; e->args[1] = r; e->nargs = 2;
    return e;
  }
  return l;
}

static pl_expr *mulx(P *p) {
  pl_expr *l = powx(p);
  while (p->cur.k == T_STAR || p->cur.k == T_SLASH || p->cur.k == T_PCT) {
    pl_op op = p->cur.k == T_STAR ? PL_OP_MUL
             : p->cur.k == T_SLASH ? PL_OP_DIV : PL_OP_MOD;
    lex(p);
    pl_expr *r = powx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *addx(P *p) {
  pl_expr *l = mulx(p);
  while (p->cur.k == T_PLUS || p->cur.k == T_MINUS) {
    pl_op op = p->cur.k == T_PLUS ? PL_OP_ADD : PL_OP_SUB;
    lex(p);
    pl_expr *r = mulx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *relx(P *p) {
  pl_expr *l = addx(p);
  tk k = p->cur.k;
  if (k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE) {
    pl_op op = k == T_EQ ? PL_OP_EQ : k == T_NE ? PL_OP_NE
             : k == T_LT ? PL_OP_LT : k == T_LE ? PL_OP_LE
             : k == T_GT ? PL_OP_GT : PL_OP_GE;
    lex(p);
    pl_expr *r = addx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = op; e->l = l; e->r = r;
    return e;
  }
  return l;
}

static pl_expr *andx(P *p) {
  pl_expr *l = relx(p);
  while (eat(p, T_AND)) {
    pl_expr *r = relx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_AND; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *orx(P *p) {
  pl_expr *l = andx(p);
  while (eat(p, T_OR)) {
    pl_expr *r = andx(p);
    pl_expr *e = mke(p, PL_E_BINARY); e->op = PL_OP_OR; e->l = l; e->r = r;
    l = e;
  }
  return l;
}

static pl_expr *expr(P *p) { return orx(p); }

static pl_stmt **collect(P *p, int *n_out, tk stop1, tk stop2, tk stop3) {
  pl_stmt **arr = NULL; int n = 0, cap = 0;
  while (!p->err && p->cur.k != T_EOF &&
         p->cur.k != stop1 && p->cur.k != stop2 && p->cur.k != stop3) {
    pl_stmt *s = stmt(p);
    if (!s) break;
    if (n == cap) {
      int nc = cap ? cap * 2 : 4;
      pl_stmt **g = (pl_stmt **)tdb_arena_alloc(p->a, sizeof(pl_stmt *) * (size_t)nc);
      for (int i = 0; i < n; i++) g[i] = arr[i];
      arr = g; cap = nc;
    }
    arr[n++] = s;
  }
  *n_out = n;
  return arr;
}

static pl_stmt *stmt(P *p) {
  if (eat(p, T_SEMI)) {
    pl_stmt *s = mks(p, PL_S_NULL);
    return s;
  }
  if (eat(p, T_LOCAL)) {
    if (p->cur.k != T_ID) { perr(p, "expected identifier after 'local'"); return NULL; }
    int slot = slot_for(p, p->cur.z, p->cur.n);
    lex(p);
    pl_stmt *s = mks(p, PL_S_ASSIGN); s->slot = slot;
    if (eat(p, T_ASSIGN)) s->e1 = expr(p);
    else { pl_expr *z = mke(p, PL_E_NUM); z->num = 0; s->e1 = z; }
    eat(p, T_SEMI);
    return s;
  }
  if (eat(p, T_RETURN)) {
    pl_stmt *s = mks(p, PL_S_RETURN);
    if (p->cur.k != T_EOF && p->cur.k != T_END && p->cur.k != T_SEMI)
      s->e1 = expr(p);
    eat(p, T_SEMI);
    return s;
  }
  if (eat(p, T_IF)) {
    pl_stmt *s = mks(p, PL_S_IF);
    int cap = 0;
    for (;;) {
      pl_expr *c = expr(p);
      if (!eat(p, T_THEN)) { perr(p, "expected 'then'"); break; }
      int n; pl_stmt **body = collect(p, &n, T_ELSEIF, T_ELSE, T_END);
      if (s->nclause == cap) {
        int nc = cap ? cap * 2 : 2;
        pl_clause *g = (pl_clause *)tdb_arena_alloc(p->a, sizeof(pl_clause) * (size_t)nc);
        for (int i = 0; i < s->nclause; i++) g[i] = s->clauses[i];
        s->clauses = g; cap = nc;
      }
      s->clauses[s->nclause].cond = c;
      s->clauses[s->nclause].body = body;
      s->clauses[s->nclause].nbody = n;
      s->nclause++;
      if (eat(p, T_ELSEIF)) continue;
      break;
    }
    if (eat(p, T_ELSE)) {
      int n; s->elsebody = collect(p, &n, T_END, T_END, T_END); s->nelse = n;
    }
    if (!eat(p, T_END)) perr(p, "expected 'end'");
    return s;
  }
  if (eat(p, T_WHILE)) {
    pl_stmt *s = mks(p, PL_S_WHILE);
    s->e1 = expr(p);
    if (!eat(p, T_DO)) perr(p, "expected 'do'");
    int n; s->body = collect(p, &n, T_END, T_END, T_END); s->nbody = n;
    if (!eat(p, T_END)) perr(p, "expected 'end'");
    return s;
  }
  if (eat(p, T_FOR)) {
    if (p->cur.k != T_ID) { perr(p, "expected loop variable"); return NULL; }
    int slot = slot_for(p, p->cur.z, p->cur.n);
    lex(p);
    if (!eat(p, T_ASSIGN)) perr(p, "expected '='");
    pl_stmt *s = mks(p, PL_S_FOR);
    s->slot = slot;
    s->e1 = expr(p);
    if (!eat(p, T_COMMA)) perr(p, "expected ',' in for");
    s->e2 = expr(p);
    /* Optional step ignored — numeric subset only handles +1. */
    if (eat(p, T_COMMA)) { (void)expr(p); }
    if (!eat(p, T_DO)) perr(p, "expected 'do'");
    int n; s->body = collect(p, &n, T_END, T_END, T_END); s->nbody = n;
    if (!eat(p, T_END)) perr(p, "expected 'end'");
    return s;
  }
  if (p->cur.k == T_ID) {
    /* x = expr  (assignment to existing or new variable) */
    tok name = p->cur;
    lex(p);
    if (eat(p, T_ASSIGN)) {
      pl_stmt *s = mks(p, PL_S_ASSIGN);
      s->slot = slot_for(p, name.z, name.n);
      s->e1 = expr(p);
      eat(p, T_SEMI);
      return s;
    }
    perr(p, "expected '=' after identifier");
    return NULL;
  }
  perr(p, "unexpected token");
  return NULL;
}

int tdb_jit_lua_parse(const char *src, const char *const *params, int nparams,
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
  int n; pl_stmt **stmts = collect(&p, &n, T_EOF, T_EOF, T_EOF);
  if (p.err) {
    if (errbuf && errlen) snprintf(errbuf, (size_t)errlen, "%s", p.msg);
    tdb_arena_free(a);
    return TDB_ERROR;
  }
  tdb_plsql_proc *proc = (tdb_plsql_proc *)tdb_arena_alloc(a, sizeof(*proc));
  memset(proc, 0, sizeof(*proc));
  proc->a = a;
  proc->nslots = p.nslots; proc->slotnames = p.names;
  proc->nparams = p.nparams;
  proc->stmts = stmts; proc->nstmt = n;
  *out = proc;
  return TDB_OK;
}
