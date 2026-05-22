/*
 * dbfile_v2.cpp — v2 reader/writer.
 *
 * v2 introduces a section-TOC layout so we can grow the format without
 * re-breaking it. Tables, indexes, views, matviews, saved queries,
 * tablespaces, sequences, and version history are all v2-aware sections.
 * Phase-2..4 features (documents, scripts, triggers, users) reuse the same
 * dispatch and add sections without touching this file's structure.
 *
 * Unknown TOC kinds are silently skipped → forward-compatible.
 */

#include "tdb/dbfile.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace tdb::dbfile {

// ─── Binary write/read helpers ───
static void write_u8(FILE *f, uint8_t v)   { fwrite(&v, 1, 1, f); }
static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void write_i64(FILE *f, int64_t v)  { fwrite(&v, 8, 1, f); }
static void write_f64(FILE *f, double v)   { fwrite(&v, 8, 1, f); }
static void write_str(FILE *f, const std::string &s) {
    write_u32(f, (uint32_t)s.size());
    if (!s.empty()) fwrite(s.data(), 1, s.size(), f);
}

static uint8_t read_u8(FILE *f)   { uint8_t v=0;  (void)fread(&v,1,1,f); return v; }
static uint32_t read_u32(FILE *f) { uint32_t v=0; (void)fread(&v,4,1,f); return v; }
static uint64_t read_u64(FILE *f) { uint64_t v=0; (void)fread(&v,8,1,f); return v; }
static int64_t read_i64(FILE *f)  { int64_t v=0;  (void)fread(&v,8,1,f); return v; }
static double read_f64(FILE *f)   { double v=0;   (void)fread(&v,8,1,f); return v; }
static std::string read_str(FILE *f) {
    uint32_t len = read_u32(f);
    if (len == 0) return "";
    std::string s(len, '\0');
    (void)fread(s.data(), 1, len, f);
    return s;
}

static void write_version(FILE *f, const ObjectVersion &v) {
    write_u8(f, v.major); write_u8(f, v.minor); write_u8(f, v.patch);
    write_u64(f, v.created_epoch_ms);
}
static ObjectVersion read_version(FILE *f) {
    ObjectVersion v;
    v.major = read_u8(f); v.minor = read_u8(f); v.patch = read_u8(f);
    v.created_epoch_ms = read_u64(f);
    return v;
}

static void write_value(FILE *f, const sql::Value &v) {
    switch (v.type) {
    case sql::Value::Type::NULL_VAL:    write_u8(f, 0); break;
    case sql::Value::Type::INT64:       write_u8(f, 1); write_i64(f, v.int_val); break;
    case sql::Value::Type::FLOAT64:     write_u8(f, 2); write_f64(f, v.float_val); break;
    case sql::Value::Type::STRING:      write_u8(f, 3); write_str(f, v.str_val); break;
    case sql::Value::Type::BOOL:        write_u8(f, 4); write_u8(f, v.bool_val ? 1 : 0); break;
    case sql::Value::Type::BLOB:        write_u8(f, 5); write_str(f, v.str_val); break;
    case sql::Value::Type::DATE_VAL:    write_u8(f, 6); write_i64(f, v.int_val); break;
    case sql::Value::Type::TIME_VAL:    write_u8(f, 7); write_i64(f, v.int_val); break;
    case sql::Value::Type::TIMESTAMP_VAL:write_u8(f, 8); write_i64(f, v.int_val); break;
    default:
        // Extended types: serialize as string with tag 3
        write_u8(f, 3); write_str(f, v.to_string()); break;
    }
}

static sql::Value read_value(FILE *f) {
    uint8_t tag = read_u8(f);
    switch (tag) {
    case 0: return sql::Value::make_null();
    case 1: return sql::Value::make_int(read_i64(f));
    case 2: return sql::Value::make_float(read_f64(f));
    case 3: return sql::Value::make_string(read_str(f));
    case 4: return sql::Value::make_bool(read_u8(f) != 0);
    case 5: { sql::Value v; v.type = sql::Value::Type::BLOB; v.str_val = read_str(f); return v; }
    case 6: { sql::Value v; v.type = sql::Value::Type::DATE_VAL;      v.int_val = read_i64(f); return v; }
    case 7: { sql::Value v; v.type = sql::Value::Type::TIME_VAL;      v.int_val = read_i64(f); return v; }
    case 8: { sql::Value v; v.type = sql::Value::Type::TIMESTAMP_VAL; v.int_val = read_i64(f); return v; }
    default: return sql::Value::make_null();
    }
}

