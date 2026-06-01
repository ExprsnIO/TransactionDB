// tdb_plugin_example.cpp — an example C++ extension (plugin) for TransactionDB.
//
// Built as a shared module and loaded at runtime via tdb_load_extension(). The
// entry point must have C linkage. It registers SQL scalar functions through
// the same public API a C extension would use.
#include "transactiondb.h"

#include <string>

namespace {

// plugin_addone(x) -> x + 1
void fn_addone(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  tdb_result_int64(ctx, tdb_value_int64(argv[0]) + 1);
}

// plugin_greet(name) -> "hello, <name>!"   (demonstrates C++ in a plugin)
void fn_greet(tdb_context *ctx, int argc, tdb_value **argv) {
  (void)argc;
  const char *n = tdb_value_text(argv[0]);
  std::string s = "hello, ";
  s += (n ? n : "world");
  s += "!";
  tdb_result_text(ctx, s.c_str(), static_cast<int>(s.size()));
}

} // namespace

extern "C" int tdb_extension_init(tdb_db *db, char **errmsg) {
  (void)errmsg;
  tdb_create_function(db, "plugin_addone", 1, fn_addone, nullptr);
  tdb_create_function(db, "plugin_greet", 1, fn_greet, nullptr);
  return TDB_OK;
}
