/* tdb_parser.c — hand-written recursive-descent parser. */
#include "tdb_parser.h"
#include "tdb_lexer.h"
#include "../common/tdb_mem.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
  tdb_arena  *a;
  const char *sql;
  tdb_lexer   lx;
  tdb_token   cur;
  int         err;
  char       *errmsg;
} P;

/* ------------------------------ utilities ----------------------------- */

static void advance(P *p) { tdb_lex_next(&p->lx, &p->cur); }

static void set_err(P *p, const char *msg) {
  if (p->err) return;
  p->err = 1;
  char buf[160];
  snprintf(buf, sizeof(buf), "near \"%.*s\": %s",
           p->cur.n ? p->cur.n : 1, p->cur.z ? p->cur.z : "?", msg);
  p->errmsg = tdb_arena_strndup(p->a, buf, strlen(buf));
}

static int accept(P *p, tdb_token_kind k) {
  if (p->cur.kind == k) { advance(p); return 1; }
  return 0;
}
static int expect(P *p, tdb_token_kind k, const char *what) {
  if (p->cur.kind == k) { advance(p); return 1; }
  set_err(p, what);
  return 0;
}

static char *dupz(P *p, const char *z, int n) { return tdb_arena_strndup(p->a, z, (size_t)n); }
static char *dup_tok(P *p, const tdb_token *t) { return dupz(p, t->z, t->n); }

static size_t off(P *p) { return (size_t)(p->cur.z - p->sql); }

/* arena copy of sql[start,end) with surrounding whitespace trimmed */
static char *span(P *p, size_t start, size_t end) {
  while (start < end && isspace((unsigned char)p->sql[start])) start++;
  while (end > start && isspace((unsigned char)p->sql[end - 1])) end--;
  return dupz(p, p->sql + start, (int)(end - start));
}

/* decode '' escapes in a string token into an arena buffer */
static char *decode_string(P *p, const tdb_token *t) {
  char *s = (char *)tdb_arena_alloc(p->a, (size_t)t->n + 1);
  int j = 0;
  for (int i = 0; i < t->n; i++) {
    s[j++] = t->z[i];
    if (t->z[i] == '\'' && i + 1 < t->n && t->z[i + 1] == '\'') i++; /* skip dbl */
  }
  s[j] = '\0';
  return s;
}

/* forward decls */
static tdb_expr *parse_expr(P *p, int minprec);
static tdb_expr *parse_unary(P *p);
static tdb_select *parse_select(P *p);

/* ------------------------------ expressions --------------------------- */

static int is_agg_name(const char *n) {
  return !strcasecmp(n, "count") || !strcasecmp(n, "sum") ||
         !strcasecmp(n, "avg") || !strcasecmp(n, "min") ||
         !strcasecmp(n, "max") || !strcasecmp(n, "total") ||
         !strcasecmp(n, "group_concat");
}

static int binprec(int op) {
  switch (op) {
    case TK_OR: return 1;
    case TK_AND: return 2;
    case TK_EQ: case TK_NE: case TK_LT: case TK_LE: case TK_GT: case TK_GE:
    case TK_IS: case TK_IN: case TK_LIKE: case TK_BETWEEN: return 4;
    case TK_PLUS: case TK_MINUS: return 6;
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 7;
    case TK_CONCAT: return 8;
    default: return 0;
  }
}