// ─── Section writers ───────────────────────────────────────────────────────

static void write_sec_tables(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_tables();
    write_u32(f, (uint32_t)names.size());
    for (auto &tn : names) {
        auto *ti = cat.find_table(tn);
        if (!ti) continue;
        write_str(f, ti->name);
        write_str(f, ti->schema);
        write_str(f, ti->tablespace);
        write_version(f, ti->version);
        write_i64(f, ti->auto_increment_counter);
        write_u32(f, (uint32_t)ti->columns.size());
        for (auto &col : ti->columns) {
            write_str(f, col.name);
            write_str(f, col.type.name);
            uint8_t flags = 0;
            if (col.nullable)       flags |= 0x01;
            if (col.primary_key)    flags |= 0x02;
            if (col.unique)         flags |= 0x04;
            if (col.auto_increment) flags |= 0x08;
            if (col.encrypted)      flags |= 0x10;
            if (col.generated)      flags |= 0x20;
            write_u8(f, flags);
        }
        write_u32(f, (uint32_t)ti->rows.size());
        for (auto &row : ti->rows) {
            write_u32(f, (uint32_t)row.size());
            for (auto &val : row) write_value(f, val);
        }
    }
}

static void read_sec_tables(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::TableInfo ti;
        ti.name = read_str(f);
        ti.schema = read_str(f);
        ti.tablespace = read_str(f);
        ti.version = read_version(f);
        ti.auto_increment_counter = read_i64(f);
        uint32_t ncols = read_u32(f);
        for (uint32_t c = 0; c < ncols; c++) {
            catalog::ColumnInfo ci;
            ci.name = read_str(f);
            ci.type.name = read_str(f);
            uint8_t flags = read_u8(f);
            ci.nullable       = (flags & 0x01) != 0;
            ci.primary_key    = (flags & 0x02) != 0;
            ci.unique         = (flags & 0x04) != 0;
            ci.auto_increment = (flags & 0x08) != 0;
            ci.encrypted      = (flags & 0x10) != 0;
            ci.generated      = (flags & 0x20) != 0;
            ci.ordinal = (uint16_t)c;
            ti.columns.push_back(std::move(ci));
        }
        uint32_t nrows = read_u32(f);
        ti.rows.reserve(nrows);
        for (uint32_t r = 0; r < nrows; r++) {
            uint32_t cells = read_u32(f);
            sql::Tuple row;
            row.reserve(cells);
            for (uint32_t c = 0; c < cells; c++) row.push_back(read_value(f));
            ti.rows.push_back(std::move(row));
        }
        std::string key = ti.name;
        cat.add_table(key, std::move(ti));
    }
}

static void write_sec_indexes(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_indexes();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *ix = cat.find_index(n);
        if (!ix) continue;
        write_str(f, ix->name);
        write_str(f, ix->table);
        write_u32(f, (uint32_t)ix->method);
        write_u8(f, ix->unique ? 1 : 0);
        write_version(f, ix->version);
        write_u32(f, (uint32_t)ix->columns.size());
        for (auto &c : ix->columns) write_str(f, c);
    }
}

static void read_sec_indexes(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::IndexInfo ix;
        ix.name = read_str(f);
        ix.table = read_str(f);
        ix.method = (sql::ast::IndexMethod)read_u32(f);
        ix.unique = read_u8(f) != 0;
        ix.version = read_version(f);
        uint32_t nc = read_u32(f);
        ix.columns.reserve(nc);
        for (uint32_t k = 0; k < nc; k++) ix.columns.push_back(read_str(f));
        std::string key = ix.name;
        cat.add_index(key, std::move(ix));
    }
}

