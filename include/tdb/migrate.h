#ifndef TDB_MIGRATE_H
#define TDB_MIGRATE_H

#include "tdb/catalog.h"
#include "tdb/sql/executor.h"
#include <cstdint>
#include <string>

namespace tdb {
class Database;
}

namespace tdb::migrate {

struct CsvOptions {
    char delimiter = ',';
    char quote = '"';
    char escape = '\\';
    bool header = true;
    std::string null_string = "NULL";
    std::string encoding = "UTF-8";
};

// Import CSV into a table. Creates table if it doesn't exist.
// Returns number of rows imported.
int64_t import_csv(catalog::Catalog &catalog, const std::string &table_name,
                    const std::string &file_path, const CsvOptions &options = {});

// Export a table to CSV file. Returns number of rows exported.
int64_t export_csv(const catalog::Catalog &catalog, const std::string &table_name,
                    const std::string &file_path, const CsvOptions &options = {});

// Import from SQL dump (series of SQL statements)
int64_t import_sql_dump(Database &db, const std::string &file_path);

// Export entire database as SQL dump
int64_t export_sql_dump(const catalog::Catalog &catalog, const std::string &file_path);

} // namespace tdb::migrate

#endif // TDB_MIGRATE_H
