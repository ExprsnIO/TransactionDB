#ifndef TDB_SQL_TYPECOERCE_H
#define TDB_SQL_TYPECOERCE_H

#include "tdb/sql/executor.h"
#include <string>

namespace tdb::sql {

// ─── Date/time string parsing ───

// Parse "YYYY-MM-DD" into a DATE_VAL Value.
// Returns NULL_VAL on parse failure.
Value parse_date(const std::string &s);

// Parse "HH:MM:SS" into a TIME_VAL Value.
// Returns NULL_VAL on parse failure.
Value parse_time(const std::string &s);

// Parse "YYYY-MM-DD HH:MM:SS" into a TIMESTAMP_VAL Value.
// Returns NULL_VAL on parse failure.
Value parse_timestamp(const std::string &s);

// ─── Type coercion ───

// Convert a value to the target SQL type name.
// Supported target types (case-insensitive):
//   "INT", "INTEGER", "BIGINT"  -> INT64
//   "FLOAT", "DOUBLE", "REAL"   -> FLOAT64
//   "TEXT", "VARCHAR", "STRING"  -> STRING
//   "BOOL", "BOOLEAN"           -> BOOL
//   "DATE"                      -> DATE_VAL
//   "TIME"                      -> TIME_VAL
//   "TIMESTAMP", "DATETIME"     -> TIMESTAMP_VAL
// Returns NULL_VAL if coercion is not possible.
Value coerce_value(const Value &v, const std::string &target_type);

// ─── Type compatibility checking ───

// Returns true if a value of type 'from' can be coerced to 'target_type'.
bool is_type_compatible(Value::Type from, const std::string &target_type);

// ─── Date arithmetic ───

// Add integer days to a DATE_VAL.  The interval must be INT64.
// Returns NULL_VAL if date is not DATE_VAL or interval is not INT64.
Value date_add(const Value &date, const Value &interval);

// Subtract two DATE_VAL values.  Returns INT64 representing the number
// of days between date1 and date2 (date1 - date2).
// Returns NULL_VAL if either argument is not DATE_VAL.
Value date_diff(const Value &date1, const Value &date2);

} // namespace tdb::sql

#endif // TDB_SQL_TYPECOERCE_H
