/*
** tdb_plsql_llvm.c — lower a numeric PL/SQL routine to textual LLVM IR.
**
** Every variable, parameter and the return value is modelled as an IR `double`
** held in an `alloca` slot (opaque-pointer form). A constant-folding pass runs
** over the expression trees first, so literal arithmetic is collapsed before a
** single instruction is emitted. Control flow (IF/ELSIF/ELSE, WHILE, FOR..LOOP,
** RETURN) lowers to basic blocks with explicit branches. The result is a valid
** LLVM module that an out-of-tree `llc`/`clang` (or libLLVM, when built with
** TDB_BUILD_LLVM) can optimize and JIT.
*/
#include "tdb_plsql_int.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------- constant folding --------------------------- */

static pl_expr *mk_num(tdb_arena *a, double v) {
  pl_expr *e = (pl_expr *)tdb_arena_alloc(a, sizeof(*e));
  memset(e, 0, sizeof(*e));
  e->kind = PL_E_NUM; e->num = v; e->is_int = (v == floor(v));
  return e;
}

static const pl_expr *fold(tdb_arena *a, const pl_expr *e) {
  if (!e) return e;
  if (e->kind == PL_E_UNARY) {
    const pl_expr *c = fold(a, e->l);
    if (e->op == PL_OP_NEG && c->kind == PL_E_NUM) return mk_num(a, -c->num);
    if (c == e->l) return e;
    pl_expr *n = (pl_expr *)tdb_arena_alloc(a, sizeof(*n)); *n = *e; n->l = (pl_expr *)c;
    return n;
  }
  if (e->kind == PL_E_BINARY) {
    const pl_expr *l = fold(a, e->l), *r = fold(a, e->r);
    if (l->kind == PL_E_NUM && r->kind == PL_E_NUM) {
      double x = l->num, y = r->num;
      switch (e->op) {
        case PL_OP_ADD: return mk_num(a, x + y);
        case PL_OP_SUB: return mk_num(a, x - y);
        case PL_OP_MUL: return mk_num(a, x * y);
        case PL_OP_DIV: if (y != 0.0) return mk_num(a, x / y); break;
        case PL_OP_MOD: if (y != 0.0) return mk_num(a, fmod(x, y)); break;
        default: break;
      }
    }
    if (l == e->l && r == e->r) return e;
    pl_expr *n = (pl_expr *)tdb_arena_alloc(a, sizeof(*n)); *n = *e;
    n->l = (pl_expr *)l; n->r = (pl_expr *)r;
    return n;
  }
  return e;
}

/* ------------------------------ emitter ------------------------------- */

typedef struct {
  tdb_buf   *out;
  tdb_arena *a;
  int        reg;
  int        lbl;
  int        terminated;
  int        failed;
  char       err[160];
} E;

