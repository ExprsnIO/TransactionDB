/*
** tdb_jit.c — LLVM-ORC LLJIT engine + language dispatcher.
**
** When the build sets TDB_HAVE_LLVM, this file is the bridge between the
** textual IR produced by tdb_jit_emit_ir_prog (see tdb_jit_ir.c) and a
** native function pointer. We use the C API for ORC LLJIT v2 — it gives
** us lazy compilation, a per-instance ExecutionSession and a single
** dylib for added IR modules — and look up the requested entry point via
** the standard symbol mangler.
**
** When TDB_HAVE_LLVM is not defined the engine entry points return
** TDB_UNSUPPORTED so that callers can probe with tdb_jit_is_available()
** and degrade gracefully (e.g. fall back to tdb_plsql_exec).
*/
#include "tdb_jit.h"
#include "tdb_jit_int.h"
#include "tdb_plsql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TDB_HAVE_LLVM
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Target.h>
#include <llvm-c/Error.h>
#include <llvm-c/Orc.h>
#include <llvm-c/LLJIT.h>
#endif

/* ----------------------- IR emission dispatcher ----------------------- */

int tdb_jit_emit_ir(tdb_jit_lang lang, const char *src,
                    const char *const *params, int nparams,
                    tdb_buf *out, char *errbuf, int errlen) {
  if (!src || !out) return TDB_MISUSE;
  tdb_plsql_proc *proc = NULL;
  int rc;
  const char *fname = "jit_entry";

  switch (lang) {
    case TDB_JIT_PLSQL:
      rc = tdb_plsql_parse(src, params, nparams, &proc, errbuf, errlen);
      fname = "plsql_routine";
      break;
    case TDB_JIT_SQL:
      rc = tdb_jit_sql_parse(src, params, nparams, &proc, errbuf, errlen);
      break;
    case TDB_JIT_LUA:
      rc = tdb_jit_lua_parse(src, params, nparams, &proc, errbuf, errlen);
      break;
    case TDB_JIT_XPATH:
      rc = tdb_jit_xpath_parse(src, params, nparams, &proc, errbuf, errlen);
      break;
    case TDB_JIT_XQUERY:
      rc = tdb_jit_xquery_parse(src, params, nparams, &proc, errbuf, errlen);
      break;
    default:
      if (errbuf && errlen) snprintf(errbuf, (size_t)errlen, "unknown language %d", lang);
      return TDB_MISUSE;
  }
  if (rc != TDB_OK) return rc;

  rc = tdb_jit_emit_ir_prog(proc, fname, out, errbuf, errlen);
  tdb_plsql_free(proc);
  return rc;
}

/* ----------------------- LLJIT engine -------------------------------- */

#ifdef TDB_HAVE_LLVM

struct tdb_jit {
  LLVMOrcLLJITRef          lljit;
  LLVMOrcThreadSafeContextRef tsc;   /* one shared TSContext per JIT */
};

static int g_llvm_init = 0;

static void ensure_llvm_init(void) {
  if (g_llvm_init) return;
  /* Just the native target — we are doing in-process JIT only. */
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  g_llvm_init = 1;
}

static int err_to_status(LLVMErrorRef err, char *errbuf, int errlen,
                         const char *ctx) {
  if (!err) return TDB_OK;
  char *msg = LLVMGetErrorMessage(err);  /* consumes err */
  if (errbuf && errlen)
    snprintf(errbuf, (size_t)errlen, "%s: %s", ctx, msg ? msg : "?");
  LLVMDisposeErrorMessage(msg);
  return TDB_ERROR;
}

int tdb_jit_is_available(void) { return 1; }

const char *tdb_jit_llvm_version(void) {
  /* Encoded into the LLVM headers at build time. */
#ifdef LLVM_VERSION_STRING
  return LLVM_VERSION_STRING;
#else
  return "unknown";
#endif
}

int tdb_jit_open(tdb_jit **out) {
  if (!out) return TDB_MISUSE;
  ensure_llvm_init();
  tdb_jit *j = (tdb_jit *)calloc(1, sizeof(*j));
  if (!j) return TDB_NOMEM;
  LLVMErrorRef err = LLVMOrcCreateLLJIT(&j->lljit, NULL);
  if (err) {
    err_to_status(err, NULL, 0, "create");
    free(j);
    return TDB_ERROR;
  }
  /* One ThreadSafeContext shared by every module we add. Each module
  ** still owns its own LLVMContext lifetime once wrapped, so this is
  ** purely a convenience to avoid creating a context per compile. */
  j->tsc = LLVMOrcCreateNewThreadSafeContext();
  if (!j->tsc) { LLVMOrcDisposeLLJIT(j->lljit); free(j); return TDB_NOMEM; }
  *out = j;
  return TDB_OK;
}