static void write_sec_sequences(FILE *f, const catalog::Catalog &cat) {
    // Sequences are looked up via find_sequence; iterate via information_schema-ish loop.
    // No public list_sequences exposed — we fall back to information_schema query.
    auto [schema, rows] = cat.get_information_schema("SEQUENCES");
    (void)schema;
    write_u32(f, (uint32_t)rows.size());
    for (auto &r : rows) {
        std::string name = r[2].str_val;  // sequence_name
        auto *si = cat.find_sequence(name);
        if (!si) continue;
        write_str(f, si->name);
        write_str(f, si->schema);
        write_i64(f, si->current_value);
        write_i64(f, si->start);
        write_i64(f, si->increment);
        write_i64(f, si->min_value);
        write_i64(f, si->max_value);
        write_u8(f, si->cycle ? 1 : 0);
        write_version(f, si->version);
    }
}

static void read_sec_sequences(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::SequenceInfo si;
        si.name = read_str(f);
        si.schema = read_str(f);
        si.current_value = read_i64(f);
        si.start = read_i64(f);
        si.increment = read_i64(f);
        si.min_value = read_i64(f);
        si.max_value = read_i64(f);
        si.cycle = read_u8(f) != 0;
        si.version = read_version(f);
        std::string key = si.name;
        cat.add_sequence(key, std::move(si));
    }
}

static void write_sec_tablespaces(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_tablespaces();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *ts = cat.find_tablespace(n);
        if (!ts) continue;
        write_str(f, ts->name);
        write_str(f, ts->location);
        write_str(f, ts->owner);
        write_version(f, ts->version);
        write_u32(f, (uint32_t)ts->options.size());
        for (auto &[k, v] : ts->options) {
            write_str(f, k);
            write_str(f, v);
        }
    }
}

static void read_sec_tablespaces(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::TablespaceInfo ts;
        ts.name = read_str(f);
        ts.location = read_str(f);
        ts.owner = read_str(f);
        ts.version = read_version(f);
        uint32_t nopt = read_u32(f);
        for (uint32_t k = 0; k < nopt; k++) {
            std::string key = read_str(f);
            std::string val = read_str(f);
            ts.options[key] = val;
        }
        std::string key = ts.name;
        cat.add_tablespace(key, std::move(ts));
    }
}

static void write_sec_documents(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_documents();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *d = cat.find_document(n);
        if (!d) continue;
        write_str(f, d->name);
        write_str(f, d->schema);
        write_u8(f, (uint8_t)d->format);   // 0=JSON 1=XML
        write_str(f, d->content);
        write_version(f, d->version);
        write_u32(f, (uint32_t)d->namespaces.size());
        for (auto &[k, v] : d->namespaces) { write_str(f, k); write_str(f, v); }
    }
}

static void read_sec_documents(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::DocumentInfo d;
        d.name = read_str(f);
        d.schema = read_str(f);
        d.format = (catalog::DocumentFormat)read_u8(f);
        d.content = read_str(f);
        d.version = read_version(f);
        uint32_t nns = read_u32(f);
        for (uint32_t k = 0; k < nns; k++) {
            std::string key = read_str(f);
            std::string val = read_str(f);
            d.namespaces[key] = val;
        }
        std::string key = d.name;
        cat.add_document(key, std::move(d));
    }
}

static void write_sec_scripts(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_scripts();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *s = cat.find_script(n);
        if (!s) continue;
        write_str(f, s->name);
        write_str(f, s->schema);
        write_str(f, s->lua_source);
        write_u8(f, s->is_udf ? 1 : 0);
        write_u8(f, s->has_return ? 1 : 0);
        write_str(f, s->return_type);
        write_version(f, s->version);
        write_u32(f, (uint32_t)s->params.size());
        for (auto &p : s->params) { write_str(f, p.name); write_str(f, p.type_name); }
    }
}

static void read_sec_scripts(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::ScriptInfo s;
        s.name = read_str(f);
        s.schema = read_str(f);
        s.lua_source = read_str(f);
        s.is_udf = read_u8(f) != 0;
        s.has_return = read_u8(f) != 0;
        s.return_type = read_str(f);
        s.version = read_version(f);
        uint32_t np = read_u32(f);
        for (uint32_t k = 0; k < np; k++) {
            catalog::ScriptParam p;
            p.name = read_str(f);
            p.type_name = read_str(f);
            s.params.push_back(std::move(p));
        }
        std::string key = s.name;
        cat.add_script(key, std::move(s));
    }
}

