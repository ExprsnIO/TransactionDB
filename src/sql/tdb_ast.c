/* tdb_ast.c — AST node constructors and a debug printer. */
#include "tdb_ast.h"
#include "../common/tdb_mem.h"
#include "tdb_lexer.h"

#include <string.h>
#include <stdio.h>

tdb_expr *tdb_expr_new(tdb_arena *a, tdb_expr_kind k) {
  tdb_expr *e = (tdb_expr *)tdb_arena_alloc(a, sizeof(*e));
  if (e) { memset(e, 0, sizeof(*e)); e->kind = k; tdb_value_init(&e->lit); }
  return e;
}

tdb_exprlist *tdb_exprlist_new(tdb_arena *a) {
  tdb_exprlist *l = (tdb_exprlist *)tdb_arena_alloc(a, sizeof(*l));
  if (l) memset(l, 0, sizeof(*l));
  return l;
}

void tdb_exprlist_add(tdb_arena *a, tdb_exprlist *l, tdb_expr *e, char *alias) {
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 4;
    tdb_expr **items = (tdb_expr **)tdb_arena_alloc(a, sizeof(void *) * (size_t)cap);
    char **aliases = (char **)tdb_arena_alloc(a, sizeof(void *) * (size_t)cap);
    for (int i = 0; i < l->n; i++) { items[i] = l->items[i]; aliases[i] = l->aliases[i]; }
    l->items = items; l->aliases = aliases; l->cap = cap;
  }
  l->aliases[l->n] = alias;
  l->items[l->n++] = e;
}

tdb_orderby *tdb_orderby_new(tdb_arena *a) {
  tdb_orderby *o = (tdb_orderby *)tdb_arena_alloc(a, sizeof(*o));
  if (o) memset(o, 0, sizeof(*o));
  return o;
}

void tdb_orderby_add(tdb_arena *a, tdb_orderby *o, tdb_expr *e, int desc) {
  if (o->n == o->cap) {
    int cap = o->cap ? o->cap * 2 : 4;
    tdb_expr **ex = (tdb_expr **)tdb_arena_alloc(a, sizeof(void *) * (size_t)cap);
    uint8_t *ds = (uint8_t *)tdb_arena_alloc(a, (size_t)cap);
    for (int i = 0; i < o->n; i++) { ex[i] = o->exprs[i]; ds[i] = o->desc[i]; }
    o->exprs = ex; o->desc = ds; o->cap = cap;
  }
  o->desc[o->n] = (uint8_t)(desc ? 1 : 0);
  o->exprs[o->n++] = e;
}

/* ------------------------------- printer ------------------------------ */

static const char *op_str(int op) { return tdb_token_name((tdb_token_kind)op); }

void tdb_expr_debug(tdb_buf *out, const tdb_expr *e) {
  if (!e) { tdb_buf_append(out, "NULL", 4); return; }
  char tmp[64];
  switch (e->kind) {
    case EX_NULL: tdb_buf_append(out, "NULL", 4); break;
    case EX_STAR: tdb_buf_append(out, "*", 1); break;
    case EX_LITERAL:
      switch (e->lit.type) {
        case TDB_VAL_INT:
          snprintf(tmp, sizeof(tmp), "%lld", (long long)e->lit.u.i);
          tdb_buf_append(out, tmp, strlen(tmp));
          break;
        case TDB_VAL_REAL:
          snprintf(tmp, sizeof(tmp), "%g", e->lit.u.r);
          tdb_buf_append(out, tmp, strlen(tmp));
          break;
        case TDB_VAL_TEXT:
          tdb_buf_putc(out, '\'');
          tdb_buf_append(out, e->lit.u.s.p, (size_t)e->lit.u.s.n);
          tdb_buf_putc(out, '\'');
          break;
        default: tdb_buf_append(out, "?lit", 4); break;
      }
      break;
    case EX_PARAM:
      tdb_buf_append(out, e->param ? e->param : "?", e->param ? strlen(e->param) : 1);
      break;
    case EX_COLUMN:
      if (e->table) { tdb_buf_append(out, e->table, strlen(e->table)); tdb_buf_putc(out, '.'); }
      tdb_buf_append(out, e->name ? e->name : "?", e->name ? strlen(e->name) : 1);
      break;
    case EX_UNARY:
      tdb_buf_putc(out, '(');
      tdb_buf_append(out, op_str(e->op), strlen(op_str(e->op)));
      tdb_buf_putc(out, ' ');
      tdb_expr_debug(out, e->left);
      tdb_buf_putc(out, ')');
      break;
    case EX_BINARY:
      tdb_buf_putc(out, '(');
      tdb_expr_debug(out, e->left);
      tdb_buf_putc(out, ' ');
      tdb_buf_append(out, op_str(e->op), strlen(op_str(e->op)));
      tdb_buf_putc(out, ' ');
      tdb_expr_debug(out, e->right);
      tdb_buf_putc(out, ')');
      break;
    case EX_FUNC:
    case EX_AGG:
      tdb_buf_append(out, e->name ? e->name : "fn", e->name ? strlen(e->name) : 2);
      tdb_buf_putc(out, '(');
      if (e->distinct) tdb_buf_append(out, "DISTINCT ", 9);
      if (e->args) {
        for (int i = 0; i < e->args->n; i++) {
          if (i) tdb_buf_append(out, ", ", 2);
          tdb_expr_debug(out, e->args->items[i]);
        }
      }
      tdb_buf_putc(out, ')');
      break;
    case EX_CAST:
      tdb_buf_append(out, "CAST(", 5);
      tdb_expr_debug(out, e->left);
      tdb_buf_append(out, " AS ", 4);
      { const char *tn = tdb_typeid_name(e->cast.id); tdb_buf_append(out, tn, strlen(tn)); }
      tdb_buf_putc(out, ')');
      break;
    case EX_IN:
      tdb_buf_putc(out, '(');
      tdb_expr_debug(out, e->left);
      tdb_buf_append(out, e->negated ? " NOT IN (" : " IN (", e->negated ? 9 : 5);
      if (e->args) for (int i = 0; i < e->args->n; i++) {
        if (i) tdb_buf_append(out, ", ", 2);
        tdb_expr_debug(out, e->args->items[i]);
      }
      tdb_buf_append(out, "))", 2);
      break;
    case EX_BETWEEN:
      tdb_buf_putc(out, '(');
      tdb_expr_debug(out, e->left);
      tdb_buf_append(out, e->negated ? " NOT BETWEEN " : " BETWEEN ", e->negated ? 13 : 9);
      if (e->args && e->args->n == 2) {
        tdb_expr_debug(out, e->args->items[0]);
        tdb_buf_append(out, " AND ", 5);
        tdb_expr_debug(out, e->args->items[1]);
      }
      tdb_buf_putc(out, ')');
      break;
    case EX_CASE:
      tdb_buf_append(out, "CASE", 4);
      tdb_buf_putc(out, ')');
      break;
    case EX_EXISTS:
      tdb_buf_append(out, "EXISTS(...)", 11);
      break;
    case EX_SUBQUERY:
      tdb_buf_append(out, "(SELECT ...)", 12);
      break;
  }
}
