#include "tdb/tuple.h"

#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace tdb::storage {

// ──────────────────────────────────────────────────────────────────────
// Internal helpers
// ──────────────────────────────────────────────────────────────────────

/// Number of bytes needed for a null bitmap covering `num_cols` columns.
static inline uint16_t bitmap_bytes(uint16_t num_cols) {
    return static_cast<uint16_t>((num_cols + 7) / 8);
}

/// Per-value type tag stored in the column data stream so that
/// deserialize_tuple can reconstruct the correct Value::Type without
/// needing a separate type catalog.
///
/// Tags 0-5 are the original v2 set; they keep their on-disk encoding so old
/// dbfiles round-trip unchanged. Tags 16-31 are the v3 extension for lossless
/// preservation of extended Value types added in Batches 5/5b/3.
enum class WireType : uint8_t {
    NULL_VAL      = 0,
    INT64         = 1,
    FLOAT64       = 2,
    STRING        = 3,
    BOOL          = 4,
    BLOB          = 5,
    // v3 lossless tags
    DATE_V3       = 16,   // int64
    TIME_V3       = 17,   // int64
    TIMESTAMP_V3  = 18,   // int64
    TIMESTAMP_TZ  = 19,   // int64 utc + int32 offset_minutes
    DECIMAL_V3    = 20,   // int32 scale + len-prefixed canonical text
    UUID_V3       = 21,   // 16 fixed bytes
    INTERVAL_V3   = 22,   // int64 months + int64 micros
    ENUM_V3       = 23,   // int64 ordinal + len-prefixed type name
    BIT_V3        = 24,   // len-prefixed '0'/'1' string
    JSON_V3       = 25,   // len-prefixed UTF-8 text
    XML_V3        = 26,   // len-prefixed UTF-8 text
    VARBINARY_V3  = 27,   // len-prefixed raw bytes (distinct from BLOB by intent)
    GEOMETRY_V3   = 28,   // int32 srid + int32 dim + len-prefixed WKT
    COMPOSITE_V3  = 29,   // len-prefixed type name + uint32 nfields + recursive (tag+payload)*nfields
    ARRAY_V3      = 30,   // uint32 nelems + recursive (tag+payload)*nelems
    MULTISET_V3   = 31,   // uint32 nelems + recursive (tag+payload)*nelems
    INT64_VARINT  = 32,   // zig-zag varint INT64; chosen when shorter than fixed 8 bytes
};

// Forward declarations for varint helpers — bodies live further down so that
// value_data_size can use them in its size calculation.
static size_t varint_size(uint64_t v);
static inline uint64_t zigzag_encode(int64_t v);
static inline bool int64_fits_varint(int64_t v);

static WireType value_to_wire(sql::Value::Type t) {
    switch (t) {
        case sql::Value::Type::NULL_VAL:      return WireType::NULL_VAL;
        case sql::Value::Type::INT64:         return WireType::INT64;
        case sql::Value::Type::FLOAT64:       return WireType::FLOAT64;
        case sql::Value::Type::STRING:        return WireType::STRING;
        case sql::Value::Type::BOOL:          return WireType::BOOL;
        case sql::Value::Type::BLOB:          return WireType::BLOB;
        case sql::Value::Type::DATE_VAL:      return WireType::DATE_V3;
        case sql::Value::Type::TIME_VAL:      return WireType::TIME_V3;
        case sql::Value::Type::TIMESTAMP_VAL: return WireType::TIMESTAMP_V3;
        case sql::Value::Type::TIMESTAMP_TZ:  return WireType::TIMESTAMP_TZ;
        case sql::Value::Type::DECIMAL:       return WireType::DECIMAL_V3;
        case sql::Value::Type::UUID:          return WireType::UUID_V3;
        case sql::Value::Type::INTERVAL:      return WireType::INTERVAL_V3;
        case sql::Value::Type::ENUM_VAL:      return WireType::ENUM_V3;
        case sql::Value::Type::BIT_VAL:       return WireType::BIT_V3;
        case sql::Value::Type::JSON_VAL:      return WireType::JSON_V3;
        case sql::Value::Type::XML_VAL:       return WireType::XML_V3;
        case sql::Value::Type::VARBINARY:     return WireType::VARBINARY_V3;
        case sql::Value::Type::GEOMETRY:      return WireType::GEOMETRY_V3;
        case sql::Value::Type::COMPOSITE:     return WireType::COMPOSITE_V3;
        case sql::Value::Type::ARRAY:         return WireType::ARRAY_V3;
        case sql::Value::Type::MULTISET:      return WireType::MULTISET_V3;
    }
    return WireType::STRING; // unknown -> treat as string
}

