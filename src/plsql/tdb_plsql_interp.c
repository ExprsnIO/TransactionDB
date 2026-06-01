/* tdb_plsql_interp.c — tree-walking interpreter for the PL/SQL AST. */
#include "tdb_plsql_int.h"
#include "../value/tdb_type.h"   /* tdb_value_compare / TDB_COLL_BINARY */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  tdb_value *vars;
  int        nvars;
  char       msg[160];
  int        err;
} ENV;

static int eval(ENV *env, const pl_expr *e, tdb_value *out);

static double as_num(const tdb_value *v) { return tdb_value_as_real(v); }
static int    is_int(const tdb_value *v) { return v->type == TDB_VAL_INT; }
static int    truthy(const tdb_value *v) {
  if (v->type == TDB_VAL_NULL) return 0;
  if (v->type == TDB_VAL_TEXT) return v->u.s.n != 0;
  return as_num(v) != 0.0;
}

static void set_err(ENV *env, const char *m) {
  if (env->err) return;
  env->err = 1;
  snprintf(env->msg, sizeof(env->msg), "PL/SQL: %s", m);
}

static int eval_call(ENV *env, const pl_expr *e, tdb_value *out) {
  const char *fn = e->fname;
  double a[4]; int na = e->nargs < 4 ? e->nargs : 4;
  tdb_value av[4];
  for (int i = 0; i < e->nargs; i++) {
    tdb_value v; tdb_value_init(&v);
    if (eval(env, e->args[i], &v)) { tdb_value_clear(&v); return TDB_ERROR; }
    if (i < 4) { av[i] = v; a[i] = as_num(&v); } else tdb_value_clear(&v);
  }
  int rc = TDB_OK;
  if ((!strcasecmp(fn, "abs")) && e->nargs == 1) tdb_value_set_real(out, fabs(a[0]));
  else if (!strcasecmp(fn, "sqrt") && e->nargs == 1) tdb_value_set_real(out, sqrt(a[0]));
  else if (!strcasecmp(fn, "floor") && e->nargs == 1) tdb_value_set_int(out, (int64_t)floor(a[0]));
  else if (!strcasecmp(fn, "ceil") && e->nargs == 1) tdb_value_set_int(out, (int64_t)ceil(a[0]));
  else if ((!strcasecmp(fn, "power") || !strcasecmp(fn, "pow")) && e->nargs == 2) tdb_value_set_real(out, pow(a[0], a[1]));
  else if ((!strcasecmp(fn, "mod")) && e->nargs == 2) tdb_value_set_int(out, (int64_t)a[1] != 0 ? (int64_t)a[0] % (int64_t)a[1] : 0);
  else if ((!strcasecmp(fn, "greatest") || !strcasecmp(fn, "max")) && e->nargs >= 1) {
    double m = a[0]; for (int i = 1; i < na; i++) if (a[i] > m) m = a[i];
    tdb_value_set_real(out, m);
  } else if ((!strcasecmp(fn, "least") || !strcasecmp(fn, "min")) && e->nargs >= 1) {
    double m = a[0]; for (int i = 1; i < na; i++) if (a[i] < m) m = a[i];
    tdb_value_set_real(out, m);
  } else if (!strcasecmp(fn, "round") && (e->nargs == 1 || e->nargs == 2)) {
    int nd = e->nargs == 2 ? (int)a[1] : 0;
    double p = pow(10.0, (double)nd);
    tdb_value_set_real(out, round(a[0] * p) / p);
  } else if (!strcasecmp(fn, "length") && e->nargs == 1) {
    const char *s = tdb_value_as_text(&av[0]);
    tdb_value_set_int(out, s ? (int64_t)strlen(s) : 0);
  } else {
    set_err(env, "unknown PL/SQL function");
    rc = TDB_ERROR;
  }
  for (int i = 0; i < na; i++) tdb_value_clear(&av[i]);
  return rc;
}

