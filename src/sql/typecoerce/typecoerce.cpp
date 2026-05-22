#include "tdb/sql/typecoerce.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace tdb::sql {

// ─── Internal helpers ───
namespace {

// Uppercase a string for case-insensitive comparison.
std::string to_upper(const std::string &s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// Try to parse an integer from a string.  Returns (value, success).
std::pair<int64_t, bool> try_parse_int64(const std::string &s) {
    if (s.empty()) return {0, false};
    const char *start = s.c_str();
    char *end = nullptr;
    long long val = std::strtoll(start, &end, 10);
    // Check that we consumed the entire string (possibly with trailing spaces)
    while (*end == ' ' || *end == '\t') ++end;
    if (end == start || *end != '\0') return {0, false};
    return {static_cast<int64_t>(val), true};
}

// Try to parse a double from a string.  Returns (value, success).
std::pair<double, bool> try_parse_double(const std::string &s) {
    if (s.empty()) return {0.0, false};
    const char *start = s.c_str();
    char *end = nullptr;
    double val = std::strtod(start, &end);
    while (*end == ' ' || *end == '\t') ++end;
    if (end == start || *end != '\0') return {0.0, false};
    return {val, true};
}

// Parse an integer at a specific position in a string, advancing pos.
// Returns the parsed integer, or -1 on failure.
int parse_int_at(const std::string &s, size_t &pos, int max_digits) {
    if (pos >= s.size()) return -1;
    int result = 0;
    int digits = 0;
    while (pos < s.size() && digits < max_digits && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        result = result * 10 + (s[pos] - '0');
        ++pos;
        ++digits;
    }
    return digits > 0 ? result : -1;
}

// Expect a specific character at pos, advancing pos.
bool expect_char(const std::string &s, size_t &pos, char c) {
    if (pos >= s.size() || s[pos] != c) return false;
    ++pos;
    return true;
}

// Trim leading/trailing whitespace.
std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

} // anonymous namespace

// ─── Date/time parsing ───

Value parse_date(const std::string &s) {
    std::string input = trim(s);
    if (input.size() < 8) return Value::make_null(); // minimum: "Y-MM-DD" won't work but "YYYY-M-D" might

    size_t pos = 0;

    // Parse year (up to 4 digits, but could be negative)
    bool neg_year = false;
    if (pos < input.size() && input[pos] == '-') {
        neg_year = true;
        ++pos;
    }
    int year = parse_int_at(input, pos, 4);
    if (year < 0) return Value::make_null();
    if (neg_year) year = -year;

    if (!expect_char(input, pos, '-')) return Value::make_null();

    int month = parse_int_at(input, pos, 2);
    if (month < 1 || month > 12) return Value::make_null();

    if (!expect_char(input, pos, '-')) return Value::make_null();

    int day = parse_int_at(input, pos, 2);
    if (day < 1 || day > 31) return Value::make_null();

    // Must have consumed the entire string
    if (pos != input.size()) return Value::make_null();

    return Value::make_date(year, month, day);
}

Value parse_time(const std::string &s) {
    std::string input = trim(s);
    if (input.size() < 5) return Value::make_null(); // minimum: "H:MM" won't work, need "HH:MM:SS"

    size_t pos = 0;

    int hour = parse_int_at(input, pos, 2);
    if (hour < 0 || hour > 23) return Value::make_null();

    if (!expect_char(input, pos, ':')) return Value::make_null();

    int minute = parse_int_at(input, pos, 2);
    if (minute < 0 || minute > 59) return Value::make_null();

    if (!expect_char(input, pos, ':')) return Value::make_null();

    int second = parse_int_at(input, pos, 2);
    if (second < 0 || second > 59) return Value::make_null();

    if (pos != input.size()) return Value::make_null();

    return Value::make_time(hour, minute, second);
}

Value parse_timestamp(const std::string &s) {
    std::string input = trim(s);
    // Expected: "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS"
    if (input.size() < 19) return Value::make_null();

    size_t pos = 0;

    // Parse year
    bool neg_year = false;
    if (pos < input.size() && input[pos] == '-') {
        neg_year = true;
        ++pos;
    }
    int year = parse_int_at(input, pos, 4);
    if (year < 0) return Value::make_null();
    if (neg_year) year = -year;

    if (!expect_char(input, pos, '-')) return Value::make_null();

    int month = parse_int_at(input, pos, 2);
    if (month < 1 || month > 12) return Value::make_null();

    if (!expect_char(input, pos, '-')) return Value::make_null();

    int day = parse_int_at(input, pos, 2);
    if (day < 1 || day > 31) return Value::make_null();

    // Accept either space or 'T' as separator
    if (pos >= input.size()) return Value::make_null();
    if (input[pos] != ' ' && input[pos] != 'T') return Value::make_null();
    ++pos;

    int hour = parse_int_at(input, pos, 2);
    if (hour < 0 || hour > 23) return Value::make_null();

    if (!expect_char(input, pos, ':')) return Value::make_null();

    int minute = parse_int_at(input, pos, 2);
    if (minute < 0 || minute > 59) return Value::make_null();

    if (!expect_char(input, pos, ':')) return Value::make_null();

    int second = parse_int_at(input, pos, 2);
    if (second < 0 || second > 59) return Value::make_null();

    if (pos != input.size()) return Value::make_null();

    return Value::make_timestamp(year, month, day, hour, minute, second);
}

// ─── Type coercion ───

Value coerce_value(const Value &v, const std::string &target_type) {
    if (v.is_null()) return Value::make_null();

    std::string upper = to_upper(target_type);

    // Target: INT64
    if (upper == "INT" || upper == "INTEGER" || upper == "BIGINT" || upper == "INT64") {
        switch (v.type) {
            case Value::Type::INT64:
                return v;
            case Value::Type::FLOAT64:
                return Value::make_int(static_cast<int64_t>(v.float_val));
            case Value::Type::STRING: {
                auto [val, ok] = try_parse_int64(v.str_val);
                if (ok) return Value::make_int(val);
                // Try parsing as float first, then truncate
                auto [fval, fok] = try_parse_double(v.str_val);
                if (fok) return Value::make_int(static_cast<int64_t>(fval));
                return Value::make_null();
            }
            case Value::Type::BOOL:
                return Value::make_int(v.bool_val ? 1 : 0);
            case Value::Type::DATE_VAL:
                // Return the internal representation (days since epoch)
                return Value::make_int(v.int_val);
            case Value::Type::TIME_VAL:
                // Return microseconds since midnight
                return Value::make_int(v.int_val);
            case Value::Type::TIMESTAMP_VAL:
                // Return microseconds since epoch
                return Value::make_int(v.int_val);
            default:
                return Value::make_null();
        }
    }

    // Target: FLOAT64
    if (upper == "FLOAT" || upper == "DOUBLE" || upper == "REAL" || upper == "FLOAT64") {
        switch (v.type) {
            case Value::Type::FLOAT64:
                return v;
            case Value::Type::INT64:
                return Value::make_float(static_cast<double>(v.int_val));
            case Value::Type::STRING: {
                auto [val, ok] = try_parse_double(v.str_val);
                if (ok) return Value::make_float(val);
                return Value::make_null();
            }
            case Value::Type::BOOL:
                return Value::make_float(v.bool_val ? 1.0 : 0.0);
            default:
                return Value::make_null();
        }
    }

    // Target: STRING
    if (upper == "TEXT" || upper == "VARCHAR" || upper == "STRING" || upper == "CHAR") {
        return Value::make_string(v.to_string());
    }

    // Target: BOOL
    if (upper == "BOOL" || upper == "BOOLEAN") {
        switch (v.type) {
            case Value::Type::BOOL:
                return v;
            case Value::Type::INT64:
                return Value::make_bool(v.int_val != 0);
            case Value::Type::FLOAT64:
                return Value::make_bool(v.float_val != 0.0);
            case Value::Type::STRING: {
                std::string us = to_upper(trim(v.str_val));
                if (us == "TRUE" || us == "1" || us == "YES" || us == "ON") {
                    return Value::make_bool(true);
                }
                if (us == "FALSE" || us == "0" || us == "NO" || us == "OFF") {
                    return Value::make_bool(false);
                }
                return Value::make_null();
            }
            default:
                return Value::make_null();
        }
    }

    // Target: DATE
    if (upper == "DATE") {
        switch (v.type) {
            case Value::Type::DATE_VAL:
                return v;
            case Value::Type::STRING:
                return parse_date(v.str_val);
            case Value::Type::TIMESTAMP_VAL: {
                // Extract date part from timestamp
                int y = v.date_year();
                int m = v.date_month();
                int d = v.date_day();
                return Value::make_date(y, m, d);
            }
            default:
                return Value::make_null();
        }
    }

    // Target: TIME
    if (upper == "TIME") {
        switch (v.type) {
            case Value::Type::TIME_VAL:
                return v;
            case Value::Type::STRING:
                return parse_time(v.str_val);
            case Value::Type::TIMESTAMP_VAL: {
                // Extract time part from timestamp
                int h = v.time_hour();
                int mi = v.time_minute();
                int s = v.time_second();
                return Value::make_time(h, mi, s);
            }
            default:
                return Value::make_null();
        }
    }

    // Target: TIMESTAMP
    if (upper == "TIMESTAMP" || upper == "DATETIME") {
        switch (v.type) {
            case Value::Type::TIMESTAMP_VAL:
                return v;
            case Value::Type::STRING:
                return parse_timestamp(v.str_val);
            case Value::Type::DATE_VAL: {
                // Promote date to timestamp at midnight
                int y = v.date_year();
                int m = v.date_month();
                int d = v.date_day();
                return Value::make_timestamp(y, m, d, 0, 0, 0);
            }
            default:
                return Value::make_null();
        }
    }

    // Unknown target type
    return Value::make_null();
}

// ─── Type compatibility ───

bool is_type_compatible(Value::Type from, const std::string &target_type) {
    std::string upper = to_upper(target_type);

    // NULL is compatible with everything
    if (from == Value::Type::NULL_VAL) return true;

    // INT targets
    if (upper == "INT" || upper == "INTEGER" || upper == "BIGINT" || upper == "INT64") {
        return from == Value::Type::INT64 ||
               from == Value::Type::FLOAT64 ||
               from == Value::Type::BOOL ||
               from == Value::Type::STRING ||
               from == Value::Type::DATE_VAL ||
               from == Value::Type::TIME_VAL ||
               from == Value::Type::TIMESTAMP_VAL;
    }

    // FLOAT targets
    if (upper == "FLOAT" || upper == "DOUBLE" || upper == "REAL" || upper == "FLOAT64") {
        return from == Value::Type::INT64 ||
               from == Value::Type::FLOAT64 ||
               from == Value::Type::BOOL ||
               from == Value::Type::STRING;
    }

    // STRING targets
    if (upper == "TEXT" || upper == "VARCHAR" || upper == "STRING" || upper == "CHAR") {
        // Everything can be converted to string
        return true;
    }

    // BOOL targets
    if (upper == "BOOL" || upper == "BOOLEAN") {
        return from == Value::Type::BOOL ||
               from == Value::Type::INT64 ||
               from == Value::Type::FLOAT64 ||
               from == Value::Type::STRING;
    }

    // DATE targets
    if (upper == "DATE") {
        return from == Value::Type::DATE_VAL ||
               from == Value::Type::STRING ||
               from == Value::Type::TIMESTAMP_VAL;
    }

    // TIME targets
    if (upper == "TIME") {
        return from == Value::Type::TIME_VAL ||
               from == Value::Type::STRING ||
               from == Value::Type::TIMESTAMP_VAL;
    }

    // TIMESTAMP targets
    if (upper == "TIMESTAMP" || upper == "DATETIME") {
        return from == Value::Type::TIMESTAMP_VAL ||
               from == Value::Type::STRING ||
               from == Value::Type::DATE_VAL;
    }

    return false;
}

// ─── Date arithmetic ───

Value date_add(const Value &date, const Value &interval) {
    if (date.type != Value::Type::DATE_VAL) return Value::make_null();
    if (interval.type != Value::Type::INT64) return Value::make_null();

    Value result;
    result.type = Value::Type::DATE_VAL;
    result.int_val = date.int_val + interval.int_val;
    return result;
}

Value date_diff(const Value &date1, const Value &date2) {
    if (date1.type != Value::Type::DATE_VAL) return Value::make_null();
    if (date2.type != Value::Type::DATE_VAL) return Value::make_null();

    return Value::make_int(date1.int_val - date2.int_val);
}

} // namespace tdb::sql