static tdb_expr *parse_primary(P *p) {
  tdb_expr *e;
  switch (p->cur.kind) {
    case TK_INTEGER:
      e = tdb_expr_new(p->a, EX_LITERAL);
      tdb_value_set_int(&e->lit, p->cur.ival); advance(p); return e;
    case TK_FLOAT:
      e = tdb_expr_new(p->a, EX_LITERAL);
      tdb_value_set_real(&e->lit, p->cur.rval); advance(p); return e;
    case TK_STRING:
      e = tdb_expr_new(p->a, EX_LITERAL);
      { char *s = decode_string(p, &p->cur);
        tdb_value_set_text(&e->lit, s, -1, 0); }
      advance(p); return e;
    case TK_BLOB:
      e = tdb_expr_new(p->a, EX_LITERAL);
      tdb_value_set_text(&e->lit, dup_tok(p, &p->cur), -1, 0); /* hex text; analyzer decodes */
      advance(p); return e;
    case TK_NULL: e = tdb_expr_new(p->a, EX_NULL); advance(p); return e;
    case TK_TRUE: e = tdb_expr_new(p->a, EX_LITERAL); tdb_value_set_int(&e->lit, 1); advance(p); return e;
    case TK_FALSE: e = tdb_expr_new(p->a, EX_LITERAL); tdb_value_set_int(&e->lit, 0); advance(p); return e;
    case TK_PARAM:
      e = tdb_expr_new(p->a, EX_PARAM); e->param = dup_tok(p, &p->cur); advance(p); return e;
    case TK_STAR:
      e = tdb_expr_new(p->a, EX_STAR); advance(p); return e;
    case TK_LP:
      advance(p);
      if (p->cur.kind == TK_SELECT) {
        e = tdb_expr_new(p->a, EX_SUBQUERY);
        e->subquery = parse_select(p);
        expect(p, TK_RP, "expected )");
        return e;
      }
      e = parse_expr(p, 0);
      expect(p, TK_RP, "expected )");
      return e;
    case TK_CAST: {
      advance(p);
      expect(p, TK_LP, "expected ( after CAST");
      e = tdb_expr_new(p->a, EX_CAST);
      e->left = parse_expr(p, 0);
      expect(p, TK_AS, "expected AS in CAST");
      size_t ts = off(p);
      while (p->cur.kind == TK_ID) advance(p);
      e->cast = tdb_typespec_parse(span(p, ts, off(p)));
      expect(p, TK_RP, "expected ) after CAST");
      return e;
    }
    case TK_EXISTS:
      advance(p);
      e = tdb_expr_new(p->a, EX_EXISTS);
      expect(p, TK_LP, "expected ( after EXISTS");
      e->subquery = parse_select(p);
      expect(p, TK_RP, "expected )");
      return e;
    case TK_CASE: {
      advance(p);
      e = tdb_expr_new(p->a, EX_CASE);
      e->args = tdb_exprlist_new(p->a);
      if (p->cur.kind != TK_WHEN) e->left = parse_expr(p, 0); /* base */
      while (accept(p, TK_WHEN)) {
        tdb_expr *w = parse_expr(p, 0);
        expect(p, TK_THEN, "expected THEN");
        tdb_expr *t = parse_expr(p, 0);
        tdb_exprlist_add(p->a, e->args, w, NULL);
        tdb_exprlist_add(p->a, e->args, t, NULL);
      }
      if (accept(p, TK_ELSE)) e->right = parse_expr(p, 0);
      expect(p, TK_END, "expected END");
      return e;
    }
    case TK_ID: {
      char *name = dup_tok(p, &p->cur);
      advance(p);
      if (p->cur.kind == TK_DOT) {        /* qualified: table.col or table.* */
        advance(p);
        if (p->cur.kind == TK_STAR) { e = tdb_expr_new(p->a, EX_STAR); advance(p); return e; }
        e = tdb_expr_new(p->a, EX_COLUMN);
        e->table = name; e->name = dup_tok(p, &p->cur); advance(p);
        return e;
      }
      if (p->cur.kind == TK_LP) {         /* function call */
        advance(p);
        int agg = is_agg_name(name);
        e = tdb_expr_new(p->a, agg ? EX_AGG : EX_FUNC);
        e->name = name;
        e->args = tdb_exprlist_new(p->a);
        if (accept(p, TK_DISTINCT)) e->distinct = 1;
        if (p->cur.kind != TK_RP) {
          do { tdb_exprlist_add(p->a, e->args, parse_expr(p, 0), NULL); }
          while (accept(p, TK_COMMA));
        }
        expect(p, TK_RP, "expected ) after arguments");
        return e;
      }
      e = tdb_expr_new(p->a, EX_COLUMN); e->name = name; return e;
    }
    default:
      set_err(p, "expected an expression");
      return tdb_expr_new(p->a, EX_NULL);
  }
}

static tdb_expr *parse_unary(P *p) {
  if (p->cur.kind == TK_MINUS || p->cur.kind == TK_PLUS) {
    int op = p->cur.kind; advance(p);
    tdb_expr *e = tdb_expr_new(p->a, EX_UNARY);
    e->op = op; e->left = parse_unary(p);
    return e;
  }
  if (p->cur.kind == TK_NOT) {
    advance(p);
    tdb_expr *e = tdb_expr_new(p->a, EX_UNARY);
    e->op = TK_NOT; e->left = parse_expr(p, 3);
    return e;
  }
  return parse_primary(p);
}

