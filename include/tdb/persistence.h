#ifndef TDB_PERSISTENCE_H
#define TDB_PERSISTENCE_H

#include "tdb/types.h"
#include "tdb/storage.h"
#include "tdb/buffer.h"
#include "tdb/wal.h"
#include "tdb/page.h"
#include "tdb/tuple.h"
#include "tdb/sql/executor.h"
#include "tdb/catalog.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace tdb::persistence {

class PersistenceEngine {
public:
    PersistenceEngine();
    ~PersistenceEngine();

    // Database lifecycle
    bool open(const std::string &path);
    void close();
    bool is_open() const;

    // Table storage -- maps table names to storage files
    bool create_table_storage(const std::string &table_name, const sql::Schema &schema);
    bool drop_table_storage(const std::string &table_name);

    // Row operations via pages
    bool insert_row(const std::string &table_name, const sql::Tuple &row, const sql::Schema &schema);
    bool delete_row(const std::string &table_name, size_t row_index);
    std::vector<sql::Tuple> scan_all_rows(const std::string &table_name, const sql::Schema &schema);

    // WAL operations
    void log_insert(const std::string &table_name, const sql::Tuple &row, const sql::Schema &schema, uint64_t txn_id);
    void log_delete(const std::string &table_name, size_t row_index, uint64_t txn_id);
    void log_commit(uint64_t txn_id);
    void log_abort(uint64_t txn_id);
    void flush_wal();

    // Checkpoint and recovery
    void checkpoint();
    void recover(); // ARIES: analysis -> redo -> undo

    // Catalog persistence
    void save_catalog(const catalog::Catalog &cat);
    void load_catalog(catalog::Catalog &cat);

private:
    std::string db_path_;
    bool is_open_ = false;
    tdb_wal_t wal_;
    // Per-table storage: table_name -> {storage, buffer_pool}
    struct TableStorage {
        tdb_storage_t storage;
        tdb_buffer_pool_t pool;
        size_t row_count = 0;
    };
    std::unordered_map<std::string, TableStorage> table_stores_;

    // Helpers
    std::string table_file_path(const std::string &table_name) const;
    TableStorage *get_or_create_storage(const std::string &table_name);
};

} // namespace tdb::persistence

#endif // TDB_PERSISTENCE_H
