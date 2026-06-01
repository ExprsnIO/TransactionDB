/* tdb_plsql_parse.c — self-contained PL/SQL tokenizer + recursive-descent parser. */
#include "tdb_plsql_int.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------- tokens ------------------------------- */

typedef enum {
  PT_EOF, PT_ILLEGAL,
  PT_NUM, PT_STR, PT_ID,
  PT_ASSIGN, PT_DOTDOT,
  PT_LP, PT_RP, PT_COMMA, PT_SEMI,
  PT_PLUS, PT_MINUS, PT_STAR, PT_SLASH, PT_PCT, PT_CONCAT,
  PT_EQ, PT_NE, PT_LT, PT_LE, PT_GT, PT_GE
} pt_kind;

typedef struct {
  pt_kind     kind;
  const char *z;
  int         n;
  double      num;
  int         is_int;
} pt_tok;

typedef struct {
  tdb_arena  *a;
  const char *src;
  size_t      len, pos;
  pt_tok      cur;
  int         err;
  char        msg[160];
  /* symbol table */
  char      **names;
  int         nslots, cap;
  int         nparams;
} PP;

static int is_ids(int c) { return isalpha(c) || c == '_'; }
static int is_idc(int c) { return isalnum(c) || c == '_' || c == '$'; }

static void pp_err(PP *p, const char *m) {
  if (p->err) return;
  p->err = 1;
  snprintf(p->msg, sizeof(p->msg), "PL/SQL: %s near \"%.*s\"", m,
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?");
}

static int at(PP *p, size_t off) {
  return (p->pos + off < p->len) ? (unsigned char)p->src[p->pos + off] : 0;
}

static void lex(PP *p) {
  pt_tok *t = &p->cur;
  memset(t, 0, sizeof(*t));
  for (;;) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
    if (at(p, 0) == '-' && at(p, 1) == '-') {           /* line comment */
      while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
      continue;
    }
    if (at(p, 0) == '/' && at(p, 1) == '*') {           /* block comment */
      p->pos += 2;
      while (p->pos + 1 < p->len && !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) p->pos++;
      p->pos = (p->pos + 1 < p->len) ? p->pos + 2 : p->len;
      continue;
    }
    break;
  }
  t->z = p->src + p->pos;
  if (p->pos >= p->len) { t->kind = PT_EOF; return; }

  int c = at(p, 0);
  if (is_ids(c)) {
    size_t s = p->pos;
    while (p->pos < p->len && is_idc((unsigned char)p->src[p->pos])) p->pos++;
    t->kind = PT_ID; t->n = (int)(p->pos - s);
    return;
  }
  if (isdigit(c) || (c == '.' && isdigit(at(p, 1)))) {
    size_t s = p->pos; int isflt = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (at(p, 0) == '.' && at(p, 1) != '.') {           /* '..' is the range op */
      isflt = 1; p->pos++;
      while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (at(p, 0) == 'e' || at(p, 0) == 'E') {
      isflt = 1; p->pos++;
      if (at(p, 0) == '+' || at(p, 0) == '-') p->pos++;
      while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    t->kind = PT_NUM; t->n = (int)(p->pos - s); t->is_int = !isflt;
    char buf[64];
    int m = t->n < (int)sizeof(buf) - 1 ? t->n : (int)sizeof(buf) - 1;
    memcpy(buf, t->z, (size_t)m); buf[m] = '\0';
    t->num = strtod(buf, NULL);
    return;
  }
  if (c == '\'') {
    p->pos++; t->z = p->src + p->pos; size_t s = p->pos;
    for (;;) {
      if (p->pos >= p->len) break;
      if (p->src[p->pos] == '\'') {
        if (at(p, 1) == '\'') { p->pos += 2; continue; }
        break;
      }
      p->pos++;
    }
    t->kind = PT_STR; t->n = (int)(p->pos - s);
    if (p->pos < p->len) p->pos++;
    return;
  }

  p->pos++; t->n = 1;
  switch (c) {
    case '(': t->kind = PT_LP; break;
    case ')': t->kind = PT_RP; break;
    case ',': t->kind = PT_COMMA; break;
    case ';': t->kind = PT_SEMI; break;
    case '+': t->kind = PT_PLUS; break;
    case '-': t->kind = PT_MINUS; break;
    case '*': t->kind = PT_STAR; break;
    case '/': t->kind = PT_SLASH; break;
    case '%': t->kind = PT_PCT; break;
    case '=': t->kind = PT_EQ; break;
    case '.': if (at(p, 0) == '.') { p->pos++; t->n = 2; t->kind = PT_DOTDOT; } else t->kind = PT_ILLEGAL; break;
    case ':': if (at(p, 0) == '=') { p->pos++; t->n = 2; t->kind = PT_ASSIGN; } else t->kind = PT_ILLEGAL; break;
    case '|': if (at(p, 0) == '|') { p->pos++; t->n = 2; t->kind = PT_CONCAT; } else t->kind = PT_ILLEGAL; break;
    case '<':
      if (at(p, 0) == '=') { p->pos++; t->n = 2; t->kind = PT_LE; }
      else if (at(p, 0) == '>') { p->pos++; t->n = 2; t->kind = PT_NE; }
      else t->kind = PT_LT;
      break;
    case '>': if (at(p, 0) == '=') { p->pos++; t->n = 2; t->kind = PT_GE; } else t->kind = PT_GT; break;
    case '!': if (at(p, 0) == '=') { p->pos++; t->n = 2; t->kind = PT_NE; } else t->kind = PT_ILLEGAL; break;
    default:  t->kind = PT_ILLEGAL; break;
  }
}

/* ------------------------------ helpers ------------------------------- */

static int tok_is(const pt_tok *t, pt_kind k) { return t->kind == k; }

/* current token is the (case-insensitive) keyword `kw`? */
static int kw_is(const pt_tok *t, const char *kw) {
  return t->kind == PT_ID && (int)strlen(kw) == t->n &&
         strncasecmp(t->z, kw, (size_t)t->n) == 0;
}

static int find_slot(PP *p, const char *z, int n) {
  for (int i = 0; i < p->nslots; i++)
    if ((int)strlen(p->names[i]) == n && strncasecmp(p->names[i], z, (size_t)n) == 0)
      return i;
  return -1;
}

static int add_slot(PP *p, const char *z, int n) {
  int existing = find_slot(p, z, n);
  if (existing >= 0) return existing;
  if (p->nslots == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 8;
    char **g = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)p->cap);
    for (int i = 0; i < p->nslots; i++) g[i] = p->names[i];
    p->names = g;
  }
  p->names[p->nslots] = tdb_arena_strndup(p->a, z, (size_t)n);
  return p->nslots++;
}