static void write_sec_triggers(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_triggers();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *t = cat.find_trigger(n);
        if (!t) continue;
        write_str(f, t->name);
        write_str(f, t->table);
        write_u8(f, (uint8_t)t->timing);
        write_u8(f, (uint8_t)t->event);
        write_str(f, t->script_name);
        write_version(f, t->version);
    }
}

static void read_sec_triggers(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    for (uint32_t i = 0; i < n; i++) {
        catalog::TriggerInfo t;
        t.name = read_str(f);
        t.table = read_str(f);
        t.timing = (catalog::TriggerTiming)read_u8(f);
        t.event  = (catalog::TriggerEvent)read_u8(f);
        t.script_name = read_str(f);
        t.version = read_version(f);
        std::string key = t.name;
        cat.add_trigger(key, std::move(t));
    }
}

static void write_sec_users(FILE *f, const catalog::Catalog &cat) {
    auto names = cat.list_users();
    write_u32(f, (uint32_t)names.size());
    for (auto &n : names) {
        auto *u = cat.find_user(n);
        if (!u) continue;
        write_str(f, u->username);
        fwrite(u->salt, 1, sizeof(u->salt), f);
        fwrite(u->password_hash, 1, sizeof(u->password_hash), f);
        write_u32(f, u->kdf_iterations);
        write_u8(f, u->is_superuser ? 1 : 0);
        write_version(f, u->version);
    }
    // Privileges follow, in the same section.
    const auto &privs = cat.all_privileges();
    write_u32(f, (uint32_t)privs.size());
    for (auto &p : privs) {
        write_str(f, p.grantee);
        write_str(f, p.privilege);
        write_str(f, p.object_kind);
        write_str(f, p.object_name);
        write_u8(f, p.with_grant_option ? 1 : 0);
    }
}

static void read_sec_users(FILE *f, catalog::Catalog &cat) {
    uint32_t nu = read_u32(f);
    for (uint32_t i = 0; i < nu; i++) {
        catalog::UserInfo u;
        u.username = read_str(f);
        (void)fread(u.salt, 1, sizeof(u.salt), f);
        (void)fread(u.password_hash, 1, sizeof(u.password_hash), f);
        u.kdf_iterations = read_u32(f);
        u.is_superuser = read_u8(f) != 0;
        u.version = read_version(f);
        std::string key = u.username;
        cat.add_user_raw(key, std::move(u));
    }
    uint32_t np = read_u32(f);
    std::vector<catalog::Privilege> privs;
    privs.reserve(np);
    for (uint32_t i = 0; i < np; i++) {
        catalog::Privilege p;
        p.grantee = read_str(f);
        p.privilege = read_str(f);
        p.object_kind = read_str(f);
        p.object_name = read_str(f);
        p.with_grant_option = read_u8(f) != 0;
        privs.push_back(std::move(p));
    }
    cat.load_privileges(std::move(privs));
}

static void write_sec_version_hist(FILE *f, const catalog::Catalog &cat) {
    const auto &h = cat.all_history();
    write_u32(f, (uint32_t)h.size());
    for (auto &e : h) {
        write_str(f, e.object_kind);
        write_str(f, e.object_name);
        write_version(f, e.version);
        write_u64(f, e.timestamp_ms);
        write_str(f, e.change_summary);
        write_str(f, e.serialized_snapshot);
    }
}

static void read_sec_version_hist(FILE *f, catalog::Catalog &cat) {
    uint32_t n = read_u32(f);
    std::vector<VersionHistoryEntry> h;
    h.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        VersionHistoryEntry e;
        e.object_kind = read_str(f);
        e.object_name = read_str(f);
        e.version = read_version(f);
        e.timestamp_ms = read_u64(f);
        e.change_summary = read_str(f);
        e.serialized_snapshot = read_str(f);
        h.push_back(std::move(e));
    }
    cat.load_history(std::move(h));
}

// ─── Save/load ─────────────────────────────────────────────────────────────

namespace v2 {

bool save(const std::string &path, const catalog::Catalog &cat) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;