static tdb_expr *parse_expr(P *p, int minprec) {
  tdb_expr *left = parse_unary(p);
  for (;;) {
    if (p->err) return left;
    int op = p->cur.kind;
    int negated = 0;
    if (op == TK_NOT) {  /* expr NOT IN / NOT LIKE / NOT BETWEEN */
      tdb_lexer save = p->lx; tdb_token savecur = p->cur;
      advance(p);
      if (p->cur.kind == TK_IN || p->cur.kind == TK_LIKE || p->cur.kind == TK_BETWEEN) {
        negated = 1; op = p->cur.kind;
      } else { p->lx = save; p->cur = savecur; break; }
    }
    int prec = binprec(op);
    if (prec < minprec || prec == 0) break;

    if (op == TK_BETWEEN) {
      advance(p);
      tdb_expr *e = tdb_expr_new(p->a, EX_BETWEEN);
      e->left = left; e->negated = negated; e->args = tdb_exprlist_new(p->a);
      tdb_exprlist_add(p->a, e->args, parse_expr(p, prec + 1), NULL);
      expect(p, TK_AND, "expected AND in BETWEEN");
      tdb_exprlist_add(p->a, e->args, parse_expr(p, prec + 1), NULL);
      left = e; continue;
    }
    if (op == TK_IN) {
      advance(p);
      tdb_expr *e = tdb_expr_new(p->a, EX_IN);
      e->left = left; e->negated = negated;
      expect(p, TK_LP, "expected ( after IN");
      if (p->cur.kind == TK_SELECT) {
        e->subquery = parse_select(p);
      } else {
        e->args = tdb_exprlist_new(p->a);
        if (p->cur.kind != TK_RP)
          do { tdb_exprlist_add(p->a, e->args, parse_expr(p, 0), NULL); }
          while (accept(p, TK_COMMA));
      }
      expect(p, TK_RP, "expected ) after IN list");
      left = e; continue;
    }
    if (op == TK_IS) {
      advance(p);
      int isnot = accept(p, TK_NOT);
      tdb_expr *e = tdb_expr_new(p->a, EX_BINARY);
      e->op = TK_IS;               /* NULL-aware equality (handled in eval) */
      e->negated = isnot;
      e->left = left;
      e->right = parse_unary(p);   /* IS [NOT] NULL / value */
      left = e; continue;
    }

    advance(p);
    tdb_expr *e = tdb_expr_new(p->a, EX_BINARY);
    e->op = op; e->negated = negated; e->left = left;
    e->right = parse_expr(p, prec + 1);
    left = e;
  }
  return left;
}

/* ------------------------------- SELECT ------------------------------- */

/* optional alias: "AS name" or a bare identifier */
static char *opt_alias(P *p) {
  if (accept(p, TK_AS)) { char *a = dup_tok(p, &p->cur); expect(p, TK_ID, "expected alias"); return a; }
  if (p->cur.kind == TK_ID) { char *a = dup_tok(p, &p->cur); advance(p); return a; }
  return NULL;
}

static tdb_src *parse_src_item(P *p) {
  tdb_src *s = (tdb_src *)tdb_arena_alloc(p->a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  if (p->cur.kind == TK_LP) {
    advance(p);
    s->subquery = parse_select(p);
    expect(p, TK_RP, "expected ) after subquery");
    s->alias = opt_alias(p);
  } else {
    s->table = dup_tok(p, &p->cur);
    expect(p, TK_ID, "expected table name");
    /* FOR SYSTEM_TIME AS OF <expr> (temporal) — checked before an alias */
    if (p->cur.kind == TK_FOR) {
      advance(p);
      expect(p, TK_ID, "expected SYSTEM_TIME");   /* SYSTEM_TIME is one identifier */
      accept(p, TK_AS);
      if (p->cur.kind == TK_ID) advance(p);        /* the word OF */
      s->as_of = parse_expr(p, 0);
    } else {
      s->alias = opt_alias(p);
    }
  }
  return s;
}

static tdb_src *parse_from(P *p) {
  tdb_src *head = parse_src_item(p);
  tdb_src *tailp = head;
  for (;;) {
    tdb_join_kind jk = JOIN_NONE;
    if (accept(p, TK_COMMA)) jk = JOIN_CROSS;
    else if (p->cur.kind == TK_JOIN) { advance(p); jk = JOIN_INNER; }
    else if (p->cur.kind == TK_INNER) { advance(p); accept(p, TK_JOIN); jk = JOIN_INNER; }
    else if (p->cur.kind == TK_CROSS) { advance(p); accept(p, TK_JOIN); jk = JOIN_CROSS; }
    else if (p->cur.kind == TK_LEFT)  { advance(p); accept(p, TK_OUTER); accept(p, TK_JOIN); jk = JOIN_LEFT; }
    else if (p->cur.kind == TK_RIGHT) { advance(p); accept(p, TK_OUTER); accept(p, TK_JOIN); jk = JOIN_RIGHT; }
    else if (p->cur.kind == TK_FULL)  { advance(p); accept(p, TK_OUTER); accept(p, TK_JOIN); jk = JOIN_FULL; }
    else break;

    tdb_src *next = parse_src_item(p);
    next->join = jk;
    if (accept(p, TK_ON)) next->on = parse_expr(p, 0);
    tailp->next = next; tailp = next;
    if (p->err) break;
  }
  return head;
}

static tdb_select *parse_select(P *p) {
  expect(p, TK_SELECT, "expected SELECT");
  tdb_select *s = (tdb_select *)tdb_arena_alloc(p->a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  if (accept(p, TK_DISTINCT)) s->distinct = 1; else accept(p, TK_ALL);

  s->cols = tdb_exprlist_new(p->a);
  do {
    tdb_expr *e = parse_expr(p, 0);
    char *alias = NULL;
    if (accept(p, TK_AS)) { alias = dup_tok(p, &p->cur); expect(p, TK_ID, "expected alias"); }
    else if (p->cur.kind == TK_ID) { alias = dup_tok(p, &p->cur); advance(p); }
    tdb_exprlist_add(p->a, s->cols, e, alias);
  } while (accept(p, TK_COMMA) && !p->err);

  if (accept(p, TK_FROM)) s->from = parse_from(p);
  if (accept(p, TK_WHERE)) s->where = parse_expr(p, 0);
  if (accept(p, TK_GROUP)) {
    expect(p, TK_BY, "expected BY after GROUP");
    s->group = tdb_exprlist_new(p->a);
    do { tdb_exprlist_add(p->a, s->group, parse_expr(p, 0), NULL); } while (accept(p, TK_COMMA));
    if (accept(p, TK_HAVING)) s->having = parse_expr(p, 0);
  }
  if (accept(p, TK_ORDER)) {
    expect(p, TK_BY, "expected BY after ORDER");
    s->order = tdb_orderby_new(p->a);
    do {
      tdb_expr *e = parse_expr(p, 0);
      int desc = 0;
      if (accept(p, TK_ASC)) desc = 0; else if (accept(p, TK_DESC)) desc = 1;
      tdb_orderby_add(p->a, s->order, e, desc);
    } while (accept(p, TK_COMMA));
  }
  if (accept(p, TK_LIMIT)) {
    s->has_limit = 1;
    s->limit = parse_expr(p, 0);
    if (accept(p, TK_OFFSET)) s->offset = parse_expr(p, 0);
    else if (accept(p, TK_COMMA)) { s->offset = s->limit; s->limit = parse_expr(p, 0); }
  }
  return s;
}

/* -------------------------------- DML --------------------------------- */

static tdb_ast_stmt *new_stmt(P *p, tdb_stmt_kind k) {
  tdb_ast_stmt *s = (tdb_ast_stmt *)tdb_arena_alloc(p->a, sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->kind = k;
  return s;
}

static char **parse_name_list(P *p, int *count) {
  char **names = NULL; int n = 0, cap = 0;
  do {
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      char **g = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)cap);
      for (int i = 0; i < n; i++) g[i] = names[i];
      names = g;
    }
    names[n++] = dup_tok(p, &p->cur);
    expect(p, TK_ID, "expected name");
  } while (accept(p, TK_COMMA));
  *count = n;
  return names;
}