/* ----------------------------- expressions ---------------------------- */

static pl_expr *new_expr(PP *p, pl_ekind k) {
  pl_expr *e = (pl_expr *)tdb_arena_alloc(p->a, sizeof(*e));
  memset(e, 0, sizeof(*e));
  e->kind = k;
  return e;
}

static pl_expr *parse_expr(PP *p, int minprec);

/* decode '' escapes from a string token */
static char *decode_str(PP *p, const pt_tok *t) {
  char *s = (char *)tdb_arena_alloc(p->a, (size_t)t->n + 1);
  int j = 0;
  for (int i = 0; i < t->n; i++) {
    s[j++] = t->z[i];
    if (t->z[i] == '\'' && i + 1 < t->n && t->z[i + 1] == '\'') i++;
  }
  s[j] = '\0';
  return s;
}

static pl_expr *parse_primary(PP *p) {
  pt_tok *t = &p->cur;
  if (tok_is(t, PT_NUM)) {
    pl_expr *e = new_expr(p, PL_E_NUM);
    e->num = t->num; e->is_int = t->is_int; lex(p); return e;
  }
  if (tok_is(t, PT_STR)) {
    pl_expr *e = new_expr(p, PL_E_STR);
    e->str = decode_str(p, t); lex(p); return e;
  }
  if (tok_is(t, PT_LP)) {
    lex(p);
    pl_expr *e = parse_expr(p, 0);
    if (!tok_is(t, PT_RP)) pp_err(p, "expected )"); else lex(p);
    return e;
  }
  if (kw_is(t, "NOT")) {
    lex(p);
    pl_expr *e = new_expr(p, PL_E_UNARY);
    e->op = PL_OP_NOT; e->l = parse_expr(p, 3);
    return e;
  }
  if (tok_is(t, PT_MINUS) || tok_is(t, PT_PLUS)) {
    int neg = tok_is(t, PT_MINUS);
    lex(p);
    pl_expr *operand = parse_primary(p);
    if (!neg) return operand;
    pl_expr *e = new_expr(p, PL_E_UNARY);
    e->op = PL_OP_NEG; e->l = operand;
    return e;
  }
  if (kw_is(t, "TRUE") || kw_is(t, "FALSE")) {
    pl_expr *e = new_expr(p, PL_E_NUM);
    e->num = kw_is(t, "TRUE") ? 1.0 : 0.0; e->is_int = 1; lex(p); return e;
  }
  if (kw_is(t, "NULL")) {
    pl_expr *e = new_expr(p, PL_E_NUM); e->num = 0; e->is_int = 1; lex(p); return e;
  }
  if (tok_is(t, PT_ID)) {
    pt_tok name = *t;
    lex(p);
    if (tok_is(t, PT_LP)) {                 /* builtin function call */
      pl_expr *e = new_expr(p, PL_E_CALL);
      e->fname = tdb_arena_strndup(p->a, name.z, (size_t)name.n);
      lex(p);
      int cap = 0;
      if (!tok_is(t, PT_RP)) {
        for (;;) {
          if (e->nargs == cap) {
            cap = cap ? cap * 2 : 4;
            pl_expr **g = (pl_expr **)tdb_arena_alloc(p->a, sizeof(pl_expr *) * (size_t)cap);
            for (int i = 0; i < e->nargs; i++) g[i] = e->args[i];
            e->args = g;
          }
          e->args[e->nargs++] = parse_expr(p, 0);
          if (tok_is(t, PT_COMMA)) { lex(p); continue; }
          break;
        }
      }
      if (!tok_is(t, PT_RP)) pp_err(p, "expected ) after arguments"); else lex(p);
      return e;
    }
    int slot = find_slot(p, name.z, name.n);
    if (slot < 0) { pp_err(p, "undeclared identifier"); slot = 0; }
    pl_expr *e = new_expr(p, PL_E_VAR);
    e->slot = slot;
    return e;
  }
  pp_err(p, "expected an expression");
  return new_expr(p, PL_E_NUM);
}