void tdb_jit_close(tdb_jit *j) {
  if (!j) return;
  if (j->tsc)   LLVMOrcDisposeThreadSafeContext(j->tsc);
  if (j->lljit) LLVMOrcDisposeLLJIT(j->lljit);
  free(j);
}

int tdb_jit_compile_ir(tdb_jit *j, const char *ir, size_t irlen,
                       const char *fname, tdb_jit_fn *fn,
                       char *errbuf, int errlen) {
  if (!j || !ir || !fname || !fn) return TDB_MISUSE;

  /* Parse IR into a fresh per-compile context so module disposal is clean
  ** and so concurrent compiles do not contend. We wrap that context in a
  ** ThreadSafeContext so it can be moved into LLJIT. */
  LLVMOrcThreadSafeContextRef tsc = LLVMOrcCreateNewThreadSafeContext();
  if (!tsc) return TDB_NOMEM;
  LLVMContextRef ctx = LLVMOrcThreadSafeContextGetContext(tsc);

  LLVMMemoryBufferRef mb = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      ir, irlen, "tdb_jit_ir");
  LLVMModuleRef m = NULL;
  char *parse_err = NULL;
  if (LLVMParseIRInContext(ctx, mb, &m, &parse_err)) {
    if (errbuf && errlen)
      snprintf(errbuf, (size_t)errlen, "ir parse: %s",
               parse_err ? parse_err : "?");
    if (parse_err) LLVMDisposeMessage(parse_err);
    LLVMOrcDisposeThreadSafeContext(tsc);
    return TDB_ERROR;
  }
  /* On success LLVMParseIRInContext takes ownership of `mb`. */

  LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(m, tsc);
  /* tsm now owns m; we still have to dispose tsc handle (the ref-counted
  ** underlying context is retained by tsm). */
  LLVMOrcDisposeThreadSafeContext(tsc);

  LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(j->lljit);
  LLVMErrorRef add_err = LLVMOrcLLJITAddLLVMIRModule(j->lljit, jd, tsm);
  if (add_err)
    return err_to_status(add_err, errbuf, errlen, "jit add module");

  LLVMOrcJITTargetAddress addr = 0;
  LLVMErrorRef look_err = LLVMOrcLLJITLookup(j->lljit, &addr, fname);
  if (look_err)
    return err_to_status(look_err, errbuf, errlen, "jit lookup");
  if (!addr) {
    if (errbuf && errlen)
      snprintf(errbuf, (size_t)errlen, "symbol %s resolved to NULL", fname);
    return TDB_ERROR;
  }
  /* Round-trip via uintptr_t to silence -Wpedantic about object→fnptr. */
  uintptr_t a = (uintptr_t)addr;
  *fn = (tdb_jit_fn)a;
  return TDB_OK;
}

#else /* !TDB_HAVE_LLVM ---------- stub implementation ------------------- */

struct tdb_jit { int unused; };

int tdb_jit_is_available(void) { return 0; }
const char *tdb_jit_llvm_version(void) { return NULL; }

int tdb_jit_open(tdb_jit **out) {
  (void)out;
  return TDB_UNSUPPORTED;
}
void tdb_jit_close(tdb_jit *j) { (void)j; }

int tdb_jit_compile_ir(tdb_jit *j, const char *ir, size_t irlen,
                       const char *fname, tdb_jit_fn *fn,
                       char *errbuf, int errlen) {
  (void)j; (void)ir; (void)irlen; (void)fname; (void)fn;
  if (errbuf && errlen)
    snprintf(errbuf, (size_t)errlen,
             "JIT disabled: rebuild with -DTDB_BUILD_LLVM=ON");
  return TDB_UNSUPPORTED;
}

#endif /* TDB_HAVE_LLVM */

/* ----------------------- high-level compile -------------------------- */

int tdb_jit_compile(tdb_jit *j, tdb_jit_lang lang,
                    const char *src,
                    const char *const *params, int nparams,
                    tdb_jit_fn *fn,
                    char *errbuf, int errlen) {
  if (!src || !fn) return TDB_MISUSE;

  tdb_buf ir; tdb_buf_init(&ir);
  int rc = tdb_jit_emit_ir(lang, src, params, nparams, &ir, errbuf, errlen);
  if (rc != TDB_OK) { tdb_buf_free(&ir); return rc; }
  /* The emitter does not NUL-terminate, but LLVMParseIRInContext takes a
  ** length-delimited buffer so that is fine; we still ensure the buffer is
  ** non-empty. */
  if (ir.len == 0) { tdb_buf_free(&ir); return TDB_ERROR; }

  const char *fname = (lang == TDB_JIT_PLSQL) ? "plsql_routine" : "jit_entry";
  rc = tdb_jit_compile_ir(j, (const char *)ir.data, ir.len, fname, fn,
                          errbuf, errlen);
  tdb_buf_free(&ir);
  return rc;
}