static tdb_ast_stmt *parse_insert(P *p) {
  advance(p); /* INSERT */
  expect(p, TK_INTO, "expected INTO");
  tdb_ast_stmt *s = new_stmt(p, ST_INSERT);
  s->u.insert.table = dup_tok(p, &p->cur);
  expect(p, TK_ID, "expected table name");
  if (accept(p, TK_LP)) {
    s->u.insert.cols = parse_name_list(p, &s->u.insert.ncol);
    expect(p, TK_RP, "expected )");
  }
  if (p->cur.kind == TK_SELECT) {
    s->u.insert.select = parse_select(p);
  } else {
    expect(p, TK_VALUES, "expected VALUES or SELECT");
    int cap = 0;
    do {
      expect(p, TK_LP, "expected ( in VALUES");
      tdb_exprlist *row = tdb_exprlist_new(p->a);
      if (p->cur.kind != TK_RP)
        do { tdb_exprlist_add(p->a, row, parse_expr(p, 0), NULL); } while (accept(p, TK_COMMA));
      expect(p, TK_RP, "expected ) in VALUES");
      if (s->u.insert.nrows == cap) {
        cap = cap ? cap * 2 : 4;
        tdb_exprlist **g = (tdb_exprlist **)tdb_arena_alloc(p->a, sizeof(void *) * (size_t)cap);
        for (int i = 0; i < s->u.insert.nrows; i++) g[i] = s->u.insert.rows[i];
        s->u.insert.rows = g;
      }
      s->u.insert.rows[s->u.insert.nrows++] = row;
    } while (accept(p, TK_COMMA) && !p->err);
  }
  return s;
}

static tdb_ast_stmt *parse_update(P *p) {
  advance(p); /* UPDATE */
  tdb_ast_stmt *s = new_stmt(p, ST_UPDATE);
  s->u.update.table = dup_tok(p, &p->cur);
  expect(p, TK_ID, "expected table name");
  expect(p, TK_SET, "expected SET");
  int cap = 0;
  do {
    if (s->u.update.nset == cap) {
      cap = cap ? cap * 2 : 4;
      char **gc = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)cap);
      tdb_expr **gv = (tdb_expr **)tdb_arena_alloc(p->a, sizeof(void *) * (size_t)cap);
      for (int i = 0; i < s->u.update.nset; i++) { gc[i] = s->u.update.set_cols[i]; gv[i] = s->u.update.set_vals[i]; }
      s->u.update.set_cols = gc; s->u.update.set_vals = gv;
    }
    s->u.update.set_cols[s->u.update.nset] = dup_tok(p, &p->cur);
    expect(p, TK_ID, "expected column name");
    expect(p, TK_EQ, "expected = in SET");
    s->u.update.set_vals[s->u.update.nset] = parse_expr(p, 0);
    s->u.update.nset++;
  } while (accept(p, TK_COMMA) && !p->err);
  if (accept(p, TK_WHERE)) s->u.update.where = parse_expr(p, 0);
  return s;
}

