#include "tdb/database.h"

/* C API */
extern "C" {
typedef struct tdb_db tdb_db;
tdb_db *tdb_open(const char *path) {
    auto *db = new tdb::Database();
    if (!db->open(path ? path : "")) { delete db; return nullptr; }
    return reinterpret_cast<tdb_db *>(db);
}
void tdb_close(tdb_db *db) {
    if (db) { auto *d = reinterpret_cast<tdb::Database *>(db); d->close(); delete d; }
}
int tdb_execute(tdb_db *db, const char *sql) {
    if (!db || !sql) return -1;
    auto *d = reinterpret_cast<tdb::Database *>(db);
    auto result = d->execute(sql);
    return result.success ? 0 : -1;
}
} // extern "C"
