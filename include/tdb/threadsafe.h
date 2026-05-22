#ifndef TDB_THREADSAFE_H
#define TDB_THREADSAFE_H

#include "tdb/database.h"
#include "tdb/sql/executor.h"
#include <shared_mutex>
#include <string>
#include <vector>

namespace tdb {

class ThreadSafeDatabase {
public:
    ThreadSafeDatabase();
    ~ThreadSafeDatabase();

    bool open(const std::string &path);
    void close();

    // Thread-safe execute (acquires write lock for DML/DDL, read lock for SELECT)
    sql::ResultSet execute(const std::string &sql);

    // Batch execute (acquires write lock for entire batch)
    std::vector<sql::ResultSet> execute_batch(const std::string &sql);

private:
    mutable std::shared_mutex mutex_;
    Database db_;

    bool is_read_only(const std::string &sql) const;
};

} // namespace tdb

#endif // TDB_THREADSAFE_H
