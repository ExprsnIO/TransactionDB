#include "tdb/threadsafe.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>

namespace tdb {

ThreadSafeDatabase::ThreadSafeDatabase() = default;
ThreadSafeDatabase::~ThreadSafeDatabase() = default;

bool ThreadSafeDatabase::open(const std::string &path) {
    std::unique_lock lock(mutex_);
    return db_.open(path);
}

void ThreadSafeDatabase::close() {
    std::unique_lock lock(mutex_);
    db_.close();
}

sql::ResultSet ThreadSafeDatabase::execute(const std::string &sql) {
    if (is_read_only(sql)) {
        std::shared_lock lock(mutex_);
        return db_.execute(sql);
    }
    std::unique_lock lock(mutex_);
    return db_.execute(sql);
}

std::vector<sql::ResultSet> ThreadSafeDatabase::execute_batch(const std::string &sql) {
    std::unique_lock lock(mutex_);
    return db_.execute_batch(sql);
}

bool ThreadSafeDatabase::is_read_only(const std::string &sql) const {
    // Skip leading whitespace
    size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
    }

    // Need at least a few characters to check the keyword
    if (i >= sql.size()) {
        return false;
    }

    // Extract the first word (up to 7 chars covers SELECT, EXPLAIN, SHOW)
    std::string first_word;
    first_word.reserve(8);
    for (size_t j = i; j < sql.size() && j - i < 8 && std::isalpha(static_cast<unsigned char>(sql[j])); ++j) {
        first_word += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[j])));
    }

    return first_word == "SELECT" || first_word == "EXPLAIN" || first_word == "SHOW";
}

} // namespace tdb
