#include "tdb/persistence.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// Page size used for all table storage files.
static constexpr size_t kPageSize = 8192;
// Number of buffer-pool frames per table.
static constexpr size_t kPoolCapacity = 64;
// Maximum serialised tuple buffer (must fit in one page).
static constexpr size_t kMaxTupleBuf = kPageSize;

// ─── Catalog-file magic / version ──────────────────────────────────────
static constexpr uint32_t kCatalogMagic   = 0x54444243; // "TDBC"
static constexpr uint32_t kCatalogVersion = 1;

// ─── WAL "table_name" encoding helpers ─────────────────────────────────
// WAL records carry a fixed-size page_id but we also need to know which
// table the record belongs to.  We encode the table name as a
// length-prefixed string at the start of the before-image for UPDATE
// records, and as the sole content for COMMIT/ABORT/BEGIN records.
//
// For INSERT WAL records:
//   before_image = [uint32 name_len][name bytes]
//   after_image  = serialised tuple bytes
//
// For DELETE WAL records:
//   before_image = [uint32 name_len][name bytes][uint64 row_index]
//   after_image  = (empty)

namespace tdb::persistence {

// ────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ────────────────────────────────────────────────────────────────────────

PersistenceEngine::PersistenceEngine() {
    std::memset(&wal_, 0, sizeof(wal_));
}

PersistenceEngine::~PersistenceEngine() {
    if (is_open_) {
        close();
    }
}

// ────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────

bool PersistenceEngine::open(const std::string &path) {
    if (is_open_) {
        close();
    }
    db_path_ = path;

    // Create the database directory tree.
    std::error_code ec;
    fs::create_directories(db_path_, ec);
    if (ec) {
        return false;
    }

    // Open (or create) the WAL file.
    std::string wal_path = db_path_ + "/tdb_wal.log";
    tdb_status_t st = tdb_wal_init(&wal_, wal_path.c_str());
    if (st != TDB_OK) {
        return false;
    }

    is_open_ = true;

    // Run recovery (redo-only) on any prior WAL contents.
    recover();

    return true;
}

void PersistenceEngine::close() {
    if (!is_open_) return;

    // Flush every buffer pool to disk.
    for (auto &[name, ts] : table_stores_) {
        tdb_buffer_pool_flush_all(&ts.pool);
    }

    // Checkpoint the WAL so the next open starts clean.
    checkpoint();

    // Tear down per-table resources.
    for (auto &[name, ts] : table_stores_) {
        tdb_buffer_pool_destroy(&ts.pool);
        tdb_storage_close(&ts.storage);
    }
    table_stores_.clear();

    tdb_wal_destroy(&wal_);
    std::memset(&wal_, 0, sizeof(wal_));

    is_open_ = false;
}

bool PersistenceEngine::is_open() const {
    return is_open_;
}

// ────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────

std::string PersistenceEngine::table_file_path(const std::string &table_name) const {
    return db_path_ + "/" + table_name + ".tdb";
}

PersistenceEngine::TableStorage *
PersistenceEngine::get_or_create_storage(const std::string &table_name) {
    auto it = table_stores_.find(table_name);
    if (it != table_stores_.end()) {
        return &it->second;
    }

    // Open (or create) the backing file and initialise a buffer pool.
    std::string fpath = table_file_path(table_name);

    TableStorage ts{};
    std::memset(&ts.storage, 0, sizeof(ts.storage));
    std::memset(&ts.pool, 0, sizeof(ts.pool));

    tdb_status_t st = tdb_storage_open(&ts.storage, fpath.c_str(), kPageSize);
    if (st != TDB_OK) {
        return nullptr;
    }

    st = tdb_buffer_pool_init(&ts.pool, kPoolCapacity, kPageSize, &ts.storage);
    if (st != TDB_OK) {
        tdb_storage_close(&ts.storage);
        return nullptr;
    }

    // Insert into map.  We need the address to remain stable, so fetch
    // a reference after insertion.
    auto [ins_it, ok] = table_stores_.emplace(table_name, std::move(ts));
    if (!ok) {
        tdb_buffer_pool_destroy(&ts.pool);
        tdb_storage_close(&ts.storage);
        return nullptr;
    }

    // Re-point the buffer pool's storage pointer at the map-owned copy
    // (the move left the pointer dangling).
    ins_it->second.pool.storage = &ins_it->second.storage;

    return &ins_it->second;
}

// ────────────────────────────────────────────────────────────────────────
// Table storage management
// ────────────────────────────────────────────────────────────────────────

bool PersistenceEngine::create_table_storage(const std::string &table_name,
                                              const sql::Schema & /*schema*/) {
    if (!is_open_) return false;
    return get_or_create_storage(table_name) != nullptr;
}

bool PersistenceEngine::drop_table_storage(const std::string &table_name) {
    if (!is_open_) return false;

    auto it = table_stores_.find(table_name);
    if (it != table_stores_.end()) {
        tdb_buffer_pool_flush_all(&it->second.pool);
        tdb_buffer_pool_destroy(&it->second.pool);
        tdb_storage_close(&it->second.storage);
        table_stores_.erase(it);
    }

    // Remove the file on disk.
    std::string fpath = table_file_path(table_name);
    std::error_code ec;
    fs::remove(fpath, ec);
    return true;
}

// ────────────────────────────────────────────────────────────────────────
// Row operations
// ────────────────────────────────────────────────────────────────────────

bool PersistenceEngine::insert_row(const std::string &table_name,
                                    const sql::Tuple &row,
                                    const sql::Schema &schema) {
    if (!is_open_) return false;

    TableStorage *ts = get_or_create_storage(table_name);
    if (!ts) return false;

    // Serialise the tuple.
    uint8_t tuple_buf[kMaxTupleBuf];
    size_t tuple_len = storage::serialize_tuple(row, schema, tuple_buf, sizeof(tuple_buf));
    if (tuple_len == 0) {
        return false; // tuple too large for a single page
    }

    // Try to insert into the last existing page (if any).
    uint64_t page_count = ts->storage.page_count;
    bool inserted = false;

    if (page_count > 0) {
        tdb_page_id_t last_pid = page_count - 1;
        void *page_ptr = nullptr;
        tdb_status_t st = tdb_buffer_pool_fetch(&ts->pool, last_pid, &page_ptr);
        if (st == TDB_OK) {
            size_t free_space = tdb_page_free_space(page_ptr);
            // Need space for the tuple data + a slot entry (4 bytes).
            if (free_space >= tuple_len + sizeof(tdb_slot_entry_t)) {
                tdb_slot_t slot = 0;
                st = tdb_page_insert_tuple(page_ptr, tuple_buf,
                                           static_cast<uint16_t>(tuple_len), &slot);
                if (st == TDB_OK) {
                    tdb_buffer_pool_mark_dirty(&ts->pool, last_pid);
                    inserted = true;
                }
            }
            tdb_buffer_pool_unpin(&ts->pool, last_pid);
        }
    }

    // If the last page was full (or there are no pages), allocate a new one.
    if (!inserted) {
        tdb_page_id_t new_pid = 0;
        void *page_ptr = nullptr;
        tdb_status_t st = tdb_buffer_pool_new_page(&ts->pool, &new_pid, &page_ptr);
        if (st != TDB_OK) return false;

        st = tdb_page_init(page_ptr, kPageSize, new_pid);
        if (st != TDB_OK) {
            tdb_buffer_pool_unpin(&ts->pool, new_pid);
            return false;
        }

        tdb_slot_t slot = 0;
        st = tdb_page_insert_tuple(page_ptr, tuple_buf,
                                   static_cast<uint16_t>(tuple_len), &slot);
        if (st != TDB_OK) {
            tdb_buffer_pool_unpin(&ts->pool, new_pid);
            return false;
        }

        tdb_buffer_pool_mark_dirty(&ts->pool, new_pid);
        tdb_buffer_pool_unpin(&ts->pool, new_pid);
    }

    ts->row_count++;
    return true;
}

bool PersistenceEngine::delete_row(const std::string &table_name, size_t row_index) {
    if (!is_open_) return false;

    TableStorage *ts = get_or_create_storage(table_name);
    if (!ts) return false;

    // Walk pages sequentially, counting live slots, until we reach
    // the target row_index.
    uint64_t page_count = ts->storage.page_count;
    size_t running = 0;

    for (uint64_t pid = 0; pid < page_count; ++pid) {
        void *page_ptr = nullptr;
        tdb_status_t st = tdb_buffer_pool_fetch(&ts->pool, pid, &page_ptr);
        if (st != TDB_OK) continue;

        const tdb_page_header_t *hdr = tdb_page_get_header(page_ptr);
        uint16_t slot_count = hdr->slot_count;

        for (uint16_t s = 0; s < slot_count; ++s) {
            const void *data = nullptr;
            uint16_t len = 0;
            st = tdb_page_get_tuple(page_ptr, s, &data, &len);
            if (st != TDB_OK || len == 0) {
                // Deleted or invalid slot -- skip.
                continue;
            }
            if (running == row_index) {
                tdb_page_delete_tuple(page_ptr, s);
                tdb_buffer_pool_mark_dirty(&ts->pool, pid);
                tdb_buffer_pool_unpin(&ts->pool, pid);
                if (ts->row_count > 0) ts->row_count--;
                return true;
            }
            running++;
        }
        tdb_buffer_pool_unpin(&ts->pool, pid);
    }

    return false; // row_index out of range
}

std::vector<sql::Tuple>
PersistenceEngine::scan_all_rows(const std::string &table_name,
                                  const sql::Schema &schema) {
    std::vector<sql::Tuple> result;
    if (!is_open_) return result;

    TableStorage *ts = get_or_create_storage(table_name);
    if (!ts) return result;

    uint64_t page_count = ts->storage.page_count;

    for (uint64_t pid = 0; pid < page_count; ++pid) {
        void *page_ptr = nullptr;
        tdb_status_t st = tdb_buffer_pool_fetch(&ts->pool, pid, &page_ptr);
        if (st != TDB_OK) continue;

        const tdb_page_header_t *hdr = tdb_page_get_header(page_ptr);
        uint16_t slot_count = hdr->slot_count;

        for (uint16_t s = 0; s < slot_count; ++s) {
            const void *data = nullptr;
            uint16_t len = 0;
            st = tdb_page_get_tuple(page_ptr, s, &data, &len);
            if (st != TDB_OK || len == 0) {
                continue; // deleted slot
            }
            try {
                sql::Tuple t = storage::deserialize_tuple(
                    static_cast<const uint8_t *>(data), len, schema);
                result.push_back(std::move(t));
            } catch (...) {
                // Skip corrupt tuples during scan.
            }
        }
        tdb_buffer_pool_unpin(&ts->pool, pid);
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────
// WAL operations
// ────────────────────────────────────────────────────────────────────────

// Helper: build a before-image that carries the table name.
static std::vector<uint8_t> encode_table_name(const std::string &name) {
    std::vector<uint8_t> buf(sizeof(uint32_t) + name.size());
    uint32_t len = static_cast<uint32_t>(name.size());
    std::memcpy(buf.data(), &len, sizeof(uint32_t));
    std::memcpy(buf.data() + sizeof(uint32_t), name.data(), name.size());
    return buf;
}

// Helper: decode a table name from a before-image.
static std::string decode_table_name(const void *data, uint16_t data_len) {
    if (!data || data_len < sizeof(uint32_t)) return {};
    const auto *p = static_cast<const uint8_t *>(data);
    uint32_t len = 0;
    std::memcpy(&len, p, sizeof(uint32_t));
    if (sizeof(uint32_t) + len > data_len) return {};
    return std::string(reinterpret_cast<const char *>(p + sizeof(uint32_t)), len);
}

void PersistenceEngine::log_insert(const std::string &table_name,
                                    const sql::Tuple &row,
                                    const sql::Schema &schema,
                                    uint64_t txn_id) {
    if (!is_open_) return;

    // Before-image: table name.
    std::vector<uint8_t> before = encode_table_name(table_name);

    // After-image: serialised tuple.
    uint8_t tuple_buf[kMaxTupleBuf];
    size_t tuple_len = storage::serialize_tuple(row, schema, tuple_buf, sizeof(tuple_buf));
    if (tuple_len == 0) return;

    tdb_wal_record_t rec{};
    rec.txn_id   = txn_id;
    rec.type     = TDB_WAL_UPDATE;
    rec.page_id  = 0; // not page-specific
    rec.offset   = 0;
    rec.before_len = static_cast<uint16_t>(before.size());
    rec.after_len  = static_cast<uint16_t>(tuple_len);
    rec.total_len  = static_cast<uint32_t>(sizeof(tdb_wal_record_t) + before.size() + tuple_len);

    tdb_wal_append(&wal_, &rec, before.data(), tuple_buf);
}

void PersistenceEngine::log_delete(const std::string &table_name,
                                    size_t row_index,
                                    uint64_t txn_id) {
    if (!is_open_) return;

    // Before-image: table name + row index.
    std::vector<uint8_t> before(sizeof(uint32_t) + table_name.size() + sizeof(uint64_t));
    uint32_t name_len = static_cast<uint32_t>(table_name.size());
    uint8_t *p = before.data();
    std::memcpy(p, &name_len, sizeof(uint32_t));
    p += sizeof(uint32_t);
    std::memcpy(p, table_name.data(), table_name.size());
    p += table_name.size();
    uint64_t idx = static_cast<uint64_t>(row_index);
    std::memcpy(p, &idx, sizeof(uint64_t));

    tdb_wal_record_t rec{};
    rec.txn_id   = txn_id;
    rec.type     = TDB_WAL_UPDATE;
    rec.page_id  = 0;
    rec.offset   = 1; // sentinel: 1 = delete, 0 = insert
    rec.before_len = static_cast<uint16_t>(before.size());
    rec.after_len  = 0;
    rec.total_len  = static_cast<uint32_t>(sizeof(tdb_wal_record_t) + before.size());

    tdb_wal_append(&wal_, &rec, before.data(), nullptr);
}

void PersistenceEngine::log_commit(uint64_t txn_id) {
    if (!is_open_) return;

    tdb_wal_record_t rec{};
    rec.txn_id    = txn_id;
    rec.type      = TDB_WAL_COMMIT;
    rec.before_len = 0;
    rec.after_len  = 0;
    rec.total_len  = static_cast<uint32_t>(sizeof(tdb_wal_record_t));

    tdb_wal_append(&wal_, &rec, nullptr, nullptr);
}

void PersistenceEngine::log_abort(uint64_t txn_id) {
    if (!is_open_) return;

    tdb_wal_record_t rec{};
    rec.txn_id    = txn_id;
    rec.type      = TDB_WAL_ABORT;
    rec.before_len = 0;
    rec.after_len  = 0;
    rec.total_len  = static_cast<uint32_t>(sizeof(tdb_wal_record_t));

    tdb_wal_append(&wal_, &rec, nullptr, nullptr);
}

void PersistenceEngine::flush_wal() {
    if (!is_open_) return;
    tdb_wal_flush(&wal_, wal_.next_lsn);
}

// ────────────────────────────────────────────────────────────────────────
// Checkpoint
// ────────────────────────────────────────────────────────────────────────

void PersistenceEngine::checkpoint() {
    if (!is_open_) return;

    // Flush all dirty pages for every table.
    for (auto &[name, ts] : table_stores_) {
        tdb_buffer_pool_flush_all(&ts.pool);
        tdb_storage_sync(&ts.storage);
    }

    // Write a checkpoint record with empty ATT and DPT (all pages clean).
    tdb_wal_checkpoint(&wal_, nullptr, 0, nullptr, 0);

    // Flush the WAL itself so the checkpoint record is durable.
    tdb_wal_flush(&wal_, wal_.next_lsn);
}

// ────────────────────────────────────────────────────────────────────────
// Recovery (simplified redo-only ARIES)
// ────────────────────────────────────────────────────────────────────────

// Context structure passed to the WAL scan callback.
struct RecoveryCtx {
    PersistenceEngine *engine;
    // Track committed transactions so we only redo their work.
    std::unordered_map<uint64_t, bool> committed;
    // Collected UPDATE records to replay after analysis.
    struct PendingRedo {
        tdb_wal_record_t record;
        std::vector<uint8_t> before;
        std::vector<uint8_t> after;
    };
    std::vector<PendingRedo> pending;
};

// Phase-1 callback: analysis -- collect all records.
static void recovery_scan_cb(const tdb_wal_record_t *record,
                              const void *before, const void *after,
                              void *ctx) {
    auto *rctx = static_cast<RecoveryCtx *>(ctx);

    if (record->type == TDB_WAL_COMMIT) {
        rctx->committed[record->txn_id] = true;
        return;
    }
    if (record->type == TDB_WAL_ABORT) {
        rctx->committed[record->txn_id] = false;
        return;
    }
    if (record->type == TDB_WAL_UPDATE) {
        RecoveryCtx::PendingRedo pr;
        pr.record = *record;
        if (before && record->before_len > 0) {
            const auto *b = static_cast<const uint8_t *>(before);
            pr.before.assign(b, b + record->before_len);
        }
        if (after && record->after_len > 0) {
            const auto *a = static_cast<const uint8_t *>(after);
            pr.after.assign(a, a + record->after_len);
        }
        rctx->pending.push_back(std::move(pr));
    }
    // CHECKPOINT / CLR / BEGIN records are informational; skip.
}

void PersistenceEngine::recover() {
    if (!is_open_) return;

    RecoveryCtx rctx;
    rctx.engine = this;

    // Scan the entire WAL from LSN 0 (analysis phase).
    tdb_status_t st = tdb_wal_scan(&wal_, 0, recovery_scan_cb, &rctx);
    if (st != TDB_OK) {
        // No WAL data or error -- nothing to recover.
        return;
    }

    // Phase-2: redo committed INSERT / DELETE operations.
    for (const auto &pr : rctx.pending) {
        // Only redo records belonging to committed transactions.
        auto cit = rctx.committed.find(pr.record.txn_id);
        if (cit == rctx.committed.end() || !cit->second) {
            continue; // uncommitted or aborted -- skip
        }

        // Decode table name from before-image.
        std::string tbl = decode_table_name(
            pr.before.data(), static_cast<uint16_t>(pr.before.size()));
        if (tbl.empty()) continue;

        if (pr.record.offset == 0 && !pr.after.empty()) {
            // INSERT redo: deserialise the after-image tuple and re-insert
            // into the page file.  We use an empty schema -- deserialize_tuple
            // infers types from the wire format.
            sql::Schema dummy_schema;
            try {
                sql::Tuple row = storage::deserialize_tuple(
                    pr.after.data(), pr.after.size(), dummy_schema);

                // Re-insert through the page layer.
                TableStorage *ts = get_or_create_storage(tbl);
                if (!ts) continue;

                // Re-serialise (after-image is already the serialised form).
                const uint8_t *tuple_data = pr.after.data();
                size_t tuple_len = pr.after.size();

                // Try last page, else allocate.
                bool inserted = false;
                uint64_t pc = ts->storage.page_count;
                if (pc > 0) {
                    tdb_page_id_t last_pid = pc - 1;
                    void *page_ptr = nullptr;
                    tdb_status_t pst = tdb_buffer_pool_fetch(&ts->pool, last_pid, &page_ptr);
                    if (pst == TDB_OK) {
                        size_t free = tdb_page_free_space(page_ptr);
                        if (free >= tuple_len + sizeof(tdb_slot_entry_t)) {
                            tdb_slot_t slot = 0;
                            pst = tdb_page_insert_tuple(page_ptr, tuple_data,
                                                        static_cast<uint16_t>(tuple_len), &slot);
                            if (pst == TDB_OK) {
                                tdb_buffer_pool_mark_dirty(&ts->pool, last_pid);
                                inserted = true;
                            }
                        }
                        tdb_buffer_pool_unpin(&ts->pool, last_pid);
                    }
                }
                if (!inserted) {
                    tdb_page_id_t new_pid = 0;
                    void *page_ptr = nullptr;
                    tdb_status_t pst = tdb_buffer_pool_new_page(&ts->pool, &new_pid, &page_ptr);
                    if (pst == TDB_OK) {
                        tdb_page_init(page_ptr, kPageSize, new_pid);
                        tdb_slot_t slot = 0;
                        tdb_page_insert_tuple(page_ptr, tuple_data,
                                              static_cast<uint16_t>(tuple_len), &slot);
                        tdb_buffer_pool_mark_dirty(&ts->pool, new_pid);
                        tdb_buffer_pool_unpin(&ts->pool, new_pid);
                    }
                }
                ts->row_count++;
            } catch (...) {
                // Skip corrupt WAL entries.
            }
        } else if (pr.record.offset == 1) {
            // DELETE redo: extract row_index from before-image.
            uint32_t name_len = 0;
            if (pr.before.size() < sizeof(uint32_t)) continue;
            std::memcpy(&name_len, pr.before.data(), sizeof(uint32_t));
            size_t idx_off = sizeof(uint32_t) + name_len;
            if (pr.before.size() < idx_off + sizeof(uint64_t)) continue;
            uint64_t row_idx = 0;
            std::memcpy(&row_idx, pr.before.data() + idx_off, sizeof(uint64_t));

            // Walk pages and delete the Nth live tuple.
            TableStorage *ts = get_or_create_storage(tbl);
            if (!ts) continue;

            uint64_t pc = ts->storage.page_count;
            size_t running = 0;
            bool done = false;
            for (uint64_t pid = 0; pid < pc && !done; ++pid) {
                void *page_ptr = nullptr;
                tdb_status_t pst = tdb_buffer_pool_fetch(&ts->pool, pid, &page_ptr);
                if (pst != TDB_OK) continue;

                const tdb_page_header_t *hdr = tdb_page_get_header(page_ptr);
                for (uint16_t s = 0; s < hdr->slot_count; ++s) {
                    const void *d = nullptr;
                    uint16_t l = 0;
                    pst = tdb_page_get_tuple(page_ptr, s, &d, &l);
                    if (pst != TDB_OK || l == 0) continue;
                    if (running == static_cast<size_t>(row_idx)) {
                        tdb_page_delete_tuple(page_ptr, s);
                        tdb_buffer_pool_mark_dirty(&ts->pool, pid);
                        if (ts->row_count > 0) ts->row_count--;
                        done = true;
                        break;
                    }
                    running++;
                }
                tdb_buffer_pool_unpin(&ts->pool, pid);
            }
        }
    }

    // Flush recovered state to disk.
    for (auto &[name, ts] : table_stores_) {
        tdb_buffer_pool_flush_all(&ts.pool);
    }
}

// ────────────────────────────────────────────────────────────────────────
// Catalog persistence
// ────────────────────────────────────────────────────────────────────────
//
// Format of tdb_catalog.dat:
//   [uint32 magic][uint32 version][uint32 table_count]
//   For each table:
//     [uint32 name_len][name bytes]
//     [uint32 schema_name_len][schema_name bytes]
//     [uint32 column_count]
//     For each column:
//       [uint32 col_name_len][col_name bytes]
//       [uint32 type_name_len][type_name bytes]
//       [uint8  nullable]
//       [uint8  primary_key]
//       [uint8  unique]
//       [uint8  auto_increment]

static void write_u32(std::ofstream &out, uint32_t v) {
    out.write(reinterpret_cast<const char *>(&v), sizeof(v));
}

static void write_u8(std::ofstream &out, uint8_t v) {
    out.write(reinterpret_cast<const char *>(&v), 1);
}

static void write_str(std::ofstream &out, const std::string &s) {
    write_u32(out, static_cast<uint32_t>(s.size()));
    if (!s.empty()) {
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

static bool read_u32(std::ifstream &in, uint32_t &v) {
    in.read(reinterpret_cast<char *>(&v), sizeof(v));
    return in.good();
}

static bool read_u8(std::ifstream &in, uint8_t &v) {
    in.read(reinterpret_cast<char *>(&v), 1);
    return in.good();
}

static bool read_str(std::ifstream &in, std::string &s) {
    uint32_t len = 0;
    if (!read_u32(in, len)) return false;
    s.resize(len);
    if (len > 0) {
        in.read(s.data(), static_cast<std::streamsize>(len));
    }
    return in.good() || in.eof();
}

void PersistenceEngine::save_catalog(const catalog::Catalog &cat) {
    if (!is_open_) return;

    std::string path = db_path_ + "/tdb_catalog.dat";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    write_u32(out, kCatalogMagic);
    write_u32(out, kCatalogVersion);

    auto tables = cat.list_tables();
    write_u32(out, static_cast<uint32_t>(tables.size()));

    for (const auto &tname : tables) {
        const catalog::TableInfo *ti = cat.find_table(tname);
        if (!ti) continue;

        write_str(out, ti->name);
        write_str(out, ti->schema);

        write_u32(out, static_cast<uint32_t>(ti->columns.size()));

        for (const auto &col : ti->columns) {
            write_str(out, col.name);
            write_str(out, col.type.name);
            write_u8(out, col.nullable ? 1 : 0);
            write_u8(out, col.primary_key ? 1 : 0);
            write_u8(out, col.unique ? 1 : 0);
            write_u8(out, col.auto_increment ? 1 : 0);
        }
    }

    out.flush();
}

void PersistenceEngine::load_catalog(catalog::Catalog &cat) {
    if (!is_open_) return;

    std::string path = db_path_ + "/tdb_catalog.dat";
    std::ifstream in(path, std::ios::binary);
    if (!in) return; // no catalog file yet -- fresh database

    uint32_t magic = 0, version = 0, table_count = 0;
    if (!read_u32(in, magic) || magic != kCatalogMagic) return;
    if (!read_u32(in, version) || version != kCatalogVersion) return;
    if (!read_u32(in, table_count)) return;

    for (uint32_t t = 0; t < table_count; ++t) {
        std::string tname, tschema;
        if (!read_str(in, tname)) break;
        if (!read_str(in, tschema)) break;

        uint32_t col_count = 0;
        if (!read_u32(in, col_count)) break;

        catalog::TableInfo ti;
        ti.name = tname;
        ti.schema = tschema;

        for (uint32_t c = 0; c < col_count; ++c) {
            catalog::ColumnInfo ci;
            std::string type_name;

            if (!read_str(in, ci.name)) break;
            if (!read_str(in, type_name)) break;
            ci.type.name = type_name;

            uint8_t flags = 0;
            if (!read_u8(in, flags)) break;
            ci.nullable = (flags != 0);
            if (!read_u8(in, flags)) break;
            ci.primary_key = (flags != 0);
            if (!read_u8(in, flags)) break;
            ci.unique = (flags != 0);
            if (!read_u8(in, flags)) break;
            ci.auto_increment = (flags != 0);

            ci.ordinal = static_cast<uint16_t>(c);

            ti.columns.push_back(std::move(ci));
        }

        cat.add_table(tname, std::move(ti));
    }
}

} // namespace tdb::persistence