static tdb_ast_stmt *parse_delete(P *p) {
  advance(p); /* DELETE */
  expect(p, TK_FROM, "expected FROM");
  tdb_ast_stmt *s = new_stmt(p, ST_DELETE);
  s->u.del.table = dup_tok(p, &p->cur);
  expect(p, TK_ID, "expected table name");
  if (accept(p, TK_WHERE)) s->u.del.where = parse_expr(p, 0);
  return s;
}

/* ------------------------------- DDL ---------------------------------- */

/* consume a declared type and return its typespec */
static tdb_typespec parse_type(P *p) {
  size_t ts = off(p);
  while (p->cur.kind == TK_ID) advance(p);
  if (p->cur.kind == TK_LP) {
    int depth = 0;
    do {
      if (p->cur.kind == TK_LP) depth++;
      else if (p->cur.kind == TK_RP) depth--;
      advance(p);
    } while (depth > 0 && p->cur.kind != TK_EOF);
  }
  return tdb_typespec_parse(span(p, ts, off(p)));
}

static void parse_column_constraints(P *p, tdb_coldef *c) {
  for (;;) {
    if (p->cur.kind == TK_NOT) { advance(p); expect(p, TK_NULL, "expected NULL"); c->notnull = 1; }
    else if (p->cur.kind == TK_NULL) { advance(p); }
    else if (p->cur.kind == TK_PRIMARY) {
      advance(p); expect(p, TK_KEY, "expected KEY"); c->pk = 1;
      if (accept(p, TK_ASC) || accept(p, TK_DESC)) {}
      if (accept(p, TK_AUTOINCREMENT)) c->autoinc = 1;
    }
    else if (p->cur.kind == TK_UNIQUE) { advance(p); c->unique = 1; }
    else if (p->cur.kind == TK_DEFAULT) {
      advance(p);
      size_t s0 = off(p);
      (void)parse_expr(p, 0);
      c->default_sql = span(p, s0, off(p));
    }
    else if (p->cur.kind == TK_CHECK) {
      advance(p); expect(p, TK_LP, "expected ( after CHECK");
      size_t s0 = off(p);
      (void)parse_expr(p, 0);
      c->check_sql = span(p, s0, off(p));
      expect(p, TK_RP, "expected ) after CHECK");
    }
    else if (p->cur.kind == TK_COLLATE) {
      advance(p);
      if (p->cur.kind == TK_ID && p->cur.n == 6 && !strncasecmp(p->cur.z, "NOCASE", 6))
        c->coll = TDB_COLL_NOCASE;
      expect(p, TK_ID, "expected collation name");
    }
    else if (p->cur.kind == TK_GENERATED || p->cur.kind == TK_AS) {
      if (p->cur.kind == TK_GENERATED) { advance(p); accept(p, TK_ALWAYS); expect(p, TK_AS, "expected AS"); }
      else advance(p); /* bare AS (...) */
      expect(p, TK_LP, "expected ( after AS");
      size_t s0 = off(p);
      (void)parse_expr(p, 0);
      c->generated_sql = span(p, s0, off(p));
      expect(p, TK_RP, "expected ) after generated expression");
      c->generated = TDB_GEN_VIRTUAL;
      if (accept(p, TK_STORED)) c->generated = TDB_GEN_STORED;
      else if (accept(p, TK_VIRTUAL)) c->generated = TDB_GEN_VIRTUAL;
    }
    else if (p->cur.kind == TK_REFERENCES) {
      advance(p); expect(p, TK_ID, "expected referenced table");
      if (accept(p, TK_LP)) { (void)parse_name_list(p, &(int){0}); expect(p, TK_RP, "expected )"); }
    }
    else break;
    if (p->err) break;
  }
}

