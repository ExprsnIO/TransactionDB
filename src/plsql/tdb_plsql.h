/*
** tdb_plsql.h — a small PL/SQL routine engine.
**
** This compiles a PL/SQL-flavoured procedural body (DECLARE / BEGIN / END with
** assignment, IF/ELSIF/ELSE, WHILE, FOR..LOOP and RETURN) into an AST that can
** be either:
**   - interpreted directly over `tdb_value` arguments (tdb_plsql_exec), or
**   - lowered to textual LLVM IR (tdb_plsql_emit_llvm) for a numeric routine,
**     after a constant-folding optimization pass.
**
** The engine is deliberately self-contained: it has its own tokenizer (PL/SQL
** uses `:=` and `..` which the SQL lexer does not) and depends only on the
** value type, the growable buffer and the arena allocator. Stored procedures
** and functions (incl. LANGUAGE PLSQL routines) build on this.
*/
#ifndef TDB_PLSQL_H
#define TDB_PLSQL_H

#include "tdb_value.h"
#include "../common/tdb_buf.h"

typedef struct tdb_plsql_proc tdb_plsql_proc; /* opaque compiled routine */

/*
** Parse a routine body. `params` lists the positional formal parameter names
** (may be NULL when nparams == 0); they occupy the first variable slots. On
** success *out receives a heap-allocated routine (free with tdb_plsql_free).
** On failure returns a non-OK status and, if errbuf != NULL, a description.
*/
int tdb_plsql_parse(const char *src, const char *const *params, int nparams,
                    tdb_plsql_proc **out, char *errbuf, int errlen);

void tdb_plsql_free(tdb_plsql_proc *p);

/*
** Interpret the routine. `argv[0..argc)` supply the parameter values (argc must
** equal the declared parameter count). The RETURN value — or NULL when the body
** falls through without RETURN — is written to *result (caller owns it and must
** tdb_value_clear it). Returns TDB_OK or a non-OK status with *errbuf set.
*/
int tdb_plsql_exec(const tdb_plsql_proc *p, const tdb_value *argv, int argc,
                   tdb_value *result, char *errbuf, int errlen);

/*
** Lower a *numeric* routine to textual LLVM IR (every parameter, variable and
** the return value is treated as an IR `double`). A constant-folding pass runs
** first, so e.g. `RETURN 2*3+x;` emits `fadd double 6.0, %x`. The IR text is
** appended to `out`. Returns TDB_OK, or TDB_UNSUPPORTED when the body uses a
** construct outside the numeric subset (e.g. string values).
*/
int tdb_plsql_emit_llvm(const tdb_plsql_proc *p, tdb_buf *out,
                        char *errbuf, int errlen);

/* Number of declared parameters (for arity checks at the call site). */
int tdb_plsql_param_count(const tdb_plsql_proc *p);

#endif /* TDB_PLSQL_H */