/// Compute the data-section size for a single non-null Value.
/// This does NOT include the 1-byte type tag (written separately).
static size_t value_data_size(const sql::Value &v) {
    switch (v.type) {
        case sql::Value::Type::NULL_VAL: return 0;
        case sql::Value::Type::INT64:    return int64_fits_varint(v.int_val)
                                                ? varint_size(zigzag_encode(v.int_val))
                                                : sizeof(int64_t);
        case sql::Value::Type::FLOAT64:  return sizeof(double);
        case sql::Value::Type::STRING:   return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::BOOL:     return 1;
        case sql::Value::Type::BLOB:     return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::DATE_VAL:
        case sql::Value::Type::TIME_VAL:
        case sql::Value::Type::TIMESTAMP_VAL: return sizeof(int64_t);
        case sql::Value::Type::TIMESTAMP_TZ:  return sizeof(int64_t) + sizeof(int32_t);
        case sql::Value::Type::DECIMAL: return sizeof(int32_t) + sizeof(uint32_t) + v.str_val.size();
        // UUID stored as 16 raw bytes — if str_val carries the 36-char canonical
        // form we still serialize 16 bytes by parsing the hex.
        case sql::Value::Type::UUID:    return 16;
        case sql::Value::Type::INTERVAL: return sizeof(int64_t) + sizeof(int64_t);
        case sql::Value::Type::ENUM_VAL: return sizeof(int64_t) + sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::BIT_VAL:  return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::JSON_VAL: return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::XML_VAL:  return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::VARBINARY: return sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::GEOMETRY: return sizeof(int32_t) + sizeof(int32_t) + sizeof(uint32_t) + v.str_val.size();
        case sql::Value::Type::COMPOSITE: {
            size_t total = sizeof(uint32_t) + v.str_val.size() + sizeof(uint32_t);
            if (v.composite_fields)
                for (auto &f : *v.composite_fields)
                    total += 1 + (f.is_null() ? 0 : value_data_size(f));
            return total;
        }
        case sql::Value::Type::ARRAY:
        case sql::Value::Type::MULTISET: {
            size_t total = sizeof(uint32_t);
            if (v.composite_fields)
                for (auto &e : *v.composite_fields)
                    total += 1 + (e.is_null() ? 0 : value_data_size(e));
            return total;
        }
    }
    std::string s = v.to_string();
    return sizeof(uint32_t) + s.size();
}

// ──────────────────────────────────────────────────────────────────────
// tuple_serialized_size
// ──────────────────────────────────────────────────────────────────────

size_t tuple_serialized_size(const sql::Tuple &tuple) {
    uint16_t num_cols = static_cast<uint16_t>(tuple.size());
    uint16_t bm_size = bitmap_bytes(num_cols);

    size_t total = sizeof(TupleHeader) + bm_size;

    for (const auto &v : tuple) {
        // 1-byte type tag per column
        total += 1;
        if (!v.is_null()) {
            total += value_data_size(v);
        }
    }
    return total;
}

// ──────────────────────────────────────────────────────────────────────
// Per-value writer / reader (used by serialize_tuple AND recursively by
// the COMPOSITE encoding). Writes the 1-byte type tag plus payload.
// ──────────────────────────────────────────────────────────────────────

static inline void write_u32(uint8_t *&p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); p += sizeof(v); }
static inline void write_i32(uint8_t *&p, int32_t v)  { std::memcpy(p, &v, sizeof(v)); p += sizeof(v); }
static inline void write_i64(uint8_t *&p, int64_t v)  { std::memcpy(p, &v, sizeof(v)); p += sizeof(v); }
static inline uint32_t read_u32(const uint8_t *&p)    { uint32_t v; std::memcpy(&v, p, sizeof(v)); p += sizeof(v); return v; }
static inline int32_t  read_i32(const uint8_t *&p)    { int32_t  v; std::memcpy(&v, p, sizeof(v)); p += sizeof(v); return v; }
static inline int64_t  read_i64(const uint8_t *&p)    { int64_t  v; std::memcpy(&v, p, sizeof(v)); p += sizeof(v); return v; }