static tdb_ast_stmt *parse_create_table(P *p) {
  tdb_ast_stmt *s = new_stmt(p, ST_CREATE_TABLE);
  tdb_create_table *ct = (tdb_create_table *)tdb_arena_alloc(p->a, sizeof(*ct));
  memset(ct, 0, sizeof(*ct));
  s->u.create_table = ct;

  if (accept(p, TK_TEMP)) ct->temp = 1;
  expect(p, TK_TABLE, "expected TABLE");
  if (p->cur.kind == TK_IF) { advance(p); expect(p, TK_NOT, "expected NOT"); expect(p, TK_EXISTS, "expected EXISTS"); ct->if_not_exists = 1; }
  ct->name = dup_tok(p, &p->cur);
  expect(p, TK_ID, "expected table name");
  expect(p, TK_LP, "expected (");

  int cap = 0;
  do {
    /* table-level constraints */
    if (p->cur.kind == TK_PRIMARY) {
      advance(p); expect(p, TK_KEY, "expected KEY"); expect(p, TK_LP, "expected (");
      ct->pk_cols = parse_name_list(p, &ct->npk);
      expect(p, TK_RP, "expected )");
      continue;
    }
    if (p->cur.kind == TK_UNIQUE || p->cur.kind == TK_FOREIGN ||
        p->cur.kind == TK_CONSTRAINT || p->cur.kind == TK_CHECK ||
        p->cur.kind == TK_PERIOD) {
      /* capture-and-skip these for now (analyzer-level handling later) */
      int depth = 0;
      while (p->cur.kind != TK_EOF) {
        if (p->cur.kind == TK_LP) depth++;
        else if (p->cur.kind == TK_RP) { if (depth == 0) break; depth--; }
        else if (p->cur.kind == TK_COMMA && depth == 0) break;
        advance(p);
      }
      continue;
    }
    /* a column definition */
    tdb_coldef cd; memset(&cd, 0, sizeof(cd));
    cd.coll = TDB_COLL_BINARY;
    cd.name = dup_tok(p, &p->cur);
    expect(p, TK_ID, "expected column name");
    cd.type = parse_type(p);
    parse_column_constraints(p, &cd);
    if (ct->ncol == cap) {
      cap = cap ? cap * 2 : 4;
      tdb_coldef *g = (tdb_coldef *)tdb_arena_alloc(p->a, sizeof(tdb_coldef) * (size_t)cap);
      for (int i = 0; i < ct->ncol; i++) g[i] = ct->cols[i];
      ct->cols = g;
    }
    ct->cols[ct->ncol++] = cd;
  } while (accept(p, TK_COMMA) && !p->err);

  expect(p, TK_RP, "expected )");
  /* table options: WITH SYSTEM VERSIONING and/or WITH COLUMNAR */
  while (accept(p, TK_WITH)) {
    if (p->cur.kind == TK_SYSTEM) {
      advance(p);
      expect(p, TK_VERSIONING, "expected VERSIONING");
      ct->system_versioning = 1;
    } else if (p->cur.kind == TK_ID && p->cur.n == 8 &&
               strncasecmp(p->cur.z, "COLUMNAR", 8) == 0) {
      advance(p);
      ct->columnar = 1;
    } else {
      set_err(p, "expected SYSTEM VERSIONING or COLUMNAR");
      break;
    }
  }
  return s;
}

static tdb_ast_stmt *parse_create_index(P *p, int unique) {
  tdb_ast_stmt *s = new_stmt(p, ST_CREATE_INDEX);
  tdb_create_index *ci = (tdb_create_index *)tdb_arena_alloc(p->a, sizeof(*ci));
  memset(ci, 0, sizeof(*ci));
  s->u.create_index = ci;
  ci->unique = unique;
  expect(p, TK_INDEX, "expected INDEX");
  if (p->cur.kind == TK_IF) { advance(p); expect(p, TK_NOT, "expected NOT"); expect(p, TK_EXISTS, "expected EXISTS"); ci->if_not_exists = 1; }
  ci->name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected index name");
  expect(p, TK_ON, "expected ON");
  ci->table = dup_tok(p, &p->cur); expect(p, TK_ID, "expected table name");
  expect(p, TK_LP, "expected (");
  int cap = 0;
  do {
    if (ci->ncol == cap) {
      cap = cap ? cap * 2 : 4;
      char **gc = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)cap);
      uint8_t *gd = (uint8_t *)tdb_arena_alloc(p->a, (size_t)cap);
      for (int i = 0; i < ci->ncol; i++) { gc[i] = ci->cols[i]; gd[i] = ci->desc[i]; }
      ci->cols = gc; ci->desc = gd;
    }
    ci->cols[ci->ncol] = dup_tok(p, &p->cur);
    expect(p, TK_ID, "expected column name");
    ci->desc[ci->ncol] = (uint8_t)(accept(p, TK_DESC) ? 1 : (accept(p, TK_ASC), 0));
    ci->ncol++;
  } while (accept(p, TK_COMMA) && !p->err);
  expect(p, TK_RP, "expected )");
  return s;
}

static tdb_ast_stmt *parse_create_view(P *p, int materialized) {
  tdb_ast_stmt *s = new_stmt(p, ST_CREATE_VIEW);
  s->u.create_view.materialized = materialized;
  expect(p, TK_VIEW, "expected VIEW");
  if (p->cur.kind == TK_IF) { advance(p); expect(p, TK_NOT, "expected NOT"); expect(p, TK_EXISTS, "expected EXISTS"); s->u.create_view.if_not_exists = 1; }
  s->u.create_view.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected view name");
  expect(p, TK_AS, "expected AS");
  size_t s0 = off(p);
  (void)parse_select(p);
  s->u.create_view.select_sql = span(p, s0, off(p));
  return s;
}

