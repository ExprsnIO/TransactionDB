/*
 * dbfile_v1.cpp — Legacy v1 reader.
 *
 * The v1 format was tables-only: a single catalog blob immediately after
 * FileHeader, followed by table-data sections. We retain a read path so
 * databases produced by TDB <= v1.0.0-GM can be transparently loaded and
 * upgraded to v2 on next save. v1 is never written.
 */

#include "tdb/dbfile.h"
#include "tdb/version_object.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace tdb::dbfile {

// ─── Binary read helpers ───
static uint8_t read_u8(FILE *f) { uint8_t v = 0; (void)fread(&v, 1, 1, f); return v; }
static uint16_t read_u16(FILE *f) { uint16_t v = 0; (void)fread(&v, 2, 1, f); return v; }
static uint32_t read_u32(FILE *f) { uint32_t v = 0; (void)fread(&v, 4, 1, f); return v; }
static uint64_t read_u64(FILE *f) { uint64_t v = 0; (void)fread(&v, 8, 1, f); return v; }
static int64_t read_i64(FILE *f) { int64_t v = 0; (void)fread(&v, 8, 1, f); return v; }
static double read_f64(FILE *f) { double v = 0; (void)fread(&v, 8, 1, f); return v; }
static std::string read_str(FILE *f) {
    uint16_t len = read_u16(f);
    if (len == 0) return "";
    std::string s(len, '\0');
    (void)fread(s.data(), 1, len, f);
    return s;
}

static sql::Value read_value(FILE *f) {
    uint8_t tag = read_u8(f);
    switch (tag) {
    case 0: return sql::Value::make_null();
    case 1: return sql::Value::make_int(read_i64(f));
    case 2: return sql::Value::make_float(read_f64(f));
    case 3: {
        uint32_t len = read_u32(f);
        std::string s(len, '\0');
        if (len > 0) (void)fread(s.data(), 1, len, f);
        return sql::Value::make_string(std::move(s));
    }
    case 4: return sql::Value::make_bool(read_u8(f) != 0);
    case 5: {
        uint32_t len = read_u32(f);
        std::string s(len, '\0');
        if (len > 0) (void)fread(s.data(), 1, len, f);
        sql::Value v; v.type = sql::Value::Type::BLOB; v.str_val = std::move(s);
        return v;
    }
    case 6: { sql::Value v; v.type = sql::Value::Type::DATE_VAL; v.int_val = read_i64(f); return v; }
    case 7: { sql::Value v; v.type = sql::Value::Type::TIME_VAL; v.int_val = read_i64(f); return v; }
    case 8: { sql::Value v; v.type = sql::Value::Type::TIMESTAMP_VAL; v.int_val = read_i64(f); return v; }
    default: return sql::Value::make_null();
    }
}

namespace v1 {

bool load(const std::string &path, catalog::Catalog &cat) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    /*
     * v1 FileHeader layout (the original):
     *   magic[4], version u32, page_size u32, num_tables u32,
     *   catalog_offset u64, catalog_size u64, reserved[28]
     *
     * We reach into the raw 64-byte header rather than the v2 struct
     * because the field ordering differs starting at byte 12.
     */
    uint8_t header[64];
    if (fread(header, 1, 64, f) != 64) { fclose(f); return false; }
    if (memcmp(header, "TDB", 3) != 0) { fclose(f); return false; }

    uint32_t version, page_size, num_tables;
    uint64_t catalog_offset, catalog_size;
    memcpy(&version,        header + 4,  4);
    memcpy(&page_size,      header + 8,  4);
    memcpy(&num_tables,     header + 12, 4);
    memcpy(&catalog_offset, header + 16, 8);
    memcpy(&catalog_size,   header + 24, 8);
    (void)page_size; (void)catalog_size;

    if (version != 1) { fclose(f); return false; }

    fseek(f, (long)catalog_offset, SEEK_SET);
    uint32_t num_tables2 = read_u32(f);
    if (num_tables2 != num_tables) {
        // Defensive: trust header count
    }

    struct TableMeta {
        std::string name;
        std::vector<catalog::ColumnInfo> columns;
        int64_t auto_inc = 0;
    };
    std::vector<TableMeta> metas;

    for (uint32_t t = 0; t < num_tables; t++) {
        TableMeta meta;
        meta.name = read_str(f);
        uint16_t num_cols = read_u16(f);
        for (uint16_t c = 0; c < num_cols; c++) {
            catalog::ColumnInfo ci;
            ci.name = read_str(f);
            ci.type.name = read_str(f);
            uint8_t flags = read_u8(f);
            ci.nullable      = (flags & 0x01) != 0;
            ci.primary_key   = (flags & 0x02) != 0;
            ci.unique        = (flags & 0x04) != 0;
            ci.auto_increment= (flags & 0x08) != 0;
            ci.encrypted     = (flags & 0x10) != 0;
            ci.generated     = (flags & 0x20) != 0;
            ci.ordinal = c;
            meta.columns.push_back(std::move(ci));
        }
        meta.auto_inc = read_i64(f);
        metas.push_back(std::move(meta));
    }

    for (auto &meta : metas) {
        catalog::TableInfo ti;
        ti.name = meta.name;
        ti.columns = std::move(meta.columns);
        ti.auto_increment_counter = meta.auto_inc;
        ti.version = ObjectVersion::initial();
        cat.add_table(ti.name, std::move(ti));
    }

    for (uint32_t t = 0; t < num_tables; t++) {
        std::string tname = read_str(f);
        uint64_t num_rows = read_u64(f);
        uint16_t num_cols = read_u16(f);

        auto *ti = cat.find_table(tname);
        if (!ti) {
            for (uint64_t r = 0; r < num_rows; r++)
                for (uint16_t c = 0; c < num_cols; c++) read_value(f);
            continue;
        }

        ti->rows.reserve((size_t)num_rows);
        for (uint64_t r = 0; r < num_rows; r++) {
            sql::Tuple row;
            row.reserve(num_cols);
            for (uint16_t c = 0; c < num_cols; c++) row.push_back(read_value(f));
            ti->rows.push_back(std::move(row));
        }
    }

    fclose(f);
    return true;
}

} // namespace v1
} // namespace tdb::dbfile