// Varint (Protobuf-style) encoding for INT64. Uses zig-zag mapping so signed
// values map to unsigned: 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ...
// Returns number of bytes used (1-10).
static size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; n++; }
    return n;
}
static inline uint64_t zigzag_encode(int64_t v) {
    return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}
static inline int64_t zigzag_decode(uint64_t v) {
    return (int64_t)((v >> 1) ^ -(v & 1));
}
static void write_varint(uint8_t *&p, uint64_t v) {
    while (v >= 0x80) { *p++ = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
    *p++ = (uint8_t)(v & 0x7F);
}
static uint64_t read_varint(const uint8_t *&p, const uint8_t *end) {
    uint64_t out = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        out |= (uint64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return out;
        shift += 7;
        if (shift >= 64) throw std::runtime_error("varint: overflow");
    }
    throw std::runtime_error("varint: truncated");
}
// Decide whether to emit an INT64 as varint or fixed 8 bytes. Varint wins
// when zig-zagged value fits in 7 bytes — typical for row IDs / small counts.
static inline bool int64_fits_varint(int64_t v) {
    return varint_size(zigzag_encode(v)) <= 7;
}

// Pack a 36-char UUID canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into
// 16 bytes; if input isn't well-formed we zero-fill (lossy fallback).
static void pack_uuid_bytes(const std::string &s, uint8_t out[16]) {
    std::memset(out, 0, 16);
    int hi = 0;
    for (char c : s) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        else continue;
        if (hi >= 32) break;
        if ((hi & 1) == 0) out[hi/2]  = (uint8_t)(v << 4);
        else               out[hi/2] |= (uint8_t)v;
        hi++;
    }
}
static std::string unpack_uuid_bytes(const uint8_t b[16]) {
    static const char *hex = "0123456789abcdef";
    std::string s; s.reserve(36);
    for (int i = 0; i < 16; i++) {
        s.push_back(hex[(b[i] >> 4) & 0xF]);
        s.push_back(hex[b[i] & 0xF]);
        if (i == 3 || i == 5 || i == 7 || i == 9) s.push_back('-');
    }
    return s;
}

static void write_value(uint8_t *&ptr, const sql::Value &v);
static sql::Value read_value(const uint8_t *&ptr, const uint8_t *end);

static void write_value(uint8_t *&ptr, const sql::Value &v) {
    using VT = sql::Value::Type;
    // INT64 -> varint when shorter. Different tag, different payload.
    if (v.type == VT::INT64 && !v.is_null() && int64_fits_varint(v.int_val)) {
        *ptr++ = (uint8_t)WireType::INT64_VARINT;
        write_varint(ptr, zigzag_encode(v.int_val));
        return;
    }
    auto wt = value_to_wire(v.type);
    *ptr++ = (uint8_t)wt;
    if (v.is_null()) return;
    switch (v.type) {
        case VT::NULL_VAL: break;
        case VT::INT64:    write_i64(ptr, v.int_val); break;
        case VT::FLOAT64:  std::memcpy(ptr, &v.float_val, sizeof(double)); ptr += sizeof(double); break;
        case VT::STRING:
        case VT::JSON_VAL:
        case VT::XML_VAL:
        case VT::BIT_VAL: {
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            break;
        }
        case VT::BOOL: *ptr++ = v.bool_val ? 1 : 0; break;
        case VT::BLOB:
        case VT::VARBINARY: {
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            break;
        }
        case VT::DATE_VAL:
        case VT::TIME_VAL:
        case VT::TIMESTAMP_VAL: write_i64(ptr, v.int_val); break;
        case VT::TIMESTAMP_TZ:
            write_i64(ptr, v.int_val);
            write_i32(ptr, (int32_t)v.int_val_2);
            break;
        case VT::DECIMAL:
            write_i32(ptr, (int32_t)v.int_val); // scale
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            break;
        case VT::UUID: {
            uint8_t bytes[16];
            pack_uuid_bytes(v.str_val, bytes);
            std::memcpy(ptr, bytes, 16); ptr += 16;
            break;
        }
        case VT::INTERVAL:
            write_i64(ptr, v.int_val);
            write_i64(ptr, v.int_val_2);
            break;
        case VT::ENUM_VAL:
            write_i64(ptr, v.int_val);
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            break;
        case VT::GEOMETRY:
            write_i32(ptr, (int32_t)v.int_val);
            write_i32(ptr, (int32_t)v.int_val_2);
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            break;
        case VT::COMPOSITE: {
            write_u32(ptr, (uint32_t)v.str_val.size());
            if (!v.str_val.empty()) { std::memcpy(ptr, v.str_val.data(), v.str_val.size()); ptr += v.str_val.size(); }
            uint32_t n = v.composite_fields ? (uint32_t)v.composite_fields->size() : 0;
            write_u32(ptr, n);
            if (v.composite_fields)
                for (auto &f : *v.composite_fields) write_value(ptr, f);
            break;
        }
        case VT::ARRAY:
        case VT::MULTISET: {
            uint32_t n = v.composite_fields ? (uint32_t)v.composite_fields->size() : 0;
            write_u32(ptr, n);
            if (v.composite_fields)
                for (auto &e : *v.composite_fields) write_value(ptr, e);
            break;
        }
    }
}

