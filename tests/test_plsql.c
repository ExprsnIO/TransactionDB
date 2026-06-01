/* test_plsql.c — PL/SQL parser, interpreter and LLVM IR emitter. */
#include "tdb_test.h"
#include "tdb_plsql.h"
#include "tdb_value.h"
#include "tdb_buf.h"

#include <string.h>

/* helper: parse + interpret a routine, returning the result as a double */
static int run_num(const char *src, const char *const *params, int nparams,
                   const tdb_value *argv, int argc, double *out) {
  tdb_plsql_proc *p = NULL;
  char err[160] = {0};
  if (tdb_plsql_parse(src, params, nparams, &p, err, sizeof(err)) != TDB_OK) return TDB_ERROR;
  tdb_value r; tdb_value_init(&r);
  int rc = tdb_plsql_exec(p, argv, argc, &r, err, sizeof(err));
  if (rc == TDB_OK) *out = tdb_value_as_real(&r);
  tdb_value_clear(&r);
  tdb_plsql_free(p);
  return rc;
}

static void test_return_const(void) {
  double v = 0;
  TDB_CHECK_EQ(run_num("BEGIN RETURN 2 * 3 + 4; END", NULL, 0, NULL, 0, &v), TDB_OK);
  TDB_CHECK(v == 10.0);
}

static void test_params_and_decl(void) {
  const char *params[] = {"x", "y"};
  tdb_value argv[2];
  tdb_value_init(&argv[0]); tdb_value_set_int(&argv[0], 7);
  tdb_value_init(&argv[1]); tdb_value_set_int(&argv[1], 5);
  double v = 0;
  int rc = run_num(
      "DECLARE z INTEGER := 0; BEGIN z := x * y; RETURN z + 1; END",
      params, 2, argv, 2, &v);
  tdb_value_clear(&argv[0]); tdb_value_clear(&argv[1]);
  TDB_CHECK_EQ(rc, TDB_OK);
  TDB_CHECK(v == 36.0);
}

static void test_if_else(void) {
  const char *params[] = {"n"};
  double v = 0;
  const char *src =
      "BEGIN IF n < 0 THEN RETURN -1; ELSIF n = 0 THEN RETURN 0; ELSE RETURN 1; END IF; END";
  tdb_value a; tdb_value_init(&a);
  tdb_value_set_int(&a, -8);
  TDB_CHECK_EQ(run_num(src, params, 1, &a, 1, &v), TDB_OK);
  TDB_CHECK(v == -1.0);
  tdb_value_set_int(&a, 42);
  TDB_CHECK_EQ(run_num(src, params, 1, &a, 1, &v), TDB_OK);
  TDB_CHECK(v == 1.0);
  tdb_value_clear(&a);
}

static void test_while_loop(void) {
  /* factorial via WHILE */
  const char *params[] = {"n"};
  const char *src =
      "DECLARE f INTEGER := 1; i INTEGER := 1;"
      "BEGIN WHILE i <= n LOOP f := f * i; i := i + 1; END LOOP; RETURN f; END";
  tdb_value a; tdb_value_init(&a); tdb_value_set_int(&a, 5);
  double v = 0;
  TDB_CHECK_EQ(run_num(src, params, 1, &a, 1, &v), TDB_OK);
  TDB_CHECK(v == 120.0);
  tdb_value_clear(&a);
}

static void test_for_loop(void) {
  /* sum 1..n via FOR */
  const char *params[] = {"n"};
  const char *src =
      "DECLARE s INTEGER := 0; BEGIN FOR i IN 1 .. n LOOP s := s + i; END LOOP; RETURN s; END";
  tdb_value a; tdb_value_init(&a); tdb_value_set_int(&a, 10);
  double v = 0;
  TDB_CHECK_EQ(run_num(src, params, 1, &a, 1, &v), TDB_OK);
  TDB_CHECK(v == 55.0);
  tdb_value_clear(&a);
}