static int eval(ENV *env, const pl_expr *e, tdb_value *out) {
  tdb_value_clear(out);
  switch (e->kind) {
    case PL_E_NUM:
      if (e->is_int) tdb_value_set_int(out, (int64_t)e->num);
      else tdb_value_set_real(out, e->num);
      return TDB_OK;
    case PL_E_STR:
      tdb_value_set_text(out, e->str, -1, 1);
      return TDB_OK;
    case PL_E_VAR:
      if (e->slot < 0 || e->slot >= env->nvars) { tdb_value_set_null(out); return TDB_OK; }
      return tdb_value_copy(out, &env->vars[e->slot]);
    case PL_E_CALL:
      return eval_call(env, e, out);
    case PL_E_UNARY: {
      tdb_value v; tdb_value_init(&v);
      if (eval(env, e->l, &v)) { tdb_value_clear(&v); return TDB_ERROR; }
      if (e->op == PL_OP_NOT) tdb_value_set_int(out, !truthy(&v));
      else { /* NEG */
        if (is_int(&v)) tdb_value_set_int(out, -v.u.i);
        else tdb_value_set_real(out, -as_num(&v));
      }
      tdb_value_clear(&v);
      return TDB_OK;
    }
    case PL_E_BINARY: {
      tdb_value l, r; tdb_value_init(&l); tdb_value_init(&r);
      if (eval(env, e->l, &l) || eval(env, e->r, &r)) { tdb_value_clear(&l); tdb_value_clear(&r); return TDB_ERROR; }
      int rc = TDB_OK;
      switch (e->op) {
        case PL_OP_OR:  tdb_value_set_int(out, truthy(&l) || truthy(&r)); break;
        case PL_OP_AND: tdb_value_set_int(out, truthy(&l) && truthy(&r)); break;
        case PL_OP_CONCAT: {
          const char *ls = tdb_value_as_text(&l), *rs = tdb_value_as_text(&r);
          size_t ln = ls ? strlen(ls) : 0, rn = rs ? strlen(rs) : 0;
          char *buf = (char *)tdb_malloc(ln + rn + 1);
          if (ls) memcpy(buf, ls, ln);
          if (rs) memcpy(buf + ln, rs, rn);
          buf[ln + rn] = '\0';
          tdb_value_set_text(out, buf, (int)(ln + rn), 1);
          tdb_mfree(buf);
          break;
        }
        case PL_OP_EQ: case PL_OP_NE: case PL_OP_LT:
        case PL_OP_LE: case PL_OP_GT: case PL_OP_GE: {
          int c = tdb_value_compare(&l, &r, TDB_COLL_BINARY);
          int res = 0;
          switch (e->op) {
            case PL_OP_EQ: res = c == 0; break;
            case PL_OP_NE: res = c != 0; break;
            case PL_OP_LT: res = c <  0; break;
            case PL_OP_LE: res = c <= 0; break;
            case PL_OP_GT: res = c >  0; break;
            default:       res = c >= 0; break;
          }
          tdb_value_set_int(out, res);
          break;
        }
        default: { /* arithmetic */
          if (e->op == PL_OP_DIV) {
            double rv = as_num(&r);
            if (rv == 0.0) { set_err(env, "division by zero"); rc = TDB_ERROR; }
            else tdb_value_set_real(out, as_num(&l) / rv);
          } else if (e->op == PL_OP_MOD) {
            int64_t rv = tdb_value_as_int(&r);
            if (rv == 0) { set_err(env, "modulo by zero"); rc = TDB_ERROR; }
            else tdb_value_set_int(out, tdb_value_as_int(&l) % rv);
          } else if (is_int(&l) && is_int(&r)) {
            int64_t lv = l.u.i, rv = r.u.i, res = 0;
            if (e->op == PL_OP_ADD) res = lv + rv;
            else if (e->op == PL_OP_SUB) res = lv - rv;
            else res = lv * rv;
            tdb_value_set_int(out, res);
          } else {
            double lv = as_num(&l), rv = as_num(&r), res = 0;
            if (e->op == PL_OP_ADD) res = lv + rv;
            else if (e->op == PL_OP_SUB) res = lv - rv;
            else res = lv * rv;
            tdb_value_set_real(out, res);
          }
          break;
        }
      }
      tdb_value_clear(&l); tdb_value_clear(&r);
      return rc;
    }
  }
  tdb_value_set_null(out);
  return TDB_OK;
}

