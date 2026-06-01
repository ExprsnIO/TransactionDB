/*
** tdb_jit_int.h — internals shared between the JIT engine and the per-language
** front ends. Not a public header.
**
** The five front ends (SQL, PL/SQL, Lua, XPath, XQuery) all build the same
** AST — PL/SQL's `pl_expr` / `pl_stmt` nodes (see ../plsql/tdb_plsql_int.h) —
** so we get one constant-folder, one IR emitter and one set of tests instead
** of five copies.
*/
#ifndef TDB_JIT_INT_H
#define TDB_JIT_INT_H

#include "tdb_jit.h"
#include "../plsql/tdb_plsql_int.h"
#include "../common/tdb_buf.h"
#include "../common/tdb_mem.h"
#include "transactiondb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
** Emit a JIT-shaped IR module for `proc`. The generated module exports
**   define double @<fname>(ptr %argv)
** loading parameter i from argv[i] (instead of the per-parameter argument
** list used by the legacy tdb_plsql_emit_llvm). The signature is uniform
** across all five source languages so the JIT engine does not need to
** synthesize per-language trampolines.
**
** `errbuf` is set to a short description on failure. Returns TDB_OK on
** success, TDB_UNSUPPORTED for a construct outside the numeric subset.
*/
int tdb_jit_emit_ir_prog(const tdb_plsql_proc *proc, const char *fname,
                         tdb_buf *out, char *errbuf, int errlen);

/* ------------------------------------------------------------------ */
/* Per-language front ends                                            */
/* ------------------------------------------------------------------ */
/*
** Each front end parses a source string into a `tdb_plsql_proc` allocated
** in its own arena (which the caller owns and must release with
** tdb_plsql_free). The `params` array names argv positions exposed to the
** source as variables.
*/
int tdb_jit_sql_parse   (const char *src, const char *const *params, int nparams,
                         tdb_plsql_proc **out, char *errbuf, int errlen);
int tdb_jit_lua_parse   (const char *src, const char *const *params, int nparams,
                         tdb_plsql_proc **out, char *errbuf, int errlen);
int tdb_jit_xpath_parse (const char *src, const char *const *params, int nparams,
                         tdb_plsql_proc **out, char *errbuf, int errlen);
int tdb_jit_xquery_parse(const char *src, const char *const *params, int nparams,
                         tdb_plsql_proc **out, char *errbuf, int errlen);

#ifdef __cplusplus
}
#endif

#endif /* TDB_JIT_INT_H */
