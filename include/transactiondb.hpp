/*
** transactiondb.hpp — header-only C++ RAII wrapper over the C API.
**
** Usage:
**   tdb::Database db(":memory:");
**   db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)");
**   auto st = db.prepare("INSERT INTO t VALUES (?, ?)");
**   st.bind(1, 1); st.bind(2, "alice"); st.step();
**   auto q = db.prepare("SELECT name FROM t WHERE id = ?");
**   q.bind(1, 1);
**   while (q.step()) std::cout << q.getText(0) << "\n";
**
** Errors are reported by throwing tdb::Error. Database and Statement are
** move-only and release their handles in their destructors.
*/
#ifndef TRANSACTIONDB_HPP
#define TRANSACTIONDB_HPP

#include "transactiondb.h"

#include <string>
#include <string_view>
#include <stdexcept>
#include <cstdint>

namespace tdb {

class Error : public std::runtime_error {
public:
  int code;
  Error(int c, const std::string &msg) : std::runtime_error(msg), code(c) {}
};

class Statement {
public:
  explicit Statement(tdb_stmt *s) : s_(s) {}
  Statement(Statement &&o) noexcept : s_(o.s_) { o.s_ = nullptr; }
  Statement &operator=(Statement &&o) noexcept {
    if (this != &o) { if (s_) tdb_finalize(s_); s_ = o.s_; o.s_ = nullptr; }
    return *this;
  }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  ~Statement() { if (s_) tdb_finalize(s_); }

  /* Advance: returns true while a row is available, false when finished. */
  bool step() {
    int rc = tdb_step(s_);
    if (rc == TDB_ROW) return true;
    if (rc == TDB_DONE) return false;
    throw Error(rc, "step failed");
  }
  void reset() { tdb_reset(s_); }

  Statement &bind(int i, std::int64_t v) { tdb_bind_int64(s_, i, v); return *this; }
  Statement &bind(int i, int v) { tdb_bind_int(s_, i, v); return *this; }
  Statement &bind(int i, double v) { tdb_bind_double(s_, i, v); return *this; }
  Statement &bind(int i, std::string_view v) {
    tdb_bind_text(s_, i, v.data(), static_cast<int>(v.size())); return *this;
  }
  Statement &bind(int i, const char *v) { tdb_bind_text(s_, i, v, -1); return *this; }
  Statement &bindNull(int i) { tdb_bind_null(s_, i); return *this; }

  int          columnCount() const { return tdb_column_count(s_); }
  std::int64_t getInt(int i) const { return tdb_column_int64(s_, i); }
  double       getDouble(int i) const { return tdb_column_double(s_, i); }
  std::string  getText(int i) const {
    const char *t = tdb_column_text(s_, i);
    return t ? std::string(t) : std::string();
  }
  bool         isNull(int i) const { return tdb_column_type(s_, i) == TDB_NULL; }
  std::string  columnName(int i) const {
    const char *n = tdb_column_name(s_, i);
    return n ? std::string(n) : std::string();
  }
  tdb_stmt *handle() const { return s_; }

private:
  tdb_stmt *s_ = nullptr;
};

class Database {
public:
  explicit Database(const std::string &path,
                    int flags = TDB_OPEN_READWRITE | TDB_OPEN_CREATE) {
    int rc = tdb_open_v2(path.c_str(), &db_, flags);
    if (rc != TDB_OK) {
      std::string m = db_ ? tdb_errmsg(db_) : "open failed";
      if (db_) tdb_close(db_);
      db_ = nullptr;
      throw Error(rc, m);
    }
  }
  Database(Database &&o) noexcept : db_(o.db_) { o.db_ = nullptr; }
  Database &operator=(Database &&o) noexcept {
    if (this != &o) { if (db_) tdb_close(db_); db_ = o.db_; o.db_ = nullptr; }
    return *this;
  }
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  ~Database() { if (db_) tdb_close(db_); }

  void exec(const std::string &sql) {
    char *err = nullptr;
    int rc = tdb_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != TDB_OK) {
      std::string m = err ? err : tdb_errmsg(db_);
      tdb_free(err);
      throw Error(rc, m);
    }
  }
  Statement prepare(const std::string &sql) {
    tdb_stmt *s = nullptr;
    int rc = tdb_prepare_v2(db_, sql.c_str(), -1, &s, nullptr);
    if (rc != TDB_OK) throw Error(rc, tdb_errmsg(db_));
    return Statement(s);
  }
  const char *errmsg() const { return tdb_errmsg(db_); }
  tdb_db *handle() const { return db_; }

private:
  tdb_db *db_ = nullptr;
};

} // namespace tdb

#endif /* TRANSACTIONDB_HPP */
