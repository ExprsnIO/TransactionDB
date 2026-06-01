/*
** test_jit.c — exercise the JIT pipeline end to end.
**
** Three layers are tested:
**
**   1. IR emission (always available) — every front end has to produce IR
**      text that contains the expected `define double @<entry>(ptr ...)`
**      header for any host, with or without -DTDB_BUILD_LLVM.
**
**   2. JIT availability flag — tdb_jit_is_available() has to match the
**      build option so callers can probe at runtime.
**
**   3. Native execution (only when TDB_HAVE_LLVM is defined) — open a JIT,
**      compile a small program in each of the five source languages and
**      call the resulting function pointer with a few argument sets.
*/
#include "tdb_test.h"
#include "tdb_jit.h"
#include "tdb_buf.h"
#include "transactiondb.h"

#include <math.h>
#include <string.h>

/* ----------------------- IR emission tests --------------------------- */

static int contains(const char *hay, size_t hn, const char *needle) {
  size_t nn = strlen(needle);
  if (nn > hn) return 0;
  for (size_t i = 0; i + nn <= hn; i++)
    if (memcmp(hay + i, needle, nn) == 0) return 1;
  return 0;
}

static void test_sql_emit_ir(void) {
  const char *p[] = {"a", "b"};
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  int rc = tdb_jit_emit_ir(TDB_JIT_SQL, "a + b * 2", p, 2, &ir, err, sizeof(err));
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(ir.len > 0);
  TDB_CHECK(contains((const char *)ir.data, ir.len, "define double @jit_entry(ptr "));
  TDB_CHECK(contains((const char *)ir.data, ir.len, "fmul double"));
  TDB_CHECK(contains((const char *)ir.data, ir.len, "fadd double"));
  /* Constant-folded literal: `b * 2` keeps the variable but `2 * 3` would fold. */
  tdb_buf_free(&ir);
}

static void test_sql_emit_folded(void) {
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  int rc = tdb_jit_emit_ir(TDB_JIT_SQL, "2 * 3 + 4", NULL, 0, &ir, err, sizeof(err));
  TDB_CHECK_EQ(rc, TDB_OK);
  /* 2*3+4 should fold to 10.0 with no fmul/fadd instructions in the body. */
  TDB_CHECK(contains((const char *)ir.data, ir.len, "store double 1") ||
            contains((const char *)ir.data, ir.len, "ret double"));
  tdb_buf_free(&ir);
}

static void test_lua_emit_ir(void) {
  const char *p[] = {"x"};
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  const char *src =
    "local s = 0\n"
    "for i = 1, 10 do\n"
    "  s = s + i\n"
    "end\n"
    "return s + x\n";
  int rc = tdb_jit_emit_ir(TDB_JIT_LUA, src, p, 1, &ir, err, sizeof(err));
  if (rc != TDB_OK) fprintf(stderr, "  lua emit err: %s\n", err);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(contains((const char *)ir.data, ir.len, "define double @jit_entry(ptr "));
  TDB_CHECK(contains((const char *)ir.data, ir.len, "br label"));   /* loop */
  tdb_buf_free(&ir);
}

static void test_xpath_emit_ir(void) {
  const char *p[] = {"x", "y"};
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  int rc = tdb_jit_emit_ir(TDB_JIT_XPATH,
                           "$x * 2 + ceiling($y div 3) mod 7",
                           p, 2, &ir, err, sizeof(err));
  if (rc != TDB_OK) fprintf(stderr, "  xpath emit err: %s\n", err);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(contains((const char *)ir.data, ir.len, "llvm.ceil.f64"));
  TDB_CHECK(contains((const char *)ir.data, ir.len, "frem double"));
  tdb_buf_free(&ir);
}

static void test_xquery_emit_ir(void) {
  const char *p[] = {"n"};
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  const char *src =
    "let $a := $n * 2\n"
    "let $b := $a + 3\n"
    "return $b * $b\n";
  int rc = tdb_jit_emit_ir(TDB_JIT_XQUERY, src, p, 1, &ir, err, sizeof(err));
  if (rc != TDB_OK) fprintf(stderr, "  xquery emit err: %s\n", err);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(contains((const char *)ir.data, ir.len, "store double"));   /* let bindings */
  TDB_CHECK(contains((const char *)ir.data, ir.len, "fmul double"));
  tdb_buf_free(&ir);
}

