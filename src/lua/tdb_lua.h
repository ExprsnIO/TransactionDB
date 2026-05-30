/*
** tdb_lua.h — embedded Lua 5.4 integration.
**
** Each database owns one lua_State. SQL routines created via
** CREATE FUNCTION/PROCEDURE ... LANGUAGE LUA AS $$ ... $$ are stored in the
** catalog as a Lua chunk that evaluates to a function (e.g.
** "function(x) return x*2 end") and registered here by name. The executor
** calls registered scalar functions when it evaluates a non-builtin function
** call, and CALL invokes a procedure. Scripts may issue nested SQL through the
** exposed `tdb` module (tdb.exec / tdb.query) against the owning connection.
*/
#ifndef TDB_LUA_H
#define TDB_LUA_H

#include "../common/tdb_internal.h"
#include "../value/tdb_value.h"

typedef struct tdb_lua tdb_lua;

/* `db` is the owning tdb_db, made available to the `tdb` Lua module. */
int  tdb_lua_open(void *db, tdb_lua **out);
void tdb_lua_close(tdb_lua *L);

/* Register a routine whose source is a function-valued chunk. */
int  tdb_lua_define(tdb_lua *L, const char *name, const char *func_src,
                    char **err);

/* Call a registered scalar function; result marshalled into *result. */
int  tdb_lua_call_scalar(tdb_lua *L, const char *name, tdb_value *argv,
                         int argc, tdb_value *result, char **err);

/* Call a registered procedure (result ignored). */
int  tdb_lua_call_proc(tdb_lua *L, const char *name, tdb_value *argv,
                       int argc, char **err);

#endif /* TDB_LUA_H */