static tdb_ast_stmt *parse_create_routine(P *p, int is_function) {
  tdb_ast_stmt *s = new_stmt(p, ST_CREATE_ROUTINE);
  s->u.create_routine.is_function = is_function;
  advance(p); /* FUNCTION / PROCEDURE */
  s->u.create_routine.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected routine name");
  if (accept(p, TK_LP)) {
    if (p->cur.kind != TK_RP) {
      int cap = 0;
      do {
        char *pn = dup_tok(p, &p->cur);
        expect(p, TK_ID, "expected parameter name");
        (void)parse_type(p); /* parameter type (ignored for Lua) */
        if (s->u.create_routine.nparams == cap) {
          cap = cap ? cap * 2 : 4;
          char **g = (char **)tdb_arena_alloc(p->a, sizeof(char *) * (size_t)cap);
          for (int i = 0; i < s->u.create_routine.nparams; i++) g[i] = s->u.create_routine.params[i];
          s->u.create_routine.params = g;
        }
        s->u.create_routine.params[s->u.create_routine.nparams++] = pn;
      } while (accept(p, TK_COMMA) && !p->err);
    }
    expect(p, TK_RP, "expected )");
  }
  if (accept(p, TK_RETURNS)) (void)parse_type(p);
  expect(p, TK_LANGUAGE, "expected LANGUAGE");
  expect(p, TK_ID, "expected language name (LUA)");
  expect(p, TK_AS, "expected AS");
  if (p->cur.kind != TK_STRING) { set_err(p, "expected $$...$$ body"); return s; }
  s->u.create_routine.lua_src = dupz(p, p->cur.z, p->cur.n);
  advance(p);
  return s;
}

static tdb_ast_stmt *parse_create(P *p) {
  advance(p); /* CREATE */
  if (accept(p, TK_UNIQUE)) return parse_create_index(p, 1);
  if (p->cur.kind == TK_INDEX) return parse_create_index(p, 0);
  if (p->cur.kind == TK_MATERIALIZED) { advance(p); return parse_create_view(p, 1); }
  if (p->cur.kind == TK_VIEW) return parse_create_view(p, 0);
  if (p->cur.kind == TK_FUNCTION) return parse_create_routine(p, 1);
  if (p->cur.kind == TK_PROCEDURE) return parse_create_routine(p, 0);
  /* CREATE [OR REPLACE] FUNCTION not handled via OR REPLACE keyword set; fallthrough */
  return parse_create_table(p);
}

static tdb_ast_stmt *parse_drop(P *p) {
  advance(p); /* DROP */
  tdb_stmt_kind k = ST_DROP_TABLE;
  if (p->cur.kind == TK_INDEX) k = ST_DROP_INDEX;
  else if (p->cur.kind == TK_VIEW) k = ST_DROP_VIEW;
  advance(p); /* TABLE/INDEX/VIEW */
  tdb_ast_stmt *s = new_stmt(p, k);
  if (p->cur.kind == TK_IF) { advance(p); expect(p, TK_EXISTS, "expected EXISTS"); s->u.drop.if_exists = 1; }
  s->u.drop.name = dup_tok(p, &p->cur);
  expect(p, TK_ID, "expected name");
  return s;
}

static tdb_ast_stmt *parse_alter(P *p) {
  advance(p); /* ALTER */
  expect(p, TK_TABLE, "expected TABLE");
  tdb_ast_stmt *s = new_stmt(p, ST_ALTER_TABLE);
  s->u.alter.table = dup_tok(p, &p->cur); expect(p, TK_ID, "expected table name");
  if (accept(p, TK_ADD)) {
    accept(p, TK_COLUMN);
    tdb_coldef *cd = (tdb_coldef *)tdb_arena_alloc(p->a, sizeof(*cd));
    memset(cd, 0, sizeof(*cd)); cd->coll = TDB_COLL_BINARY;
    cd->name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected column name");
    cd->type = parse_type(p);
    parse_column_constraints(p, cd);
    s->u.alter.action = TDB_ALTER_ADD_COLUMN; s->u.alter.add = cd;
  } else if (accept(p, TK_DROP)) {
    accept(p, TK_COLUMN);
    s->u.alter.action = TDB_ALTER_DROP_COLUMN;
    s->u.alter.col = dup_tok(p, &p->cur); expect(p, TK_ID, "expected column name");
  } else if (accept(p, TK_RENAME)) {
    if (accept(p, TK_TO)) { s->u.alter.action = TDB_ALTER_RENAME_TABLE; s->u.alter.newname = dup_tok(p, &p->cur); expect(p, TK_ID, "expected name"); }
    else { accept(p, TK_COLUMN); s->u.alter.action = TDB_ALTER_RENAME_COLUMN; s->u.alter.col = dup_tok(p, &p->cur); expect(p, TK_ID, "expected column"); expect(p, TK_TO, "expected TO"); s->u.alter.newname = dup_tok(p, &p->cur); expect(p, TK_ID, "expected new name"); }
  } else set_err(p, "expected ADD/DROP/RENAME");
  return s;
}