static int bin_op(const pt_tok *t, pl_op *op, int *prec) {
  if (kw_is(t, "OR"))  { *op = PL_OP_OR;  *prec = 1; return 1; }
  if (kw_is(t, "AND")) { *op = PL_OP_AND; *prec = 2; return 1; }
  switch (t->kind) {
    case PT_EQ: *op = PL_OP_EQ; *prec = 3; return 1;
    case PT_NE: *op = PL_OP_NE; *prec = 3; return 1;
    case PT_LT: *op = PL_OP_LT; *prec = 3; return 1;
    case PT_LE: *op = PL_OP_LE; *prec = 3; return 1;
    case PT_GT: *op = PL_OP_GT; *prec = 3; return 1;
    case PT_GE: *op = PL_OP_GE; *prec = 3; return 1;
    case PT_CONCAT: *op = PL_OP_CONCAT; *prec = 4; return 1;
    case PT_PLUS:  *op = PL_OP_ADD; *prec = 5; return 1;
    case PT_MINUS: *op = PL_OP_SUB; *prec = 5; return 1;
    case PT_STAR:  *op = PL_OP_MUL; *prec = 6; return 1;
    case PT_SLASH: *op = PL_OP_DIV; *prec = 6; return 1;
    case PT_PCT:   *op = PL_OP_MOD; *prec = 6; return 1;
    default: return 0;
  }
}

static pl_expr *parse_expr(PP *p, int minprec) {
  pl_expr *lhs = parse_primary(p);
  for (;;) {
    pl_op op; int prec;
    if (!bin_op(&p->cur, &op, &prec) || prec < minprec) break;
    lex(p);
    pl_expr *rhs = parse_expr(p, prec + 1);
    pl_expr *e = new_expr(p, PL_E_BINARY);
    e->op = op; e->l = lhs; e->r = rhs;
    lhs = e;
    if (p->err) break;
  }
  return lhs;
}

/* ----------------------------- statements ----------------------------- */

static pl_stmt **parse_block(PP *p, const char *t1, const char *t2, int *count);

