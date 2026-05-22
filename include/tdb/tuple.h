#ifndef TDB_TUPLE_H
#define TDB_TUPLE_H

#include "tdb/sql/executor.h"
#include "tdb/page.h"
#include "tdb/types.h"

#include <cstdint>
#include <cstddef>

namespace tdb::storage {

/// On-disk tuple header.
/// Layout in the serialized buffer:
///   TupleHeader | null_bitmap[null_bitmap_size] | column data ...
struct TupleHeader {
    tdb_txn_id_t xmin;             // creating transaction
    tdb_txn_id_t xmax;             // deleting transaction (0 = live)
    tdb_lsn_t    lsn;              // last modification LSN
    uint32_t     version;          // row version counter
    uint16_t     num_cols;         // number of columns
    uint16_t     null_bitmap_size; // bytes in null bitmap
    // followed by: null_bitmap[null_bitmap_size], then column data
};

/// Serialize a sql::Tuple into a byte buffer according to the given schema.
///
/// Format: TupleHeader + null bitmap + column data.
/// Column encodings:
///   NULL   - bit set in bitmap, no data bytes
///   INT64  - 8 bytes (memcpy)
///   FLOAT64- 8 bytes (memcpy of double)
///   STRING - uint32_t length prefix + string bytes (no null terminator)
///   BOOL   - 1 byte (0 or 1)
///   BLOB   - uint32_t length prefix + byte data
///   Other  - treated as STRING via Value::to_string()
///
/// Returns the number of bytes written, or 0 if buf_size is insufficient.
size_t serialize_tuple(const sql::Tuple &tuple, const sql::Schema &schema,
                       uint8_t *buf, size_t buf_size);

/// Deserialize a byte buffer back into a sql::Tuple.
/// The schema is needed to interpret the column data types correctly.
/// The schema columns map to Value types as follows:
///   - Columns are decoded in order using the same encoding as serialize_tuple.
///   - The column type is inferred from the serialized data (the type tag is
///     encoded per-value), so the schema is used for column count validation.
sql::Tuple deserialize_tuple(const uint8_t *buf, size_t len,
                             const sql::Schema &schema);

/// Compute the serialized size of a tuple without writing any data.
/// Useful for checking whether a tuple fits in a page before serializing.
size_t tuple_serialized_size(const sql::Tuple &tuple);

} // namespace tdb::storage

#endif // TDB_TUPLE_H