static tdb_ast_stmt *parse_txn(P *p) {
  tdb_token_kind k = p->cur.kind;
  advance(p);
  tdb_ast_stmt *s;
  switch (k) {
    case TK_BEGIN: accept(p, TK_TRANSACTION); return new_stmt(p, ST_BEGIN);
    case TK_COMMIT: accept(p, TK_TRANSACTION); return new_stmt(p, ST_COMMIT);
    case TK_ROLLBACK:
      accept(p, TK_TRANSACTION);
      if (accept(p, TK_TO)) {
        accept(p, TK_SAVEPOINT);
        s = new_stmt(p, ST_ROLLBACK_TO);
        s->u.savepoint.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected savepoint name");
        return s;
      }
      return new_stmt(p, ST_ROLLBACK);
    case TK_SAVEPOINT:
      s = new_stmt(p, ST_SAVEPOINT);
      s->u.savepoint.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected savepoint name");
      return s;
    case TK_RELEASE:
      accept(p, TK_SAVEPOINT);
      s = new_stmt(p, ST_RELEASE);
      s->u.savepoint.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected savepoint name");
      return s;
    default: set_err(p, "bad transaction statement"); return new_stmt(p, ST_BEGIN);
  }
}

static tdb_ast_stmt *parse_call(P *p) {
  advance(p); /* CALL */
  tdb_ast_stmt *s = new_stmt(p, ST_CALL);
  s->u.call.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected routine name");
  s->u.call.args = tdb_exprlist_new(p->a);
  expect(p, TK_LP, "expected (");
  if (p->cur.kind != TK_RP)
    do { tdb_exprlist_add(p->a, s->u.call.args, parse_expr(p, 0), NULL); } while (accept(p, TK_COMMA));
  expect(p, TK_RP, "expected )");
  return s;
}

static tdb_ast_stmt *parse_prepare(P *p) {
  advance(p); /* PREPARE */
  tdb_ast_stmt *s = new_stmt(p, ST_PREPARE);
  s->u.prepare.name = dup_tok(p, &p->cur); expect(p, TK_ID, "expected name");
  expect(p, TK_AS, "expected AS");
  size_t s0 = off(p);
  while (p->cur.kind != TK_SEMI && p->cur.kind != TK_EOF) advance(p);
  s->u.prepare.sql = span(p, s0, off(p));
  return s;
}

static tdb_ast_stmt *parse_stmt(P *p) {
  switch (p->cur.kind) {
    case TK_SELECT: { tdb_ast_stmt *s = new_stmt(p, ST_SELECT); s->u.select = parse_select(p); return s; }
    case TK_INSERT: return parse_insert(p);
    case TK_UPDATE: return parse_update(p);
    case TK_DELETE: return parse_delete(p);
    case TK_CREATE: return parse_create(p);
    case TK_DROP:   return parse_drop(p);
    case TK_ALTER:  return parse_alter(p);
    case TK_CALL:   return parse_call(p);
    case TK_PREPARE:return parse_prepare(p);
    case TK_BEGIN: case TK_COMMIT: case TK_ROLLBACK:
    case TK_SAVEPOINT: case TK_RELEASE: return parse_txn(p);
    default: set_err(p, "unrecognized statement"); return NULL;
  }
}

int tdb_parse(tdb_arena *a, const char *sql, tdb_ast_stmt **out, char **errmsg,
              const char **tail) {
  P p;
  p.a = a; p.sql = sql; p.err = 0; p.errmsg = NULL;
  tdb_lex_init(&p.lx, sql, 0);
  advance(&p);

  if (p.cur.kind == TK_EOF) { if (errmsg) *errmsg = NULL; if (tail) *tail = p.cur.z; if (out) *out = NULL; return TDB_DONE; }

  tdb_ast_stmt *st = parse_stmt(&p);
  if (p.err) {
    if (errmsg) *errmsg = p.errmsg;
    if (tail) *tail = p.cur.z;
    return TDB_ERROR;
  }
  if (p.cur.kind != TK_SEMI && p.cur.kind != TK_EOF) {
    set_err(&p, "unexpected trailing tokens");
    if (errmsg) *errmsg = p.errmsg;
    if (tail) *tail = p.cur.z;
    return TDB_ERROR;
  }
  while (p.cur.kind == TK_SEMI) advance(&p);
  if (tail) *tail = p.cur.z;
  if (out) *out = st;
  return TDB_OK;
}

int tdb_parse_expression(tdb_arena *a, const char *sql, tdb_expr **out,
                         char **errmsg) {
  P p;
  p.a = a; p.sql = sql; p.err = 0; p.errmsg = NULL;
  tdb_lex_init(&p.lx, sql, 0);
  advance(&p);
  tdb_expr *e = parse_expr(&p, 0);
  if (p.err) {
    if (errmsg) *errmsg = p.errmsg;
    return TDB_ERROR;
  }
  if (p.cur.kind != TK_EOF) {
    set_err(&p, "trailing tokens in expression");
    if (errmsg) *errmsg = p.errmsg;
    return TDB_ERROR;
  }
  if (out) *out = e;
  return TDB_OK;
}