static void test_builtins(void) {
  double v = 0;
  TDB_CHECK_EQ(run_num("BEGIN RETURN abs(-9) + sqrt(16); END", NULL, 0, NULL, 0, &v), TDB_OK);
  TDB_CHECK(v == 13.0);
  TDB_CHECK_EQ(run_num("BEGIN RETURN power(2, 10); END", NULL, 0, NULL, 0, &v), TDB_OK);
  TDB_CHECK(v == 1024.0);
}

static void test_div_by_zero(void) {
  double v = 0;
  TDB_CHECK(run_num("BEGIN RETURN 1 / 0; END", NULL, 0, NULL, 0, &v) != TDB_OK);
}

static void test_parse_error(void) {
  tdb_plsql_proc *p = NULL;
  char err[160] = {0};
  /* missing END IF */
  int rc = tdb_plsql_parse("BEGIN IF 1 THEN RETURN 1; END", NULL, 0, &p, err, sizeof(err));
  TDB_CHECK(rc != TDB_OK);
  TDB_CHECK(err[0] != '\0');
}

static void test_llvm_const_fold(void) {
  tdb_plsql_proc *p = NULL;
  char err[160] = {0};
  const char *params[] = {"x"};
  TDB_CHECK_EQ(tdb_plsql_parse("BEGIN RETURN 2 * 3 + x; END", params, 1, &p, err, sizeof(err)), TDB_OK);
  tdb_buf ir; tdb_buf_init(&ir);
  TDB_CHECK_EQ(tdb_plsql_emit_llvm(p, &ir, err, sizeof(err)), TDB_OK);
  tdb_buf_putc(&ir, 0);
  const char *s = (const char *)ir.data;
  /* signature + folded constant 6 + an fadd with the parameter load */
  TDB_CHECK(strstr(s, "define double @plsql_routine(double %arg0)") != NULL);
  TDB_CHECK(strstr(s, "fadd double 6") != NULL);
  TDB_CHECK(strstr(s, "ret double") != NULL);
  tdb_buf_free(&ir);
  tdb_plsql_free(p);
}

static void test_llvm_control_flow(void) {
  tdb_plsql_proc *p = NULL;
  char err[160] = {0};
  const char *params[] = {"n"};
  const char *src =
      "DECLARE s INTEGER := 0; BEGIN FOR i IN 1 .. n LOOP s := s + i; END LOOP; RETURN s; END";
  TDB_CHECK_EQ(tdb_plsql_parse(src, params, 1, &p, err, sizeof(err)), TDB_OK);
  tdb_buf ir; tdb_buf_init(&ir);
  TDB_CHECK_EQ(tdb_plsql_emit_llvm(p, &ir, err, sizeof(err)), TDB_OK);
  tdb_buf_putc(&ir, 0);
  const char *s = (const char *)ir.data;
  TDB_CHECK(strstr(s, "fcmp ole double") != NULL);   /* loop guard */
  TDB_CHECK(strstr(s, "br i1") != NULL);
  TDB_CHECK(strstr(s, "alloca double") != NULL);
  tdb_buf_free(&ir);
  tdb_plsql_free(p);
}

static void test_llvm_unsupported(void) {
  tdb_plsql_proc *p = NULL;
  char err[160] = {0};
  const char *params[] = {"a", "b"};
  TDB_CHECK_EQ(tdb_plsql_parse("BEGIN RETURN a || b; END", params, 2, &p, err, sizeof(err)), TDB_OK);
  tdb_buf ir; tdb_buf_init(&ir);
  TDB_CHECK_EQ(tdb_plsql_emit_llvm(p, &ir, err, sizeof(err)), TDB_UNSUPPORTED);
  tdb_buf_free(&ir);
  tdb_plsql_free(p);
}

static const tdb_test_case cases[] = {
  {"return_const", test_return_const},
  {"params_and_decl", test_params_and_decl},
  {"if_else", test_if_else},
  {"while_loop", test_while_loop},
  {"for_loop", test_for_loop},
  {"builtins", test_builtins},
  {"div_by_zero", test_div_by_zero},
  {"parse_error", test_parse_error},
  {"llvm_const_fold", test_llvm_const_fold},
  {"llvm_control_flow", test_llvm_control_flow},
  {"llvm_unsupported", test_llvm_unsupported},
};
TDB_MAIN(cases)