static sql::Value read_value(const uint8_t *&ptr, const uint8_t *end) {
    if (ptr >= end) throw std::runtime_error("read_value: out of data");
    auto wt = (WireType)(*ptr++);
    auto need = [&](size_t n) { if (ptr + n > end) throw std::runtime_error("read_value: truncated"); };
    auto read_lp_string = [&]() {
        need(sizeof(uint32_t));
        uint32_t n = read_u32(ptr);
        need(n);
        std::string s(reinterpret_cast<const char*>(ptr), n);
        ptr += n;
        return s;
    };
    switch (wt) {
        case WireType::NULL_VAL: return sql::Value::make_null();
        case WireType::INT64:    need(8); return sql::Value::make_int(read_i64(ptr));
        case WireType::INT64_VARINT: {
            uint64_t z = read_varint(ptr, end);
            return sql::Value::make_int(zigzag_decode(z));
        }
        case WireType::FLOAT64:  { need(8); double d; std::memcpy(&d, ptr, 8); ptr += 8; return sql::Value::make_float(d); }
        case WireType::STRING:   return sql::Value::make_string(read_lp_string());
        case WireType::BOOL:     { need(1); bool b = *ptr++ != 0; return sql::Value::make_bool(b); }
        case WireType::BLOB: {
            std::string s = read_lp_string();
            sql::Value v; v.type = sql::Value::Type::BLOB; v.str_val = std::move(s); return v;
        }
        case WireType::DATE_V3:        { need(8); sql::Value v; v.type = sql::Value::Type::DATE_VAL;      v.int_val = read_i64(ptr); return v; }
        case WireType::TIME_V3:        { need(8); sql::Value v; v.type = sql::Value::Type::TIME_VAL;      v.int_val = read_i64(ptr); return v; }
        case WireType::TIMESTAMP_V3:   { need(8); sql::Value v; v.type = sql::Value::Type::TIMESTAMP_VAL; v.int_val = read_i64(ptr); return v; }
        case WireType::TIMESTAMP_TZ:   {
            need(12);
            int64_t utc = read_i64(ptr); int32_t off = read_i32(ptr);
            return sql::Value::make_timestamp_tz(utc, off);
        }
        case WireType::DECIMAL_V3:     {
            need(4); int32_t scale = read_i32(ptr);
            std::string s = read_lp_string();
            return sql::Value::make_decimal(std::move(s), scale);
        }
        case WireType::UUID_V3: {
            need(16);
            uint8_t b[16]; std::memcpy(b, ptr, 16); ptr += 16;
            return sql::Value::make_uuid(unpack_uuid_bytes(b));
        }
        case WireType::INTERVAL_V3: {
            need(16);
            int64_t mo = read_i64(ptr); int64_t mi = read_i64(ptr);
            return sql::Value::make_interval(mo, mi);
        }
        case WireType::ENUM_V3: {
            need(8); int64_t ord = read_i64(ptr);
            std::string tn = read_lp_string();
            return sql::Value::make_enum(std::move(tn), ord);
        }
        case WireType::BIT_V3:        return sql::Value::make_bit(read_lp_string());
        case WireType::JSON_V3:       return sql::Value::make_json(read_lp_string());
        case WireType::XML_V3:        return sql::Value::make_xml(read_lp_string());
        case WireType::VARBINARY_V3:  return sql::Value::make_varbinary(read_lp_string());
        case WireType::GEOMETRY_V3: {
            need(8); int32_t srid = read_i32(ptr); int32_t dim = read_i32(ptr);
            std::string wkt = read_lp_string();
            return sql::Value::make_geometry(std::move(wkt), srid, dim);
        }
        case WireType::COMPOSITE_V3: {
            std::string tname = read_lp_string();
            need(sizeof(uint32_t)); uint32_t n = read_u32(ptr);
            std::vector<sql::Value> fields; fields.reserve(n);
            for (uint32_t i = 0; i < n; i++) fields.push_back(read_value(ptr, end));
            return sql::Value::make_composite(std::move(tname), std::move(fields));
        }
        case WireType::ARRAY_V3: {
            need(sizeof(uint32_t)); uint32_t n = read_u32(ptr);
            std::vector<sql::Value> elems; elems.reserve(n);
            for (uint32_t i = 0; i < n; i++) elems.push_back(read_value(ptr, end));
            return sql::Value::make_array(std::move(elems));
        }
        case WireType::MULTISET_V3: {
            need(sizeof(uint32_t)); uint32_t n = read_u32(ptr);
            std::vector<sql::Value> elems; elems.reserve(n);
            for (uint32_t i = 0; i < n; i++) elems.push_back(read_value(ptr, end));
            return sql::Value::make_multiset(std::move(elems));
        }
    }
    throw std::runtime_error("read_value: unknown wire type");
}

