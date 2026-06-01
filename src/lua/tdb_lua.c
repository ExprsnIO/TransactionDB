/* tdb_lua.c — Lua state management, routine registration, and the `tdb` module. */
#include "tdb_lua.h"
#include "../common/tdb_mem.h"
#include "../common/tdb_mutex.h"
#include "transactiondb.h"   /* public API, for the tdb.* module */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>

struct tdb_lua {
  lua_State *L;
  tdb_mutex *mu;
  void      *db;
};

/* registry keys */
static const char *K_ROUTINES = "tdb.routines";
static const char *K_DB = "tdb.db";

/* ----------------------------- marshalling ---------------------------- */

static void push_value(lua_State *L, const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_NULL: lua_pushnil(L); break;
    case TDB_VAL_INT:  lua_pushinteger(L, (lua_Integer)v->u.i); break;
    case TDB_VAL_REAL: lua_pushnumber(L, v->u.r); break;
    case TDB_VAL_TEXT:
    case TDB_VAL_BLOB:
    case TDB_VAL_COMPOSITE: lua_pushlstring(L, v->u.s.p ? v->u.s.p : "", (size_t)v->u.s.n); break;
  }
}

static void to_value(lua_State *L, int idx, tdb_value *out) {
  tdb_value_init(out);
  switch (lua_type(L, idx)) {
    case LUA_TNIL: tdb_value_set_null(out); break;
    case LUA_TBOOLEAN: tdb_value_set_int(out, lua_toboolean(L, idx) ? 1 : 0); break;
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) tdb_value_set_int(out, (int64_t)lua_tointeger(L, idx));
      else tdb_value_set_real(out, (double)lua_tonumber(L, idx));
      break;
    case LUA_TSTRING: {
      size_t n; const char *s = lua_tolstring(L, idx, &n);
      tdb_value_set_text(out, s, (int)n, 1);
      break;
    }
    default: tdb_value_set_null(out); break;
  }
}

/* ------------------------------ tdb module ---------------------------- */

static tdb_db *module_db(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, K_DB);
  tdb_db *db = (tdb_db *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return db;
}

/* tdb.exec(sql) -> nil ; raises on error */
static int l_tdb_exec(lua_State *L) {
  tdb_db *db = module_db(L);
  const char *sql = luaL_checkstring(L, 1);
  char *err = NULL;
  int rc = tdb_exec(db, sql, NULL, NULL, &err);
  if (rc != TDB_OK) {
    lua_pushstring(L, err ? err : "exec failed");
    tdb_free(err);
    return lua_error(L);
  }
  return 0;
}

/* tdb.query(sql) -> array of row tables keyed by column name */
static int l_tdb_query(lua_State *L) {
  tdb_db *db = module_db(L);
  const char *sql = luaL_checkstring(L, 1);
  tdb_stmt *st = NULL;
  if (tdb_prepare_v2(db, sql, -1, &st, NULL) != TDB_OK || !st) {
    lua_pushstring(L, tdb_errmsg(db));
    return lua_error(L);
  }
  lua_newtable(L);
  int row = 0;
  while (tdb_step(st) == TDB_ROW) {
    lua_newtable(L);
    int nc = tdb_column_count(st);
    for (int i = 0; i < nc; i++) {
      switch (tdb_column_type(st, i)) {
        case TDB_INTEGER: lua_pushinteger(L, (lua_Integer)tdb_column_int64(st, i)); break;
        case TDB_FLOAT:   lua_pushnumber(L, tdb_column_double(st, i)); break;
        case TDB_NULL:    lua_pushnil(L); break;
        default: { const char *t = tdb_column_text(st, i); lua_pushstring(L, t ? t : ""); break; }
      }
      lua_setfield(L, -2, tdb_column_name(st, i));
    }
    lua_rawseti(L, -2, ++row);
  }
  tdb_finalize(st);
  return 1;
}