static pl_stmt *new_stmt(PP *p, pl_skind k) {
  pl_stmt *s = (pl_stmt *)tdb_arena_alloc(p->a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->kind = k;
  return s;
}

static void expect_semi(PP *p) {
  if (tok_is(&p->cur, PT_SEMI)) lex(p); else pp_err(p, "expected ;");
}

static pl_stmt *parse_stmt(PP *p) {
  pt_tok *t = &p->cur;

  if (kw_is(t, "NULL")) { lex(p); expect_semi(p); return new_stmt(p, PL_S_NULL); }

  if (kw_is(t, "RETURN")) {
    lex(p);
    pl_stmt *s = new_stmt(p, PL_S_RETURN);
    if (!tok_is(t, PT_SEMI)) s->e1 = parse_expr(p, 0);
    expect_semi(p);
    return s;
  }

  if (kw_is(t, "IF")) {
    lex(p);
    pl_stmt *s = new_stmt(p, PL_S_IF);
    int cap = 0;
    for (;;) {                              /* IF / ELSIF arms */
      pl_clause cl; memset(&cl, 0, sizeof(cl));
      cl.cond = parse_expr(p, 0);
      if (!kw_is(t, "THEN")) pp_err(p, "expected THEN"); else lex(p);
      cl.body = parse_block(p, "ELSIF", "ELSE", &cl.nbody); /* terminators handled below */
      if (s->nclause == cap) {
        cap = cap ? cap * 2 : 2;
        pl_clause *g = (pl_clause *)tdb_arena_alloc(p->a, sizeof(pl_clause) * (size_t)cap);
        for (int i = 0; i < s->nclause; i++) g[i] = s->clauses[i];
        s->clauses = g;
      }
      s->clauses[s->nclause++] = cl;
      if (kw_is(t, "ELSIF")) { lex(p); continue; }
      break;
    }
    if (kw_is(t, "ELSE")) { lex(p); s->elsebody = parse_block(p, "END", NULL, &s->nelse); }
    if (!kw_is(t, "END")) pp_err(p, "expected END IF"); else lex(p);
    if (!kw_is(t, "IF")) pp_err(p, "expected IF after END"); else lex(p);
    expect_semi(p);
    return s;
  }

  if (kw_is(t, "WHILE")) {
    lex(p);
    pl_stmt *s = new_stmt(p, PL_S_WHILE);
    s->e1 = parse_expr(p, 0);
    if (!kw_is(t, "LOOP")) pp_err(p, "expected LOOP"); else lex(p);
    s->body = parse_block(p, "END", NULL, &s->nbody);
    if (!kw_is(t, "END")) pp_err(p, "expected END LOOP"); else lex(p);
    if (!kw_is(t, "LOOP")) pp_err(p, "expected LOOP after END"); else lex(p);
    expect_semi(p);
    return s;
  }

  if (kw_is(t, "FOR")) {
    lex(p);
    pl_stmt *s = new_stmt(p, PL_S_FOR);
    if (!tok_is(t, PT_ID)) { pp_err(p, "expected loop variable"); return s; }
    s->slot = add_slot(p, t->z, t->n);
    lex(p);
    if (!kw_is(t, "IN")) pp_err(p, "expected IN"); else lex(p);
    s->e1 = parse_expr(p, 0);
    if (!tok_is(t, PT_DOTDOT)) pp_err(p, "expected .. in FOR range"); else lex(p);
    s->e2 = parse_expr(p, 0);
    if (!kw_is(t, "LOOP")) pp_err(p, "expected LOOP"); else lex(p);
    s->body = parse_block(p, "END", NULL, &s->nbody);
    if (!kw_is(t, "END")) pp_err(p, "expected END LOOP"); else lex(p);
    if (!kw_is(t, "LOOP")) pp_err(p, "expected LOOP after END"); else lex(p);
    expect_semi(p);
    return s;
  }

  /* assignment: ident := expr ; */
  if (tok_is(t, PT_ID)) {
    pt_tok name = *t;
    lex(p);
    if (!tok_is(t, PT_ASSIGN)) { pp_err(p, "expected := in assignment"); return new_stmt(p, PL_S_NULL); }
    lex(p);
    pl_stmt *s = new_stmt(p, PL_S_ASSIGN);
    s->slot = find_slot(p, name.z, name.n);
    if (s->slot < 0) { pp_err(p, "assignment to undeclared variable"); s->slot = 0; }
    s->e1 = parse_expr(p, 0);
    expect_semi(p);
    return s;
  }

  pp_err(p, "expected a statement");
  lex(p);
  return new_stmt(p, PL_S_NULL);
}

/* Parse statements until a terminating keyword (END / ELSIF / ELSE). The
** terminators are matched by the caller; t1/t2 are only documentation hints. */
static pl_stmt **parse_block(PP *p, const char *t1, const char *t2, int *count) {
  (void)t1; (void)t2;
  pl_stmt **list = NULL; int n = 0, cap = 0;
  while (!p->err && !tok_is(&p->cur, PT_EOF) &&
         !kw_is(&p->cur, "END") && !kw_is(&p->cur, "ELSIF") && !kw_is(&p->cur, "ELSE")) {
    pl_stmt *s = parse_stmt(p);
    if (n == cap) {
      cap = cap ? cap * 2 : 8;
      pl_stmt **g = (pl_stmt **)tdb_arena_alloc(p->a, sizeof(pl_stmt *) * (size_t)cap);
      for (int i = 0; i < n; i++) g[i] = list[i];
      list = g;
    }
    list[n++] = s;
  }
  *count = n;
  return list;
}

/* --------------------------- declarations ----------------------------- */

/* DECLARE  name type [:= expr] ;  ...  (we ignore the type, treating all
** variables as dynamically typed values). Initialisers become leading
** assignment statements so the interpreter and IR paths share one code path. */
static void parse_declare(PP *p, pl_stmt ***pre, int *npre) {
  pl_stmt **list = NULL; int n = 0, cap = 0;
  while (!p->err && tok_is(&p->cur, PT_ID) && !kw_is(&p->cur, "BEGIN")) {
    pt_tok name = p->cur;
    lex(p);
    int slot = add_slot(p, name.z, name.n);
    /* skip the type tokens up to ':=' or ';' */
    while (!p->err && !tok_is(&p->cur, PT_SEMI) && !tok_is(&p->cur, PT_ASSIGN) &&
           !tok_is(&p->cur, PT_EOF))
      lex(p);
    if (tok_is(&p->cur, PT_ASSIGN)) {
      lex(p);
      pl_stmt *s = new_stmt(p, PL_S_ASSIGN);
      s->slot = slot; s->e1 = parse_expr(p, 0);
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        pl_stmt **g = (pl_stmt **)tdb_arena_alloc(p->a, sizeof(pl_stmt *) * (size_t)cap);
        for (int i = 0; i < n; i++) g[i] = list[i];
        list = g;
      }
      list[n++] = s;
    }
    expect_semi(p);
  }
  *pre = list; *npre = n;
}

