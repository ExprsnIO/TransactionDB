#ifndef TDB_DBFILE_H
#define TDB_DBFILE_H

#include "tdb/sql/executor.h"
#include "tdb/catalog.h"
#include <string>
#include <vector>
#include <cstdint>

namespace tdb::dbfile {

/*
 * Single-file binary database format (.tdb)
 *
 * The format supports two on-disk versions. v2 is the current format and the
 * only one written by save(). v1 files are read transparently (legacy
 * databases produced by TDB <= v1.0.0-GM) and silently upgraded on next save.
 *
 * ─── v2 layout ────────────────────────────────────────────────────────────
 *   [FileHeader (64 bytes)]
 *   [Section payload 0]
 *   [Section payload 1]
 *   ...
 *   [Section TOC: toc_count × SectionTOCEntry (32 bytes each)]
 *
 * FileHeader:
 *   magic[4]         = "TDB\0"
 *   version          = uint32_t (2)
 *   page_size        = uint32_t (8192)
 *   db_mode          = uint32_t (0=ANONYMOUS, 1=SINGLE_USER, 2=MULTI_USER)
 *   flags            = uint32_t (bit0=has_users, bit1=has_scripts, bit2=has_docs)
 *   toc_offset       = uint64_t
 *   toc_count        = uint32_t
 *   toc_entry_size   = uint32_t (sizeof(SectionTOCEntry))
 *   reserved[24]
 *
 * SectionTOCEntry (32 bytes, repeated toc_count times at toc_offset):
 *   kind             = uint32_t (see SectionKind)
 *   flags            = uint32_t
 *   offset           = uint64_t
 *   size             = uint64_t
 *   checksum         = uint64_t (CRC64 of payload; 0 means "not computed")
 *
 * Unknown TOC kinds are silently skipped → forward-compatible.
 *
 * ─── v1 layout (legacy) ───────────────────────────────────────────────────
 *   FileHeader had `num_tables`, `catalog_offset`, `catalog_size`; payload
 *   was a single catalog blob + sequential table-data sections. Preserved in
 *   src/dbfile/dbfile_v1.cpp for compatibility.
 */

#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];        // "TDB\0"
    uint32_t version;         // 1 or 2
    uint32_t page_size;       // 8192
    uint32_t db_mode;         // v2: 0=ANON, 1=SINGLE, 2=MULTI (v1: unused, was num_tables)
    uint32_t flags;           // v2: feature bits        (v1: unused, was first 4 bytes of catalog_offset)
    uint64_t toc_offset;      // v2: section TOC offset  (v1: catalog_offset (overlapping layout))
    uint32_t toc_count;       // v2: number of TOC entries
    uint32_t toc_entry_size;  // v2: sizeof(SectionTOCEntry)
    uint8_t  reserved[28];
};

struct SectionTOCEntry {
    uint32_t kind;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint64_t checksum;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 64, "FileHeader must be 64 bytes");
static_assert(sizeof(SectionTOCEntry) == 32, "SectionTOCEntry must be 32 bytes");

enum SectionKind : uint32_t {
    SEC_TABLES        = 1,
    SEC_INDEXES       = 2,
    SEC_VIEWS         = 3,
    SEC_MATVIEWS      = 4,
    SEC_SAVED_QUERIES = 5,
    SEC_TABLESPACES   = 6,
    SEC_SEQUENCES     = 7,
    SEC_DOCUMENTS     = 8,   // Phase 2
    SEC_SCRIPTS       = 9,   // Phase 3
    SEC_TRIGGERS      = 10,  // Phase 3
    SEC_USERS         = 11,  // Phase 4
    SEC_VERSION_HIST  = 12,
};

// Detect file format version without loading. Returns 0 on error.
int detect_version(const std::string &path);

// Save entire database (catalog + all data) to a single binary file. Always v2.
bool save(const std::string &path, const catalog::Catalog &cat);

// Load entire database. Auto-detects v1 vs v2.
bool load(const std::string &path, catalog::Catalog &cat);

// ─── Internal: v1 reader (exposed for tests) ────────────────────────────
namespace v1 {
    bool load(const std::string &path, catalog::Catalog &cat);
}

namespace v2 {
    bool save(const std::string &path, const catalog::Catalog &cat);
    bool load(const std::string &path, catalog::Catalog &cat);
}

} // namespace tdb::dbfile

#endif // TDB_DBFILE_H