static void register_module(lua_State *L, void *db) {
  lua_pushlightuserdata(L, db);
  lua_setfield(L, LUA_REGISTRYINDEX, K_DB);

  lua_newtable(L);                 /* routines registry table */
  lua_setfield(L, LUA_REGISTRYINDEX, K_ROUTINES);

  lua_newtable(L);                 /* the `tdb` module */
  lua_pushcfunction(L, l_tdb_exec);  lua_setfield(L, -2, "exec");
  lua_pushcfunction(L, l_tdb_query); lua_setfield(L, -2, "query");
  lua_setglobal(L, "tdb");
}

/* ------------------------------ lifecycle ----------------------------- */

int tdb_lua_open(void *db, tdb_lua **out) {
  tdb_lua *l = (tdb_lua *)tdb_calloc(sizeof(*l));
  if (!l) return TDB_NOMEM;
  l->L = luaL_newstate();
  if (!l->L) { tdb_mfree(l); return TDB_NOMEM; }
  luaL_openlibs(l->L);
  l->mu = tdb_mutex_new();
  l->db = db;
  register_module(l->L, db);
  *out = l;
  return TDB_OK;
}

void tdb_lua_close(tdb_lua *l) {
  if (!l) return;
  if (l->L) lua_close(l->L);
  tdb_mutex_free(l->mu);
  tdb_mfree(l);
}

int tdb_lua_define(tdb_lua *l, const char *name, const char *func_src,
                   char **err) {
  tdb_mutex_lock(l->mu);
  lua_State *L = l->L;
  /* compile "return <func_src>" -> a function value */
  size_t n = strlen(func_src);
  char *chunk = (char *)tdb_malloc(n + 8);
  memcpy(chunk, "return ", 7);
  memcpy(chunk + 7, func_src, n + 1);
  int rc = luaL_loadstring(L, chunk);
  tdb_mfree(chunk);
  if (rc != LUA_OK) {
    if (err) *err = tdb_strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    tdb_mutex_unlock(l->mu);
    return TDB_ERROR;
  }
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    if (err) *err = tdb_strdup(lua_tostring(L, -1));
    lua_pop(L, 1);
    tdb_mutex_unlock(l->mu);
    return TDB_ERROR;
  }
  /* routines[name] = <function> */
  lua_getfield(L, LUA_REGISTRYINDEX, K_ROUTINES);
  lua_pushvalue(L, -2);            /* the function */
  lua_setfield(L, -2, name);
  lua_pop(L, 2);                   /* routines table + function */
  tdb_mutex_unlock(l->mu);
  return TDB_OK;
}

static int call_routine(tdb_lua *l, const char *name, tdb_value *argv, int argc,
                        tdb_value *result, int want_result, char **err) {
  tdb_mutex_lock(l->mu);
  lua_State *L = l->L;
  lua_getfield(L, LUA_REGISTRYINDEX, K_ROUTINES);
  lua_getfield(L, -1, name);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    if (err) *err = tdb_strdup("no such routine");
    tdb_mutex_unlock(l->mu);
    return TDB_NOTFOUND;
  }
  for (int i = 0; i < argc; i++) push_value(L, &argv[i]);
  int nres = want_result ? 1 : 0;
  if (lua_pcall(L, argc, nres, 0) != LUA_OK) {
    if (err) *err = tdb_strdup(lua_tostring(L, -1));
    lua_pop(L, 2);
    tdb_mutex_unlock(l->mu);
    return TDB_ERROR;
  }
  if (want_result && result) to_value(L, -1, result);
  lua_pop(L, want_result ? 2 : 1); /* result (if any) + routines table */
  tdb_mutex_unlock(l->mu);
  return TDB_OK;
}

int tdb_lua_call_scalar(tdb_lua *l, const char *name, tdb_value *argv, int argc,
                        tdb_value *result, char **err) {
  return call_routine(l, name, argv, argc, result, 1, err);
}

int tdb_lua_call_proc(tdb_lua *l, const char *name, tdb_value *argv, int argc,
                      char **err) {
  return call_routine(l, name, argv, argc, NULL, 0, err);
}