/* ------------------------------- entry -------------------------------- */

int tdb_plsql_parse(const char *src, const char *const *params, int nparams,
                    tdb_plsql_proc **out, char *errbuf, int errlen) {
  if (!src || !out) return TDB_MISUSE;
  *out = NULL;
  tdb_arena *a = tdb_arena_new(4096);
  if (!a) return TDB_NOMEM;

  PP p; memset(&p, 0, sizeof(p));
  p.a = a; p.src = src; p.len = strlen(src);

  for (int i = 0; i < nparams; i++) {
    const char *nm = params ? params[i] : NULL;
    if (!nm) { tdb_arena_free(a); return TDB_MISUSE; }
    add_slot(&p, nm, (int)strlen(nm));
  }
  p.nparams = nparams;

  lex(&p);

  pl_stmt **pre = NULL; int npre = 0;
  if (kw_is(&p.cur, "DECLARE")) { lex(&p); parse_declare(&p, &pre, &npre); }

  if (!kw_is(&p.cur, "BEGIN")) pp_err(&p, "expected BEGIN");
  else lex(&p);

  int nbody = 0;
  pl_stmt **body = parse_block(&p, "END", NULL, &nbody);

  if (!kw_is(&p.cur, "END")) pp_err(&p, "expected END");
  else { lex(&p); if (tok_is(&p.cur, PT_SEMI)) lex(&p); }

  if (p.err) {
    if (errbuf && errlen > 0) snprintf(errbuf, (size_t)errlen, "%s", p.msg);
    tdb_arena_free(a);
    return TDB_ERROR;
  }

  /* combine declare-initialisers with the body into one statement list */
  int total = npre + nbody;
  pl_stmt **all = (pl_stmt **)tdb_arena_alloc(a, sizeof(pl_stmt *) * (size_t)(total ? total : 1));
  for (int i = 0; i < npre; i++) all[i] = pre[i];
  for (int i = 0; i < nbody; i++) all[npre + i] = body[i];

  tdb_plsql_proc *proc = (tdb_plsql_proc *)tdb_arena_alloc(a, sizeof(*proc));
  memset(proc, 0, sizeof(*proc));
  proc->a = a;
  proc->nslots = p.nslots;
  proc->slotnames = p.names;
  proc->nparams = p.nparams;
  proc->stmts = all;
  proc->nstmt = total;
  *out = proc;
  return TDB_OK;
}

void tdb_plsql_free(tdb_plsql_proc *p) {
  if (p) tdb_arena_free(p->a);
}

int tdb_plsql_param_count(const tdb_plsql_proc *p) {
  return p ? p->nparams : 0;
}