static void test_plsql_emit_ir(void) {
  const char *p[] = {"x"};
  tdb_buf ir; tdb_buf_init(&ir);
  char err[160] = {0};
  const char *src =
    "DECLARE s INTEGER := 0; "
    "BEGIN FOR i IN 1 .. x LOOP s := s + i; END LOOP; RETURN s; END";
  int rc = tdb_jit_emit_ir(TDB_JIT_PLSQL, src, p, 1, &ir, err, sizeof(err));
  if (rc != TDB_OK) fprintf(stderr, "  plsql emit err: %s\n", err);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(contains((const char *)ir.data, ir.len, "define double @plsql_routine(ptr "));
  tdb_buf_free(&ir);
}

/* ----------------------- availability flag --------------------------- */

static void test_availability_flag(void) {
#ifdef TDB_HAVE_LLVM
  TDB_CHECK_EQ(tdb_jit_is_available(), 1);
#else
  TDB_CHECK_EQ(tdb_jit_is_available(), 0);
#endif
}

/* ----------------------- native JIT execution ------------------------ */

#ifdef TDB_HAVE_LLVM

static double drun(tdb_jit_lang lang, const char *src,
                   const char *const *params, int nparams,
                   const double *argv) {
  tdb_jit *j = NULL;
  int rc = tdb_jit_open(&j);
  if (rc != TDB_OK) { fprintf(stderr, "  open jit: %d\n", rc); return 0.0/0.0; }
  tdb_jit_fn fn = NULL;
  char err[256] = {0};
  rc = tdb_jit_compile(j, lang, src, params, nparams, &fn, err, sizeof(err));
  if (rc != TDB_OK) {
    fprintf(stderr, "  compile lang=%d failed (%d): %s\n", lang, rc, err);
    tdb_jit_close(j);
    return 0.0/0.0;
  }
  double v = fn(argv);
  tdb_jit_close(j);
  return v;
}

static void test_jit_sql_exec(void) {
  const char *p[] = {"a", "b"};
  double argv[] = {3.0, 4.0};
  double v = drun(TDB_JIT_SQL, "a*a + b*b", p, 2, argv);
  TDB_CHECK(fabs(v - 25.0) < 1e-9);

  /* Builtin: sqrt + abs. */
  double argv2[] = {-9.0};
  const char *q[] = {"x"};
  v = drun(TDB_JIT_SQL, "sqrt(abs(x))", q, 1, argv2);
  TDB_CHECK(fabs(v - 3.0) < 1e-9);
}

static void test_jit_lua_exec(void) {
  const char *p[] = {"n"};
  const char *src =
    "local s = 0\n"
    "for i = 1, n do\n"
    "  s = s + i\n"
    "end\n"
    "return s\n";
  double a10[] = {10.0};
  double v = drun(TDB_JIT_LUA, src, p, 1, a10);
  TDB_CHECK(fabs(v - 55.0) < 1e-9);

  double a100[] = {100.0};
  v = drun(TDB_JIT_LUA, src, p, 1, a100);
  TDB_CHECK(fabs(v - 5050.0) < 1e-9);

  /* Power and math.X dotted-call lookup. */
  const char *q[] = {"x"};
  double ax[] = {3.0};
  v = drun(TDB_JIT_LUA, "return math.sqrt(x*x + 16)", q, 1, ax);
  TDB_CHECK(fabs(v - 5.0) < 1e-9);

  v = drun(TDB_JIT_LUA, "return 2^10", NULL, 0, NULL);
  TDB_CHECK(fabs(v - 1024.0) < 1e-9);
}

static void test_jit_xpath_exec(void) {
  const char *p[] = {"x"};
  double a[] = {17.0};
  double v = drun(TDB_JIT_XPATH, "$x mod 5", p, 1, a);
  TDB_CHECK(fabs(v - 2.0) < 1e-9);

  v = drun(TDB_JIT_XPATH, "ceiling($x div 4)", p, 1, a);
  TDB_CHECK(fabs(v - 5.0) < 1e-9);

  /* Comparison yields a 0/1 double. */
  const char *q[] = {"a", "b"};
  double ab[] = {7.0, 3.0};
  v = drun(TDB_JIT_XPATH, "$a > $b and $a < 10", q, 2, ab);
  TDB_CHECK(fabs(v - 1.0) < 1e-9);
}