    // Reserve space for the header — backfilled at the end.
    FileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "TDB", 4);
    hdr.version = 2;
    hdr.page_size = 8192;
    hdr.db_mode = (uint32_t)cat.db_mode();
    {
        uint32_t flags = 0;
        if (!cat.list_users().empty())     flags |= 0x01;  // has_users
        if (!cat.list_scripts().empty())   flags |= 0x02;  // has_scripts
        if (!cat.list_documents().empty()) flags |= 0x04;  // has_docs
        hdr.flags = flags;
    }
    hdr.toc_entry_size = sizeof(SectionTOCEntry);
    fwrite(&hdr, sizeof(hdr), 1, f);

    // Write each section payload, remembering offset/size.
    std::vector<SectionTOCEntry> toc;
    auto emit = [&](SectionKind kind, auto writer) {
        uint64_t off = (uint64_t)ftell(f);
        writer(f, cat);
        uint64_t sz = (uint64_t)ftell(f) - off;
        SectionTOCEntry e{};
        e.kind = (uint32_t)kind;
        e.flags = 0;
        e.offset = off;
        e.size = sz;
        e.checksum = 0;
        toc.push_back(e);
    };

    emit(SEC_TABLES,        write_sec_tables);
    emit(SEC_INDEXES,       write_sec_indexes);
    emit(SEC_SEQUENCES,     write_sec_sequences);
    emit(SEC_TABLESPACES,   write_sec_tablespaces);
    emit(SEC_DOCUMENTS,     write_sec_documents);
    emit(SEC_SCRIPTS,       write_sec_scripts);
    emit(SEC_TRIGGERS,      write_sec_triggers);
    emit(SEC_USERS,         write_sec_users);
    emit(SEC_VERSION_HIST,  write_sec_version_hist);

    // Write TOC at end.
    uint64_t toc_offset = (uint64_t)ftell(f);
    fwrite(toc.data(), sizeof(SectionTOCEntry), toc.size(), f);

    // Backfill header.
    hdr.toc_offset = toc_offset;
    hdr.toc_count = (uint32_t)toc.size();
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);

    fclose(f);
    return true;
}

bool load(const std::string &path, catalog::Catalog &cat) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    FileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
    if (memcmp(hdr.magic, "TDB", 3) != 0)   { fclose(f); return false; }
    if (hdr.version != 2)                    { fclose(f); return false; }
    if (hdr.toc_entry_size != sizeof(SectionTOCEntry)) { fclose(f); return false; }

    // Restore the database access mode from the header.
    cat.set_db_mode((catalog::DatabaseMode)hdr.db_mode);

    // Read TOC.
    std::vector<SectionTOCEntry> toc(hdr.toc_count);
    fseek(f, (long)hdr.toc_offset, SEEK_SET);
    if (hdr.toc_count > 0 &&
        fread(toc.data(), sizeof(SectionTOCEntry), hdr.toc_count, f) != hdr.toc_count) {
        fclose(f);
        return false;
    }

    // Dispatch known section kinds; silently skip unknown.
    for (auto &e : toc) {
        fseek(f, (long)e.offset, SEEK_SET);
        switch ((SectionKind)e.kind) {
            case SEC_TABLES:        read_sec_tables(f, cat); break;
            case SEC_INDEXES:       read_sec_indexes(f, cat); break;
            case SEC_SEQUENCES:     read_sec_sequences(f, cat); break;
            case SEC_TABLESPACES:   read_sec_tablespaces(f, cat); break;
            case SEC_DOCUMENTS:     read_sec_documents(f, cat); break;
            case SEC_SCRIPTS:       read_sec_scripts(f, cat); break;
            case SEC_TRIGGERS:      read_sec_triggers(f, cat); break;
            case SEC_USERS:         read_sec_users(f, cat); break;
            case SEC_VERSION_HIST:  read_sec_version_hist(f, cat); break;
            // SEC_DOCUMENTS / SEC_SCRIPTS / SEC_TRIGGERS / SEC_USERS / etc.
            // are reserved for future phases — silently skip.
            default: break;
        }
    }

    fclose(f);
    return true;
}

} // namespace v2
} // namespace tdb::dbfile