/* returns: 0 normal, 1 returned (result set), -1 error */
static int exec_block(ENV *env, pl_stmt **stmts, int n, tdb_value *result);

static int exec_stmt(ENV *env, const pl_stmt *s, tdb_value *result) {
  switch (s->kind) {
    case PL_S_NULL: return 0;
    case PL_S_ASSIGN: {
      tdb_value v; tdb_value_init(&v);
      if (eval(env, s->e1, &v)) { tdb_value_clear(&v); return -1; }
      if (s->slot >= 0 && s->slot < env->nvars) {
        tdb_value_clear(&env->vars[s->slot]);
        env->vars[s->slot] = v;
      } else tdb_value_clear(&v);
      return 0;
    }
    case PL_S_RETURN: {
      if (s->e1) { if (eval(env, s->e1, result)) return -1; }
      else tdb_value_set_null(result);
      return 1;
    }
    case PL_S_IF: {
      for (int i = 0; i < s->nclause; i++) {
        tdb_value c; tdb_value_init(&c);
        if (eval(env, s->clauses[i].cond, &c)) { tdb_value_clear(&c); return -1; }
        int t = truthy(&c); tdb_value_clear(&c);
        if (t) return exec_block(env, s->clauses[i].body, s->clauses[i].nbody, result);
      }
      return exec_block(env, s->elsebody, s->nelse, result);
    }
    case PL_S_WHILE: {
      int guard = 0;
      for (;;) {
        tdb_value c; tdb_value_init(&c);
        if (eval(env, s->e1, &c)) { tdb_value_clear(&c); return -1; }
        int t = truthy(&c); tdb_value_clear(&c);
        if (!t) break;
        int r = exec_block(env, s->body, s->nbody, result);
        if (r != 0) return r;
        if (++guard > 100000000) { set_err(env, "loop iteration limit exceeded"); return -1; }
      }
      return 0;
    }
    case PL_S_FOR: {
      tdb_value lo, hi; tdb_value_init(&lo); tdb_value_init(&hi);
      if (eval(env, s->e1, &lo) || eval(env, s->e2, &hi)) { tdb_value_clear(&lo); tdb_value_clear(&hi); return -1; }
      int64_t a = tdb_value_as_int(&lo), b = tdb_value_as_int(&hi);
      tdb_value_clear(&lo); tdb_value_clear(&hi);
      for (int64_t i = a; i <= b; i++) {
        if (s->slot >= 0 && s->slot < env->nvars) tdb_value_set_int(&env->vars[s->slot], i);
        int r = exec_block(env, s->body, s->nbody, result);
        if (r != 0) return r;
      }
      return 0;
    }
  }
  return 0;
}

static int exec_block(ENV *env, pl_stmt **stmts, int n, tdb_value *result) {
  for (int i = 0; i < n; i++) {
    int r = exec_stmt(env, stmts[i], result);
    if (r != 0) return r;
  }
  return 0;
}

int tdb_plsql_exec(const tdb_plsql_proc *p, const tdb_value *argv, int argc,
                   tdb_value *result, char *errbuf, int errlen) {
  if (!p || !result) return TDB_MISUSE;
  if (argc != p->nparams) {
    if (errbuf && errlen > 0) snprintf(errbuf, (size_t)errlen, "PL/SQL: wrong number of arguments");
    return TDB_ERROR;
  }
  ENV env; memset(&env, 0, sizeof(env));
  env.nvars = p->nslots;
  env.vars = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)(p->nslots ? p->nslots : 1));
  if (!env.vars) return TDB_NOMEM;
  for (int i = 0; i < p->nslots; i++) tdb_value_init(&env.vars[i]);
  for (int i = 0; i < argc; i++) tdb_value_copy(&env.vars[i], &argv[i]);

  tdb_value_init(result);
  int r = exec_block(&env, p->stmts, p->nstmt, result);

  for (int i = 0; i < p->nslots; i++) tdb_value_clear(&env.vars[i]);
  tdb_mfree(env.vars);

  if (r < 0) {
    tdb_value_clear(result);
    if (errbuf && errlen > 0) snprintf(errbuf, (size_t)errlen, "%s", env.msg);
    return TDB_ERROR;
  }
  return TDB_OK;
}