static void emit(E *e, const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) tdb_buf_append(e->out, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

static void fail(E *e, const char *m) {
  if (e->failed) return;
  e->failed = 1;
  snprintf(e->err, sizeof(e->err), "%s", m);
}

/* format a double as a valid LLVM floating constant */
static const char *fmtd(E *e, double v) {
  char b[40];
  snprintf(b, sizeof(b), "%.17g", v);
  if (!strpbrk(b, ".eEnN")) {  /* ensure it reads as a float, not an int */
    size_t l = strlen(b);
    snprintf(b + l, sizeof(b) - l, ".0");
  }
  return tdb_arena_strndup(e->a, b, strlen(b));
}

static const char *reg(E *e) {
  char b[24]; snprintf(b, sizeof(b), "%%t%d", e->reg++);
  return tdb_arena_strndup(e->a, b, strlen(b));
}

static int new_label(E *e) { return e->lbl++; }
static void label(E *e, int n) { emit(e, "L%d:\n", n); e->terminated = 0; }
static void br(E *e, int n) { emit(e, "  br label %%L%d\n", n); e->terminated = 1; }
static void br_cond(E *e, const char *c, int t, int f) {
  emit(e, "  br i1 %s, label %%L%d, label %%L%d\n", c, t, f); e->terminated = 1;
}
/* open a fresh (possibly dead) block if the previous one was terminated */
static void ensure_open(E *e) { if (e->terminated) label(e, new_label(e)); }

static const char *emit_num(E *e, const pl_expr *x);

/* emit `x` as an i1 boolean operand */
static const char *emit_bool(E *e, const pl_expr *x) {
  x = fold(e->a, x);
  if (x->kind == PL_E_BINARY) {
    switch (x->op) {
      case PL_OP_EQ: case PL_OP_NE: case PL_OP_LT:
      case PL_OP_LE: case PL_OP_GT: case PL_OP_GE: {
        const char *a = emit_num(e, x->l), *b = emit_num(e, x->r);
        const char *pred = x->op == PL_OP_EQ ? "oeq" : x->op == PL_OP_NE ? "une"
                         : x->op == PL_OP_LT ? "olt" : x->op == PL_OP_LE ? "ole"
                         : x->op == PL_OP_GT ? "ogt" : "oge";
        const char *r = reg(e);
        emit(e, "  %s = fcmp %s double %s, %s\n", r, pred, a, b);
        return r;
      }
      case PL_OP_AND: case PL_OP_OR: {
        const char *a = emit_bool(e, x->l), *b = emit_bool(e, x->r);
        const char *r = reg(e);
        emit(e, "  %s = %s i1 %s, %s\n", r, x->op == PL_OP_AND ? "and" : "or", a, b);
        return r;
      }
      default: break;
    }
  }
  if (x->kind == PL_E_UNARY && x->op == PL_OP_NOT) {
    const char *a = emit_bool(e, x->l);
    const char *r = reg(e);
    emit(e, "  %s = xor i1 %s, true\n", r, a);
    return r;
  }
  /* numeric value used as a truth test: (v != 0.0) */
  const char *v = emit_num(e, x);
  const char *r = reg(e);
  emit(e, "  %s = fcmp une double %s, 0.0\n", r, v);
  return r;
}

static const char *emit_call(E *e, const pl_expr *x) {
  const char *fn = x->fname;
  const char *intr = NULL;
  if (!strcasecmp(fn, "abs")) intr = "llvm.fabs.f64";
  else if (!strcasecmp(fn, "sqrt")) intr = "llvm.sqrt.f64";
  else if (!strcasecmp(fn, "floor")) intr = "llvm.floor.f64";
  else if (!strcasecmp(fn, "ceil")) intr = "llvm.ceil.f64";
  else if (!strcasecmp(fn, "round") && x->nargs == 1) intr = "llvm.round.f64";
  if (intr && x->nargs == 1) {
    const char *a = emit_num(e, x->args[0]);
    const char *r = reg(e);
    emit(e, "  %s = call double @%s(double %s)\n", r, intr, a);
    return r;
  }
  if ((!strcasecmp(fn, "power") || !strcasecmp(fn, "pow")) && x->nargs == 2) {
    const char *a = emit_num(e, x->args[0]), *b = emit_num(e, x->args[1]);
    const char *r = reg(e);
    emit(e, "  %s = call double @llvm.pow.f64(double %s, double %s)\n", r, a, b);
    return r;
  }
  if ((!strcasecmp(fn, "max") || !strcasecmp(fn, "greatest") ||
       !strcasecmp(fn, "min") || !strcasecmp(fn, "least")) && x->nargs == 2) {
    int gt = !strcasecmp(fn, "max") || !strcasecmp(fn, "greatest");
    const char *a = emit_num(e, x->args[0]), *b = emit_num(e, x->args[1]);
    const char *c = reg(e);
    emit(e, "  %s = fcmp %s double %s, %s\n", c, gt ? "ogt" : "olt", a, b);
    const char *r = reg(e);
    emit(e, "  %s = select i1 %s, double %s, double %s\n", r, c, a, b);
    return r;
  }
  fail(e, "function not supported in LLVM lowering");
  return "0.0";
}

/* emit `x` as a double operand */
static const char *emit_num(E *e, const pl_expr *x) {
  x = fold(e->a, x);
  switch (x->kind) {
    case PL_E_NUM: return fmtd(e, x->num);
    case PL_E_STR: fail(e, "string values are not supported in LLVM lowering"); return "0.0";
    case PL_E_VAR: {
      const char *r = reg(e);
      emit(e, "  %s = load double, ptr %%s%d\n", r, x->slot);
      return r;
    }
    case PL_E_CALL: return emit_call(e, x);
    case PL_E_UNARY:
      if (x->op == PL_OP_NEG) {
        const char *a = emit_num(e, x->l);
        const char *r = reg(e);
        emit(e, "  %s = fneg double %s\n", r, a);
        return r;
      }
      /* NOT in numeric context: (x ? 0.0 : 1.0) -> uitofp of !bool */
      { const char *b = emit_bool(e, x); const char *r = reg(e);
        emit(e, "  %s = uitofp i1 %s to double\n", r, b); return r; }
    case PL_E_BINARY:
      switch (x->op) {
        case PL_OP_ADD: case PL_OP_SUB: case PL_OP_MUL:
        case PL_OP_DIV: case PL_OP_MOD: {
          const char *a = emit_num(e, x->l), *b = emit_num(e, x->r);
          const char *opn = x->op == PL_OP_ADD ? "fadd" : x->op == PL_OP_SUB ? "fsub"
                          : x->op == PL_OP_MUL ? "fmul" : x->op == PL_OP_DIV ? "fdiv" : "frem";
          const char *r = reg(e);
          emit(e, "  %s = %s double %s, %s\n", r, opn, a, b);
          return r;
        }
        case PL_OP_CONCAT:
          fail(e, "string concatenation is not supported in LLVM lowering");
          return "0.0";
        default: { /* comparison / logical used as a number */
          const char *b = emit_bool(e, x);
          const char *r = reg(e);
          emit(e, "  %s = uitofp i1 %s to double\n", r, b);
          return r;
        }
      }
  }
  return "0.0";
}

static void emit_block(E *e, pl_stmt **stmts, int n, int exit_lbl);

static void emit_stmt(E *e, const pl_stmt *s, int exit_lbl) {
  if (e->failed || e->terminated) { if (e->terminated) ensure_open(e); }
  switch (s->kind) {
    case PL_S_NULL: break;
    case PL_S_ASSIGN: {
      const char *v = emit_num(e, s->e1);
      emit(e, "  store double %s, ptr %%s%d\n", v, s->slot);
      break;
    }
    case PL_S_RETURN: {
      const char *v = s->e1 ? emit_num(e, s->e1) : "0.0";
      emit(e, "  store double %s, ptr %%sret\n", v);
      br(e, exit_lbl);
      break;
    }
    case PL_S_IF: {
      int end = new_label(e);
      for (int i = 0; i < s->nclause; i++) {
        const char *c = emit_bool(e, s->clauses[i].cond);
        int then_l = new_label(e), next_l = new_label(e);
        br_cond(e, c, then_l, next_l);
        label(e, then_l);
        emit_block(e, s->clauses[i].body, s->clauses[i].nbody, exit_lbl);
        if (!e->terminated) br(e, end);
        label(e, next_l);
      }
      emit_block(e, s->elsebody, s->nelse, exit_lbl);
      if (!e->terminated) br(e, end);
      label(e, end);
      break;
    }
    case PL_S_WHILE: {
      int cond_l = new_label(e), body_l = new_label(e), end_l = new_label(e);
      br(e, cond_l);
      label(e, cond_l);
      const char *c = emit_bool(e, s->e1);
      br_cond(e, c, body_l, end_l);
      label(e, body_l);
      emit_block(e, s->body, s->nbody, exit_lbl);
      if (!e->terminated) br(e, cond_l);
      label(e, end_l);
      break;
    }
    case PL_S_FOR: {
      const char *lo = emit_num(e, s->e1);
      emit(e, "  store double %s, ptr %%s%d\n", lo, s->slot);
      const char *hi = emit_num(e, s->e2);
      emit(e, "  store double %s, ptr %%sforhi\n", hi);
      int cond_l = new_label(e), body_l = new_label(e), end_l = new_label(e);
      br(e, cond_l);
      label(e, cond_l);
      const char *cur = reg(e); emit(e, "  %s = load double, ptr %%s%d\n", cur, s->slot);
      const char *h = reg(e);   emit(e, "  %s = load double, ptr %%sforhi\n", h);
      const char *c = reg(e);   emit(e, "  %s = fcmp ole double %s, %s\n", c, cur, h);
      br_cond(e, c, body_l, end_l);
      label(e, body_l);
      emit_block(e, s->body, s->nbody, exit_lbl);
      if (!e->terminated) {
        const char *cv = reg(e); emit(e, "  %s = load double, ptr %%s%d\n", cv, s->slot);
        const char *nx = reg(e); emit(e, "  %s = fadd double %s, 1.0\n", nx, cv);
        emit(e, "  store double %s, ptr %%s%d\n", nx, s->slot);
        br(e, cond_l);
      }
      label(e, end_l);
      break;
    }
  }
}

static void emit_block(E *e, pl_stmt **stmts, int n, int exit_lbl) {
  for (int i = 0; i < n && !e->failed; i++) {
    if (e->terminated) ensure_open(e);   /* dead code after a RETURN */
    emit_stmt(e, stmts[i], exit_lbl);
  }
}

int tdb_plsql_emit_llvm(const tdb_plsql_proc *p, tdb_buf *out,
                        char *errbuf, int errlen) {
  if (!p || !out) return TDB_MISUSE;
  tdb_arena *scratch = tdb_arena_new(4096);
  if (!scratch) return TDB_NOMEM;

  E e; memset(&e, 0, sizeof(e));
  e.out = out; e.a = scratch;

  /* signature */
  emit(&e, "define double @plsql_routine(");
  for (int i = 0; i < p->nparams; i++) emit(&e, "%sdouble %%arg%d", i ? ", " : "", i);
  emit(&e, ") {\n");
  label(&e, new_label(&e));   /* entry block */

  /* return slot + for-loop bound scratch */
  emit(&e, "  %%sret = alloca double\n");
  emit(&e, "  store double 0.0, ptr %%sret\n");
  emit(&e, "  %%sforhi = alloca double\n");

  /* variable slots */
  for (int i = 0; i < p->nslots; i++) {
    emit(&e, "  %%s%d = alloca double\n", i);
    if (i < p->nparams) emit(&e, "  store double %%arg%d, ptr %%s%d\n", i, i);
    else emit(&e, "  store double 0.0, ptr %%s%d\n", i);
  }

  int exit_l = new_label(&e);
  emit_block(&e, p->stmts, p->nstmt, exit_l);
  if (!e.terminated) br(&e, exit_l);

  label(&e, exit_l);
  const char *rv = reg(&e);
  emit(&e, "  %s = load double, ptr %%sret\n", rv);
  emit(&e, "  ret double %s\n", rv);
  emit(&e, "}\n");

  /* intrinsic declarations (harmless if unused) */
  emit(&e, "declare double @llvm.fabs.f64(double)\n");
  emit(&e, "declare double @llvm.sqrt.f64(double)\n");
  emit(&e, "declare double @llvm.floor.f64(double)\n");
  emit(&e, "declare double @llvm.ceil.f64(double)\n");
  emit(&e, "declare double @llvm.round.f64(double)\n");
  emit(&e, "declare double @llvm.pow.f64(double, double)\n");

  int rc = TDB_OK;
  if (e.failed) {
    if (errbuf && errlen > 0) snprintf(errbuf, (size_t)errlen, "%s", e.err);
    rc = TDB_UNSUPPORTED;
  }
  tdb_arena_free(scratch);
  return rc;
}