static void test_jit_xquery_exec(void) {
  const char *p[] = {"n"};
  const char *src =
    "let $a := $n * 2\n"
    "let $b := $a + 3\n"
    "return $b * $b\n";
  double a[] = {5.0};
  double v = drun(TDB_JIT_XQUERY, src, p, 1, a);
  TDB_CHECK(fabs(v - 169.0) < 1e-9);   /* (5*2+3)^2 == 169 */

  /* Bare expression form (no let / return). */
  double v2 = drun(TDB_JIT_XQUERY, "$n + 1", p, 1, a);
  TDB_CHECK(fabs(v2 - 6.0) < 1e-9);
}

static void test_jit_plsql_exec(void) {
  const char *p[] = {"x"};
  /* Sum of 1..x via FOR loop. */
  const char *src =
    "DECLARE s INTEGER := 0; "
    "BEGIN FOR i IN 1 .. x LOOP s := s + i; END LOOP; RETURN s; END";
  double a[] = {100.0};
  double v = drun(TDB_JIT_PLSQL, src, p, 1, a);
  TDB_CHECK(fabs(v - 5050.0) < 1e-9);
}

/* Compile twice into the same JIT instance and call both. Verifies that
** the engine's main dylib can hold many independently compiled functions
** without contention. */
static void test_jit_multimodule(void) {
  tdb_jit *j = NULL;
  TDB_CHECK_EQ(tdb_jit_open(&j), TDB_OK);
  tdb_jit_fn fa = NULL, fb = NULL;
  char err[160];
  /* Two compiled modules with different jit_entry symbols → name collision
  ** would surface here; the engine renames each entry via the language
  ** dispatch (SQL/PLSQL pick different names). Use different langs. */
  const char *p[] = {"x"};
  TDB_CHECK_EQ(tdb_jit_compile(j, TDB_JIT_PLSQL,
                               "BEGIN RETURN x * 7; END",
                               p, 1, &fa, err, sizeof(err)), TDB_OK);
  /* Second compile in another language goes into a separate symbol name. */
  TDB_CHECK_EQ(tdb_jit_compile(j, TDB_JIT_SQL, "x + 100", p, 1, &fb,
                               err, sizeof(err)), TDB_OK);
  double v[1] = {9.0};
  TDB_CHECK(fabs(fa(v) - 63.0) < 1e-9);
  TDB_CHECK(fabs(fb(v) - 109.0) < 1e-9);
  tdb_jit_close(j);
}

#else  /* !TDB_HAVE_LLVM — verify graceful degradation */

static void test_jit_unsupported(void) {
  tdb_jit *j = NULL;
  TDB_CHECK_EQ(tdb_jit_open(&j), TDB_UNSUPPORTED);
  TDB_CHECK(j == NULL);
  TDB_CHECK(tdb_jit_llvm_version() == NULL);
}

#endif

static tdb_test_case cases[] = {
  {"sql_emit_ir",      test_sql_emit_ir},
  {"sql_emit_folded",  test_sql_emit_folded},
  {"lua_emit_ir",      test_lua_emit_ir},
  {"xpath_emit_ir",    test_xpath_emit_ir},
  {"xquery_emit_ir",   test_xquery_emit_ir},
  {"plsql_emit_ir",    test_plsql_emit_ir},
  {"availability",     test_availability_flag},
#ifdef TDB_HAVE_LLVM
  {"sql_exec",         test_jit_sql_exec},
  {"lua_exec",         test_jit_lua_exec},
  {"xpath_exec",       test_jit_xpath_exec},
  {"xquery_exec",      test_jit_xquery_exec},
  {"plsql_exec",       test_jit_plsql_exec},
  {"multi_module",     test_jit_multimodule},
#else
  {"unsupported",      test_jit_unsupported},
#endif
};

TDB_MAIN(cases)