// ──────────────────────────────────────────────────────────────────────
// serialize_tuple
// ──────────────────────────────────────────────────────────────────────

size_t serialize_tuple(const sql::Tuple &tuple, const sql::Schema & /*schema*/,
                       uint8_t *buf, size_t buf_size) {
    size_t needed = tuple_serialized_size(tuple);
    if (needed > buf_size) {
        return 0; // not enough space
    }

    uint16_t num_cols = static_cast<uint16_t>(tuple.size());
    uint16_t bm_size = bitmap_bytes(num_cols);

    // ── Write header ──
    TupleHeader hdr{};
    hdr.xmin = 0;
    hdr.xmax = 0;
    hdr.lsn = 0;
    hdr.version = 0;
    hdr.num_cols = num_cols;
    hdr.null_bitmap_size = bm_size;
    std::memcpy(buf, &hdr, sizeof(TupleHeader));

    // ── Write null bitmap ──
    uint8_t *bm = buf + sizeof(TupleHeader);
    std::memset(bm, 0, bm_size);
    for (uint16_t i = 0; i < num_cols; ++i) {
        if (tuple[i].is_null()) {
            bm[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }

    // ── Write column data via the lossless per-value writer. ──
    uint8_t *ptr = bm + bm_size;
    for (uint16_t i = 0; i < num_cols; ++i) write_value(ptr, tuple[i]);
    return static_cast<size_t>(ptr - buf);
}

// ──────────────────────────────────────────────────────────────────────
// deserialize_tuple
// ──────────────────────────────────────────────────────────────────────

sql::Tuple deserialize_tuple(const uint8_t *buf, size_t len,
                             const sql::Schema & /*schema*/) {
    if (len < sizeof(TupleHeader)) {
        throw std::runtime_error("deserialize_tuple: buffer too small for header");
    }

    TupleHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(TupleHeader));

    uint16_t num_cols = hdr.num_cols;
    uint16_t bm_size = hdr.null_bitmap_size;

    if (len < sizeof(TupleHeader) + bm_size) {
        throw std::runtime_error("deserialize_tuple: buffer too small for null bitmap");
    }

    const uint8_t *bm = buf + sizeof(TupleHeader);
    const uint8_t *ptr = bm + bm_size;
    const uint8_t *end = buf + len;

    sql::Tuple tuple;
    tuple.reserve(num_cols);
    for (uint16_t i = 0; i < num_cols; ++i) {
        bool is_null_bit = (bm[i / 8] & (1u << (i % 8))) != 0;
        // read_value still consumes the type tag for NULL rows so that the
        // stream stays aligned; if the null bit is set we discard the value.
        sql::Value v = read_value(ptr, end);
        if (is_null_bit) tuple.push_back(sql::Value::make_null());
        else tuple.push_back(std::move(v));
    }
    return tuple;
}

} // namespace tdb::storage
