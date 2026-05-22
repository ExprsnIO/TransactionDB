#include "tdb/sql/executor.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace tdb::sql {

// ─── Date/time internal helpers (file-scope anonymous namespace) ───
namespace {

// Convert (year, month, day) to days since epoch 2000-01-01.
// 2000-01-01 is day 0.  Uses Howard Hinnant's days_from_civil algorithm.
int64_t ymd_to_days(int year, int month, int day) {
    int y = year;
    int m = month;
    if (m <= 2) { y -= 1; m += 12; }
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m - 3) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t era_days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return era_days - 10957;  // offset from 1970-01-01 epoch to 2000-01-01
}

// Convert days since epoch (2000-01-01) back to (year, month, day).
void days_to_ymd(int64_t days, int &year, int &month, int &day) {
    int64_t z = days + 10957 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    year  = static_cast<int>(y + (m <= 2 ? 1 : 0));
    month = static_cast<int>(m);
    day   = static_cast<int>(d);
}

// Convert (hour, minute, second) to microseconds since midnight.
int64_t hms_to_micros(int hour, int minute, int second) {
    return (static_cast<int64_t>(hour) * 3600 +
            static_cast<int64_t>(minute) * 60 +
            static_cast<int64_t>(second)) * 1000000LL;
}

// Convert microseconds since midnight to (hour, minute, second).
void micros_to_hms(int64_t micros, int &hour, int &minute, int &second) {
    int64_t total_seconds = micros / 1000000LL;
    hour   = static_cast<int>(total_seconds / 3600);
    minute = static_cast<int>((total_seconds % 3600) / 60);
    second = static_cast<int>(total_seconds % 60);
}

constexpr int64_t kMicrosPerDay = 86400LL * 1000000LL;

} // anonymous date/time helpers

// ─── Value date/time factory methods ───

Value Value::make_date(int year, int month, int day) {
    Value val;
    val.type = Type::DATE_VAL;
    val.int_val = ymd_to_days(year, month, day);
    return val;
}

Value Value::make_time(int hour, int minute, int second) {
    Value val;
    val.type = Type::TIME_VAL;
    val.int_val = hms_to_micros(hour, minute, second);
    return val;
}

Value Value::make_timestamp(int year, int month, int day,
                            int hour, int minute, int second) {
    Value val;
    val.type = Type::TIMESTAMP_VAL;
    val.int_val = ymd_to_days(year, month, day) * kMicrosPerDay +
                  hms_to_micros(hour, minute, second);
    return val;
}

// ─── Value date component extraction ───

int Value::date_year() const {
    int y = 0, m = 0, d = 0;
    if (type == Type::DATE_VAL) {
        days_to_ymd(int_val, y, m, d);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t day_part = int_val / kMicrosPerDay;
        if (int_val < 0 && (int_val % kMicrosPerDay) != 0) day_part -= 1;
        days_to_ymd(day_part, y, m, d);
    }
    return y;
}

int Value::date_month() const {
    int y = 0, m = 0, d = 0;
    if (type == Type::DATE_VAL) {
        days_to_ymd(int_val, y, m, d);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t day_part = int_val / kMicrosPerDay;
        if (int_val < 0 && (int_val % kMicrosPerDay) != 0) day_part -= 1;
        days_to_ymd(day_part, y, m, d);
    }
    return m;
}

int Value::date_day() const {
    int y = 0, m = 0, d = 0;
    if (type == Type::DATE_VAL) {
        days_to_ymd(int_val, y, m, d);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t day_part = int_val / kMicrosPerDay;
        if (int_val < 0 && (int_val % kMicrosPerDay) != 0) day_part -= 1;
        days_to_ymd(day_part, y, m, d);
    }
    return d;
}

int Value::time_hour() const {
    int h = 0, m = 0, s = 0;
    if (type == Type::TIME_VAL) {
        micros_to_hms(int_val, h, m, s);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t time_part = int_val % kMicrosPerDay;
        if (time_part < 0) time_part += kMicrosPerDay;
        micros_to_hms(time_part, h, m, s);
    }
    return h;
}

int Value::time_minute() const {
    int h = 0, m = 0, s = 0;
    if (type == Type::TIME_VAL) {
        micros_to_hms(int_val, h, m, s);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t time_part = int_val % kMicrosPerDay;
        if (time_part < 0) time_part += kMicrosPerDay;
        micros_to_hms(time_part, h, m, s);
    }
    return m;
}

int Value::time_second() const {
    int h = 0, m = 0, s = 0;
    if (type == Type::TIME_VAL) {
        micros_to_hms(int_val, h, m, s);
    } else if (type == Type::TIMESTAMP_VAL) {
        int64_t time_part = int_val % kMicrosPerDay;
        if (time_part < 0) time_part += kMicrosPerDay;
        micros_to_hms(time_part, h, m, s);
    }
    return s;
}

// ─── Value::compare ───

int Value::compare(const Value &other) const {
    if (is_null() && other.is_null()) return 0;
    if (is_null()) return -1;
    if (other.is_null()) return 1;

    // Same-type comparisons
    if (type == other.type) {
        switch (type) {
            case Type::INT64:
                return (int_val < other.int_val) ? -1 : (int_val > other.int_val ? 1 : 0);
            case Type::FLOAT64:
                return (float_val < other.float_val) ? -1 : (float_val > other.float_val ? 1 : 0);
            case Type::STRING:
                return (str_val < other.str_val) ? -1 : (str_val > other.str_val ? 1 : 0);
            case Type::BOOL:
                return (bool_val == other.bool_val) ? 0 : (bool_val ? 1 : -1);
            case Type::DATE_VAL:
            case Type::TIME_VAL:
            case Type::TIMESTAMP_VAL:
                return (int_val < other.int_val) ? -1 : (int_val > other.int_val ? 1 : 0);
            case Type::DECIMAL: {
                // Compare by numeric value, not lexicographically.
                double a = std::stod(str_val);
                double b = std::stod(other.str_val);
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
            case Type::UUID:
            case Type::JSON_VAL:
            case Type::XML_VAL:
            case Type::VARBINARY:
            case Type::BIT_VAL:
                return (str_val < other.str_val) ? -1 : (str_val > other.str_val ? 1 : 0);
            case Type::INTERVAL: {
                // Normalize to total microseconds assuming 30-day months.
                // (Calendar interval comparison is locale-dependent; this is
                // SQL standard behavior with the simplifying assumption.)
                int64_t a = int_val * 30LL * 24 * 3600 * 1000000LL + int_val_2;
                int64_t b = other.int_val * 30LL * 24 * 3600 * 1000000LL + other.int_val_2;
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
            case Type::ENUM_VAL:
                return (int_val < other.int_val) ? -1 : (int_val > other.int_val ? 1 : 0);
            case Type::TIMESTAMP_TZ:
                // Compare in UTC.
                return (int_val < other.int_val) ? -1 : (int_val > other.int_val ? 1 : 0);
            case Type::GEOMETRY:
                return (str_val < other.str_val) ? -1 : (str_val > other.str_val ? 1 : 0);
            case Type::ARRAY: {
                if (!composite_fields || !other.composite_fields) return 0;
                size_t n = std::min(composite_fields->size(), other.composite_fields->size());
                for (size_t i = 0; i < n; i++) {
                    int c = (*composite_fields)[i].compare((*other.composite_fields)[i]);
                    if (c != 0) return c;
                }
                if (composite_fields->size() < other.composite_fields->size()) return -1;
                if (composite_fields->size() > other.composite_fields->size()) return 1;
                return 0;
            }
            case Type::MULTISET: {
                // Order-insensitive: compare sorted string representations.
                if (!composite_fields || !other.composite_fields) return 0;
                std::vector<std::string> a, b;
                for (auto &e : *composite_fields) a.push_back(e.to_string());
                for (auto &e : *other.composite_fields) b.push_back(e.to_string());
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                if (a < b) return -1;
                if (a > b) return 1;
                return 0;
            }
            case Type::COMPOSITE: {
                // Lexicographic over fields.
                if (!composite_fields || !other.composite_fields) return 0;
                size_t n = std::min(composite_fields->size(), other.composite_fields->size());
                for (size_t i = 0; i < n; i++) {
                    int c = (*composite_fields)[i].compare((*other.composite_fields)[i]);
                    if (c != 0) return c;
                }
                if (composite_fields->size() < other.composite_fields->size()) return -1;
                if (composite_fields->size() > other.composite_fields->size()) return 1;
                return 0;
            }
            default:
                return 0;
        }
    }

    // Cross-type numeric comparisons
    if ((type == Type::INT64 || type == Type::FLOAT64) &&
        (other.type == Type::INT64 || other.type == Type::FLOAT64)) {
        double a = (type == Type::INT64) ? static_cast<double>(int_val) : float_val;
        double b = (other.type == Type::INT64) ? static_cast<double>(other.int_val) : other.float_val;
        return (a < b) ? -1 : (a > b ? 1 : 0);
    }

    // Cross-type date/timestamp: promote DATE to TIMESTAMP for comparison
    if ((type == Type::DATE_VAL && other.type == Type::TIMESTAMP_VAL) ||
        (type == Type::TIMESTAMP_VAL && other.type == Type::DATE_VAL)) {
        int64_t a_ts = (type == Type::DATE_VAL) ? int_val * kMicrosPerDay : int_val;
        int64_t b_ts = (other.type == Type::DATE_VAL) ? other.int_val * kMicrosPerDay : other.int_val;
        return (a_ts < b_ts) ? -1 : (a_ts > b_ts ? 1 : 0);
    }

    // Fall back to string comparison for other mixed types
    const auto sa = to_string();
    const auto sb = other.to_string();
    return (sa < sb) ? -1 : (sa > sb ? 1 : 0);
}

// ─── Value::to_string ───

std::string Value::to_string() const {
    switch (type) {
        case Type::NULL_VAL: return "NULL";
        case Type::INT64: return std::to_string(int_val);
        case Type::FLOAT64: {
            std::ostringstream oss;
            oss << float_val;
            return oss.str();
        }
        case Type::STRING: return str_val;
        case Type::BOOL: return bool_val ? "TRUE" : "FALSE";
        case Type::BLOB: return "<BLOB>";
        case Type::DATE_VAL: {
            int y, m, d;
            days_to_ymd(int_val, y, m, d);
            std::ostringstream oss;
            if (y < 0) {
                oss << '-' << std::setfill('0') << std::setw(4) << (-y);
            } else {
                oss << std::setfill('0') << std::setw(4) << y;
            }
            oss << '-' << std::setfill('0') << std::setw(2) << m
                << '-' << std::setfill('0') << std::setw(2) << d;
            return oss.str();
        }
        case Type::TIME_VAL: {
            int h, mi, s;
            micros_to_hms(int_val, h, mi, s);
            std::ostringstream oss;
            oss << std::setfill('0') << std::setw(2) << h
                << ':' << std::setfill('0') << std::setw(2) << mi
                << ':' << std::setfill('0') << std::setw(2) << s;
            return oss.str();
        }
        case Type::TIMESTAMP_VAL: {
            int64_t day_part  = int_val / kMicrosPerDay;
            int64_t time_part = int_val % kMicrosPerDay;
            if (time_part < 0) { day_part -= 1; time_part += kMicrosPerDay; }
            int y, mo, d;
            days_to_ymd(day_part, y, mo, d);
            int h, mi, s;
            micros_to_hms(time_part, h, mi, s);
            std::ostringstream oss;
            if (y < 0) {
                oss << '-' << std::setfill('0') << std::setw(4) << (-y);
            } else {
                oss << std::setfill('0') << std::setw(4) << y;
            }
            oss << '-' << std::setfill('0') << std::setw(2) << mo
                << '-' << std::setfill('0') << std::setw(2) << d
                << ' ' << std::setfill('0') << std::setw(2) << h
                << ':' << std::setfill('0') << std::setw(2) << mi
                << ':' << std::setfill('0') << std::setw(2) << s;
            return oss.str();
        }
        case Type::DECIMAL:
            return str_val;
        case Type::UUID:
            return str_val;
        case Type::VARBINARY: {
            static const char *hex = "0123456789abcdef";
            std::string out;
            out.reserve(str_val.size() * 2 + 2);
            out += "\\x";
            for (unsigned char b : str_val) {
                out.push_back(hex[(b >> 4) & 0xF]);
                out.push_back(hex[b & 0xF]);
            }
            return out;
        }
        case Type::INTERVAL: {
            int64_t months = int_val;
            int64_t micros = int_val_2;
            bool neg = months < 0 || (months == 0 && micros < 0);
            if (neg) { months = -months; micros = -micros; }
            int64_t years = months / 12;  months %= 12;
            int64_t days  = micros / (24LL * 3600LL * 1000000LL); micros %= (24LL * 3600LL * 1000000LL);
            int64_t hours = micros / (3600LL * 1000000LL); micros %= (3600LL * 1000000LL);
            int64_t mins  = micros / (60LL * 1000000LL); micros %= (60LL * 1000000LL);
            int64_t secs  = micros / 1000000LL; micros %= 1000000LL;
            std::ostringstream oss;
            if (neg) oss << '-';
            oss << 'P';
            if (years) oss << years << 'Y';
            if (months) oss << months << 'M';
            if (days)   oss << days   << 'D';
            if (hours || mins || secs || micros) {
                oss << 'T';
                if (hours) oss << hours << 'H';
                if (mins)  oss << mins  << 'M';
                if (secs || micros) {
                    oss << secs;
                    if (micros) oss << '.' << std::setfill('0') << std::setw(6) << micros;
                    oss << 'S';
                }
            }
            std::string s = oss.str();
            if (s == "P" || s == "-P") return "PT0S";
            return s;
        }
        case Type::ENUM_VAL: {
            std::ostringstream oss;
            oss << str_val << '#' << int_val;
            return oss.str();
        }
        case Type::BIT_VAL:
            return "B'" + str_val + "'";
        case Type::JSON_VAL:
            return str_val;
        case Type::XML_VAL:
            return str_val;
        case Type::COMPOSITE: {
            std::ostringstream oss;
            oss << '(';
            if (composite_fields) {
                bool first = true;
                for (auto &f : *composite_fields) {
                    if (!first) oss << ',';
                    if (f.type == Type::STRING || f.type == Type::JSON_VAL || f.type == Type::XML_VAL)
                        oss << '"' << f.to_string() << '"';
                    else
                        oss << f.to_string();
                    first = false;
                }
            }
            oss << ')';
            return oss.str();
        }
        case Type::TIMESTAMP_TZ: {
            int64_t local = int_val + int_val_2 * 60LL * 1000000LL;
            int64_t day_part = local / kMicrosPerDay;
            int64_t time_part = local % kMicrosPerDay;
            if (time_part < 0) { day_part -= 1; time_part += kMicrosPerDay; }
            int y, mo, d; days_to_ymd(day_part, y, mo, d);
            int h, mi, s; micros_to_hms(time_part, h, mi, s);
            std::ostringstream oss;
            oss << std::setfill('0') << std::setw(4) << y
                << '-' << std::setfill('0') << std::setw(2) << mo
                << '-' << std::setfill('0') << std::setw(2) << d
                << ' ' << std::setfill('0') << std::setw(2) << h
                << ':' << std::setfill('0') << std::setw(2) << mi
                << ':' << std::setfill('0') << std::setw(2) << s;
            int off = (int)int_val_2;
            char sign = off >= 0 ? '+' : '-';
            int absoff = off >= 0 ? off : -off;
            oss << sign << std::setfill('0') << std::setw(2) << (absoff / 60)
                << ':' << std::setfill('0') << std::setw(2) << (absoff % 60);
            return oss.str();
        }
        case Type::GEOMETRY:
            return str_val; // canonical WKT
        case Type::ARRAY: {
            std::ostringstream oss;
            oss << '[';
            if (composite_fields) {
                bool first = true;
                for (auto &e : *composite_fields) {
                    if (!first) oss << ',';
                    if (e.type == Type::STRING) oss << '"' << e.to_string() << '"';
                    else oss << e.to_string();
                    first = false;
                }
            }
            oss << ']';
            return oss.str();
        }
        case Type::MULTISET: {
            std::ostringstream oss;
            oss << "MULTISET[";
            if (composite_fields) {
                bool first = true;
                for (auto &e : *composite_fields) {
                    if (!first) oss << ',';
                    if (e.type == Type::STRING) oss << '"' << e.to_string() << '"';
                    else oss << e.to_string();
                    first = false;
                }
            }
            oss << ']';
            return oss.str();
        }
    }
    return "?";
}

// ─── SeqScan ───
SeqScan::SeqScan(std::string table_name, Schema schema, std::vector<Tuple> *data)
    : table_name_(std::move(table_name)), schema_(std::move(schema)),
      data_(data), cursor_(0) {}

void SeqScan::open() { cursor_ = 0; }

bool SeqScan::next(Tuple &out) {
    if (!data_ || cursor_ >= data_->size()) return false;
    out = (*data_)[cursor_++];
    return true;
}

void SeqScan::close() { cursor_ = 0; }

// ─── Filter ───
Filter::Filter(PlanNodePtr child, Predicate pred)
    : child_(std::move(child)), pred_(std::move(pred)) {}

void Filter::open() { child_->open(); }

bool Filter::next(Tuple &out) {
    while (child_->next(out)) {
        if (pred_(out)) return true;
    }
    return false;
}

void Filter::close() { child_->close(); }

// ─── Projection ───
Projection::Projection(PlanNodePtr child, std::vector<int> col_indices, Schema out_schema)
    : child_(std::move(child)), col_indices_(std::move(col_indices)),
      out_schema_(std::move(out_schema)) {}

void Projection::open() { child_->open(); }

bool Projection::next(Tuple &out) {
    Tuple full;
    if (!child_->next(full)) return false;
    out.clear();
    out.reserve(col_indices_.size());
    for (int idx : col_indices_) {
        if (idx >= 0 && idx < (int)full.size()) {
            out.push_back(full[idx]);
        } else {
            out.push_back(Value::make_null());
        }
    }
    return true;
}

void Projection::close() { child_->close(); }

// ─── Nested Loop Join ───
NestedLoopJoin::NestedLoopJoin(PlanNodePtr left, PlanNodePtr right,
                               JoinPredicate pred, Schema out_schema)
    : left_(std::move(left)), right_(std::move(right)),
      pred_(std::move(pred)), out_schema_(std::move(out_schema)),
      left_exhausted_(true) {}

void NestedLoopJoin::open() {
    left_->open();
    left_exhausted_ = !left_->next(left_tuple_);
    if (!left_exhausted_) right_->open();
}

bool NestedLoopJoin::next(Tuple &out) {
    while (!left_exhausted_) {
        Tuple right_tuple;
        while (right_->next(right_tuple)) {
            if (pred_(left_tuple_, right_tuple)) {
                out = left_tuple_;
                out.insert(out.end(), right_tuple.begin(), right_tuple.end());
                return true;
            }
        }
        /* Right side exhausted, advance left */
        right_->close();
        left_exhausted_ = !left_->next(left_tuple_);
        if (!left_exhausted_) right_->open();
    }
    return false;
}

void NestedLoopJoin::close() {
    left_->close();
    right_->close();
}

// ─── Sort ───
Sort::Sort(PlanNodePtr child, Comparator cmp)
    : child_(std::move(child)), cmp_(std::move(cmp)), cursor_(0) {}

void Sort::open() {
    child_->open();
    sorted_.clear();
    Tuple t;
    while (child_->next(t)) {
        sorted_.push_back(std::move(t));
    }
    child_->close();
    std::sort(sorted_.begin(), sorted_.end(), cmp_);
    cursor_ = 0;
}

bool Sort::next(Tuple &out) {
    if (cursor_ >= sorted_.size()) return false;
    out = sorted_[cursor_++];
    return true;
}

void Sort::close() { sorted_.clear(); cursor_ = 0; }

// ─── Limit ───
Limit::Limit(PlanNodePtr child, size_t limit, size_t offset)
    : child_(std::move(child)), limit_(limit), offset_(offset),
      emitted_(0), skipped_(0) {}

void Limit::open() {
    child_->open();
    emitted_ = 0;
    skipped_ = 0;
}

bool Limit::next(Tuple &out) {
    while (skipped_ < offset_) {
        if (!child_->next(out)) return false;
        skipped_++;
    }
    if (emitted_ >= limit_) return false;
    if (!child_->next(out)) return false;
    emitted_++;
    return true;
}

void Limit::close() { child_->close(); }

// ─── Hash Aggregate ───
HashAggregate::HashAggregate(PlanNodePtr child, std::vector<int> group_cols,
                             std::vector<AggOp> agg_ops, Schema out_schema)
    : child_(std::move(child)), group_cols_(std::move(group_cols)),
      agg_ops_(std::move(agg_ops)), out_schema_(std::move(out_schema)),
      cursor_(0) {}

void HashAggregate::open() {
    child_->open();
    results_.clear();

    // Build groups using string key
    struct AggState {
        int64_t count = 0;
        double sum = 0.0;
        Value min_val;
        Value max_val;
        bool has_min = false;
        bool has_max = false;
    };

    std::unordered_map<std::string, std::pair<Tuple, std::vector<AggState>>> groups;

    Tuple t;
    while (child_->next(t)) {
        // Build group key
        std::string gkey;
        Tuple group_vals;
        for (int ci : group_cols_) {
            if (ci >= 0 && ci < (int)t.size()) {
                gkey += t[ci].to_string() + "|";
                group_vals.push_back(t[ci]);
            }
        }

        auto &[gvals, states] = groups[gkey];
        if (states.empty()) {
            gvals = group_vals;
            states.resize(agg_ops_.size());
        }

        for (size_t a = 0; a < agg_ops_.size(); a++) {
            auto &op = agg_ops_[a];
            auto &st = states[a];
            st.count++;
            if (op.col_index >= 0 && op.col_index < (int)t.size()) {
                Value &v = t[op.col_index];
                if (!v.is_null()) {
                    if (v.type == Value::Type::INT64) st.sum += (double)v.int_val;
                    else if (v.type == Value::Type::FLOAT64) st.sum += v.float_val;

                    if (!st.has_min || v.to_string() < st.min_val.to_string()) {
                        st.min_val = v; st.has_min = true;
                    }
                    if (!st.has_max || v.to_string() > st.max_val.to_string()) {
                        st.max_val = v; st.has_max = true;
                    }
                }
            }
        }
    }
    child_->close();

    // Build result tuples
    // Special case: no groups but we have aggregates (e.g., SELECT COUNT(*) FROM empty)
    if (groups.empty() && !agg_ops_.empty() && group_cols_.empty()) {
        Tuple row;
        for (auto &op : agg_ops_) {
            (void)op;
            row.push_back(Value::make_int(0));
        }
        results_.push_back(std::move(row));
    } else {
        for (auto &[key, pair] : groups) {
            auto &[gvals, states] = pair;
            Tuple row = gvals;
            for (size_t a = 0; a < agg_ops_.size(); a++) {
                auto &op = agg_ops_[a];
                auto &st = states[a];
                switch (op.type) {
                    case AggOp::Type::COUNT:
                    case AggOp::Type::COUNT_STAR:
                        row.push_back(Value::make_int(st.count));
                        break;
                    case AggOp::Type::SUM:
                        row.push_back(Value::make_float(st.sum));
                        break;
                    case AggOp::Type::AVG:
                        row.push_back(st.count > 0 ? Value::make_float(st.sum / st.count)
                                                    : Value::make_null());
                        break;
                    case AggOp::Type::MIN:
                        row.push_back(st.has_min ? st.min_val : Value::make_null());
                        break;
                    case AggOp::Type::MAX:
                        row.push_back(st.has_max ? st.max_val : Value::make_null());
                        break;
                }
            }
            results_.push_back(std::move(row));
        }
    }

    cursor_ = 0;
}

bool HashAggregate::next(Tuple &out) {
    if (cursor_ >= results_.size()) return false;
    out = results_[cursor_++];
    return true;
}

void HashAggregate::close() { results_.clear(); cursor_ = 0; }

// ─── Insert ───
InsertExec::InsertExec(std::string table_name, std::vector<Tuple> *table_data,
                       std::vector<Tuple> rows)
    : table_name_(std::move(table_name)), table_data_(table_data),
      rows_(std::move(rows)), done_(false) {}

void InsertExec::open() { done_ = false; }

bool InsertExec::next(Tuple &out) {
    if (done_) return false;
    int64_t count = 0;
    for (auto &row : rows_) {
        table_data_->push_back(std::move(row));
        count++;
    }
    out = { Value::make_int(count) };
    done_ = true;
    return true;
}

void InsertExec::close() {}
Schema InsertExec::schema() const { return {{"rows_affected", ""}}; }

// ─── Update ───
UpdateExec::UpdateExec(std::string table_name, std::vector<Tuple> *table_data,
                       std::function<bool(const Tuple &)> predicate, UpdateFn update_fn)
    : table_name_(std::move(table_name)), table_data_(table_data),
      predicate_(std::move(predicate)), update_fn_(std::move(update_fn)), done_(false) {}

void UpdateExec::open() { done_ = false; }

bool UpdateExec::next(Tuple &out) {
    if (done_) return false;
    int64_t count = 0;
    for (auto &row : *table_data_) {
        if (predicate_(row)) {
            update_fn_(row);
            count++;
        }
    }
    out = { Value::make_int(count) };
    done_ = true;
    return true;
}

void UpdateExec::close() {}
Schema UpdateExec::schema() const { return {{"rows_affected", ""}}; }

// ─── Delete ───
DeleteExec::DeleteExec(std::string table_name, std::vector<Tuple> *table_data,
                       std::function<bool(const Tuple &)> predicate)
    : table_name_(std::move(table_name)), table_data_(table_data),
      predicate_(std::move(predicate)), done_(false) {}

void DeleteExec::open() { done_ = false; }

bool DeleteExec::next(Tuple &out) {
    if (done_) return false;
    size_t before = table_data_->size();
    table_data_->erase(
        std::remove_if(table_data_->begin(), table_data_->end(), predicate_),
        table_data_->end());
    int64_t count = (int64_t)(before - table_data_->size());
    out = { Value::make_int(count) };
    done_ = true;
    return true;
}

void DeleteExec::close() {}
Schema DeleteExec::schema() const { return {{"rows_affected", ""}}; }

// ─── Window Executor ───

namespace {

// Extract a numeric value from a Value for aggregation purposes.
double value_to_double(const Value &v) {
    switch (v.type) {
        case Value::Type::INT64: return static_cast<double>(v.int_val);
        case Value::Type::FLOAT64: return v.float_val;
        case Value::Type::BOOL: return v.bool_val ? 1.0 : 0.0;
        default: return 0.0;
    }
}

// Compare two Value objects. Returns <0 if a<b, 0 if a==b, >0 if a>b.
// NULLs sort first (smallest).  Delegates to Value::compare.
int value_compare(const Value &a, const Value &b) {
    return a.compare(b);
}

// Compare two tuples by the given list of (column_index, ascending) pairs.
int tuple_order_compare(const Tuple &a, const Tuple &b,
                        const std::vector<std::pair<int, bool>> &order_cols) {
    static const Value null_val = Value::make_null();
    for (const auto &[col, asc] : order_cols) {
        if (col < 0) continue;
        const Value &va = (col < static_cast<int>(a.size())) ? a[static_cast<size_t>(col)] : null_val;
        const Value &vb = (col < static_cast<int>(b.size())) ? b[static_cast<size_t>(col)] : null_val;
        int cmp = value_compare(va, vb);
        if (cmp != 0) return asc ? cmp : -cmp;
    }
    return 0;
}

// Build a string key from selected partition columns of a tuple.
std::string partition_key(const Tuple &t, const std::vector<int> &cols) {
    std::string key;
    for (int c : cols) {
        if (c >= 0 && c < static_cast<int>(t.size())) {
            key += t[static_cast<size_t>(c)].to_string();
        }
        key += '\x1f'; // unit separator
    }
    return key;
}

// Check if two tuples have the same ORDER BY values (used for RANK).
bool same_order_values(const Tuple &a, const Tuple &b,
                       const std::vector<std::pair<int, bool>> &order_cols) {
    static const Value null_val = Value::make_null();
    for (const auto &[col, asc_unused] : order_cols) {
        (void)asc_unused;
        if (col < 0) continue;
        const Value &va = (col < static_cast<int>(a.size())) ? a[static_cast<size_t>(col)] : null_val;
        const Value &vb = (col < static_cast<int>(b.size())) ? b[static_cast<size_t>(col)] : null_val;
        if (value_compare(va, vb) != 0) return false;
    }
    return true;
}

// Resolve frame boundaries to absolute row indices within a partition.
// partition_size: number of rows in the partition
// current_row: 0-based index of the current row within the partition
// frame_start/frame_end: spec values from WindowFuncSpec
// Returns [start_idx, end_idx] inclusive, clamped to valid range.
std::pair<size_t, size_t> resolve_frame(size_t partition_size, size_t current_row,
                                         int frame_start, int frame_end) {
    // Compute start
    size_t start = 0;
    if (frame_start == -1) {
        // UNBOUNDED PRECEDING
        start = 0;
    } else if (frame_start == 0) {
        // CURRENT ROW
        start = current_row;
    } else {
        // n PRECEDING
        if (current_row >= static_cast<size_t>(frame_start)) {
            start = current_row - static_cast<size_t>(frame_start);
        } else {
            start = 0;
        }
    }

    // Compute end
    size_t end = 0;
    if (frame_end == -1) {
        // UNBOUNDED FOLLOWING
        end = partition_size - 1;
    } else if (frame_end == 0) {
        // CURRENT ROW
        end = current_row;
    } else {
        // n FOLLOWING
        end = current_row + static_cast<size_t>(frame_end);
        if (end >= partition_size) end = partition_size - 1;
    }

    // Clamp
    if (start >= partition_size) start = partition_size > 0 ? partition_size - 1 : 0;
    if (end >= partition_size) end = partition_size > 0 ? partition_size - 1 : 0;
    if (start > end && partition_size > 0) {
        // Empty frame - return an inverted range that the caller checks
        return {1, 0};
    }
    return {start, end};
}

// Convert a Value to a string key for hash join lookups.
// Returns a pair: (key_string, is_null). If is_null is true, the key should
// never match (SQL NULL != NULL semantics).
std::pair<std::string, bool> value_to_hash_key(const Value &v) {
    if (v.is_null()) return {"", true};
    // Prefix with type tag to avoid collisions between types
    switch (v.type) {
        case Value::Type::INT64:
            return {"I:" + std::to_string(v.int_val), false};
        case Value::Type::FLOAT64: {
            std::ostringstream oss;
            oss << "F:" << v.float_val;
            return {oss.str(), false};
        }
        case Value::Type::STRING:
            return {"S:" + v.str_val, false};
        case Value::Type::BOOL:
            return {v.bool_val ? "B:1" : "B:0", false};
        case Value::Type::DATE_VAL:
            return {"D:" + std::to_string(v.int_val), false};
        case Value::Type::TIME_VAL:
            return {"TM:" + std::to_string(v.int_val), false};
        case Value::Type::TIMESTAMP_VAL:
            return {"TS:" + std::to_string(v.int_val), false};
        case Value::Type::NULL_VAL:
        case Value::Type::BLOB:
            return {"", true};
        // Extended types — hash by their canonical string form. Composite uses
        // a recursive prefix so nested values don't collide.
        case Value::Type::DECIMAL:    return {"DEC:" + v.str_val, false};
        case Value::Type::UUID:       return {"UUID:" + v.str_val, false};
        case Value::Type::VARBINARY:  return {"VB:" + v.str_val, false};
        case Value::Type::INTERVAL:
            return {"IV:" + std::to_string(v.int_val) + "/" + std::to_string(v.int_val_2), false};
        case Value::Type::ENUM_VAL:
            return {"EN:" + v.str_val + "#" + std::to_string(v.int_val), false};
        case Value::Type::BIT_VAL:    return {"BIT:" + v.str_val, false};
        case Value::Type::JSON_VAL:   return {"J:" + v.str_val, false};
        case Value::Type::XML_VAL:    return {"X:" + v.str_val, false};
        case Value::Type::TIMESTAMP_TZ:
            return {"TSZ:" + std::to_string(v.int_val) + "@" + std::to_string(v.int_val_2), false};
        case Value::Type::COMPOSITE:  return {"C:" + v.to_string(), false};
        case Value::Type::GEOMETRY:   return {"G:" + v.str_val, false};
        case Value::Type::ARRAY:      return {"A:" + v.to_string(), false};
        case Value::Type::MULTISET:   return {"MS:" + v.to_string(), false};
    }
    return {"", true};
}

} // anonymous namespace

WindowExec::WindowExec(PlanNodePtr child, std::vector<WindowFuncSpec> specs, Schema out_schema)
    : child_(std::move(child)), specs_(std::move(specs)),
      out_schema_(std::move(out_schema)), cursor_(0) {}

void WindowExec::open() {
    child_->open();
    results_.clear();

    // Step 1: Consume all rows from child.
    std::vector<Tuple> all_rows;
    {
        Tuple t;
        while (child_->next(t)) {
            all_rows.push_back(std::move(t));
        }
    }
    child_->close();

    if (all_rows.empty()) {
        cursor_ = 0;
        return;
    }

    // We need to maintain original row indices so we can stitch results back.
    // For each WindowFuncSpec, we compute a Value per original row.
    const size_t num_rows = all_rows.size();
    const size_t num_specs = specs_.size();

    // window_results[row_index][spec_index] = computed Value
    std::vector<std::vector<Value>> window_results(num_rows, std::vector<Value>(num_specs));

    for (size_t si = 0; si < num_specs; si++) {
        const auto &spec = specs_[si];

        // Step 2: Partition rows. We keep indices into all_rows.
        // Build partition groups: key -> list of row indices
        // We use a vector to maintain insertion order of partition keys.
        std::vector<std::pair<std::string, std::vector<size_t>>> partitions;
        std::unordered_map<std::string, size_t> partition_index_map;

        for (size_t ri = 0; ri < num_rows; ri++) {
            std::string pkey = partition_key(all_rows[ri], spec.partition_cols);
            auto it = partition_index_map.find(pkey);
            if (it == partition_index_map.end()) {
                partition_index_map[pkey] = partitions.size();
                partitions.emplace_back(pkey, std::vector<size_t>{ri});
            } else {
                partitions[it->second].second.push_back(ri);
            }
        }

        // Step 3: Sort within each partition by order_cols.
        for (auto &[pkey_unused, indices] : partitions) {
            (void)pkey_unused;
            if (!spec.order_cols.empty()) {
                std::stable_sort(indices.begin(), indices.end(),
                    [&](size_t a, size_t b) {
                        return tuple_order_compare(all_rows[a], all_rows[b],
                                                   spec.order_cols) < 0;
                    });
            }
        }

        // Step 4: Compute window function for each partition.
        for (auto &[pkey_unused2, indices] : partitions) {
            (void)pkey_unused2;
            const size_t psize = indices.size();

            // Precompute values for the argument column within partition order
            // for aggregate window functions.
            std::vector<Value> arg_vals(psize);
            if (spec.arg_col_index >= 0) {
                for (size_t i = 0; i < psize; i++) {
                    const auto &row = all_rows[indices[i]];
                    if (spec.arg_col_index < static_cast<int>(row.size())) {
                        arg_vals[i] = row[static_cast<size_t>(spec.arg_col_index)];
                    }
                    // else remains NULL (default)
                }
            }

            for (size_t pos = 0; pos < psize; pos++) {
                const size_t row_idx = indices[pos];
                Value result = Value::make_null();

                switch (spec.func) {
                    case WindowFuncSpec::Func::ROW_NUMBER: {
                        result = Value::make_int(static_cast<int64_t>(pos + 1));
                        break;
                    }

                    case WindowFuncSpec::Func::RANK: {
                        // RANK: 1 for first row; for subsequent rows, if ORDER BY
                        // values are the same as previous row, same rank; otherwise
                        // rank = position + 1 (1-based, counting ties).
                        if (pos == 0) {
                            result = Value::make_int(1);
                        } else {
                            if (same_order_values(all_rows[indices[pos]],
                                                  all_rows[indices[pos - 1]],
                                                  spec.order_cols)) {
                                // Same rank as previous
                                result = window_results[indices[pos - 1]][si];
                            } else {
                                result = Value::make_int(static_cast<int64_t>(pos + 1));
                            }
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::DENSE_RANK: {
                        if (pos == 0) {
                            result = Value::make_int(1);
                        } else {
                            if (same_order_values(all_rows[indices[pos]],
                                                  all_rows[indices[pos - 1]],
                                                  spec.order_cols)) {
                                result = window_results[indices[pos - 1]][si];
                            } else {
                                // Previous dense_rank + 1
                                int64_t prev = window_results[indices[pos - 1]][si].int_val;
                                result = Value::make_int(prev + 1);
                            }
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::NTILE: {
                        int buckets = spec.ntile_buckets;
                        if (buckets <= 0) buckets = 1;
                        // NTILE distributes rows as evenly as possible.
                        // Bucket assignment (1-based):
                        size_t rows_per_bucket = psize / static_cast<size_t>(buckets);
                        size_t remainder = psize % static_cast<size_t>(buckets);
                        // First 'remainder' buckets get (rows_per_bucket+1) rows each,
                        // the rest get rows_per_bucket rows each.
                        int64_t bucket = 0;
                        size_t cumulative = 0;
                        for (int b = 0; b < buckets; b++) {
                            size_t bucket_size = rows_per_bucket +
                                (static_cast<size_t>(b) < remainder ? 1 : 0);
                            cumulative += bucket_size;
                            if (pos < cumulative) {
                                bucket = static_cast<int64_t>(b + 1);
                                break;
                            }
                        }
                        result = Value::make_int(bucket);
                        break;
                    }

                    case WindowFuncSpec::Func::LAG: {
                        int offset = spec.lag_lead_offset;
                        if (offset < 0) offset = 0;
                        if (pos >= static_cast<size_t>(offset)) {
                            size_t target = pos - static_cast<size_t>(offset);
                            if (spec.arg_col_index >= 0) {
                                result = arg_vals[target];
                            } else {
                                result = Value::make_null();
                            }
                        } else {
                            result = spec.lag_lead_default;
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::LEAD: {
                        int offset = spec.lag_lead_offset;
                        if (offset < 0) offset = 0;
                        size_t target = pos + static_cast<size_t>(offset);
                        if (target < psize) {
                            if (spec.arg_col_index >= 0) {
                                result = arg_vals[target];
                            } else {
                                result = Value::make_null();
                            }
                        } else {
                            result = spec.lag_lead_default;
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::FIRST_VALUE: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        if (fs <= fe && fs < psize) {
                            result = arg_vals[fs];
                        } else {
                            result = Value::make_null();
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::LAST_VALUE: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        if (fs <= fe && fe < psize) {
                            result = arg_vals[fe];
                        } else {
                            result = Value::make_null();
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::NTH_VALUE: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        int n = spec.nth_value_n; // 1-based
                        if (n <= 0) {
                            result = Value::make_null();
                        } else if (fs <= fe) {
                            size_t target = fs + static_cast<size_t>(n - 1);
                            if (target <= fe && target < psize) {
                                result = arg_vals[target];
                            } else {
                                result = Value::make_null();
                            }
                        } else {
                            result = Value::make_null();
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::PERCENT_RANK: {
                        // (rank - 1) / (partition_size - 1), or 0 if partition has 1 row
                        if (psize <= 1) {
                            result = Value::make_float(0.0);
                        } else {
                            // Compute rank for current row
                            int64_t rank = 1;
                            if (pos > 0) {
                                // Walk backwards to find rank
                                // rank = first position in this peer group + 1
                                size_t first_peer = pos;
                                while (first_peer > 0 &&
                                       same_order_values(all_rows[indices[first_peer]],
                                                         all_rows[indices[first_peer - 1]],
                                                         spec.order_cols)) {
                                    first_peer--;
                                }
                                rank = static_cast<int64_t>(first_peer + 1);
                            }
                            double pr = static_cast<double>(rank - 1) /
                                        static_cast<double>(psize - 1);
                            result = Value::make_float(pr);
                        }
                        break;
                    }

                    case WindowFuncSpec::Func::CUME_DIST: {
                        // CUME_DIST = (number of rows with value <= current) / partition_size
                        // More precisely: number of peer rows or preceding rows / psize.
                        // Find the last row in the peer group.
                        size_t last_peer = pos;
                        while (last_peer + 1 < psize &&
                               same_order_values(all_rows[indices[last_peer + 1]],
                                                 all_rows[indices[pos]],
                                                 spec.order_cols)) {
                            last_peer++;
                        }
                        double cd = static_cast<double>(last_peer + 1) /
                                    static_cast<double>(psize);
                        result = Value::make_float(cd);
                        break;
                    }

                    case WindowFuncSpec::Func::SUM: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        double sum = 0.0;
                        bool has_value = false;
                        if (fs <= fe) {
                            for (size_t i = fs; i <= fe && i < psize; i++) {
                                if (!arg_vals[i].is_null()) {
                                    sum += value_to_double(arg_vals[i]);
                                    has_value = true;
                                }
                            }
                        }
                        result = has_value ? Value::make_float(sum) : Value::make_null();
                        break;
                    }

                    case WindowFuncSpec::Func::COUNT: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        int64_t count = 0;
                        if (fs <= fe) {
                            for (size_t i = fs; i <= fe && i < psize; i++) {
                                if (spec.arg_col_index < 0 || !arg_vals[i].is_null()) {
                                    count++;
                                }
                            }
                        }
                        result = Value::make_int(count);
                        break;
                    }

                    case WindowFuncSpec::Func::AVG: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        double sum = 0.0;
                        int64_t count = 0;
                        if (fs <= fe) {
                            for (size_t i = fs; i <= fe && i < psize; i++) {
                                if (!arg_vals[i].is_null()) {
                                    sum += value_to_double(arg_vals[i]);
                                    count++;
                                }
                            }
                        }
                        result = (count > 0) ? Value::make_float(sum / static_cast<double>(count))
                                             : Value::make_null();
                        break;
                    }

                    case WindowFuncSpec::Func::MIN: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        Value min_val;
                        bool has_min = false;
                        if (fs <= fe) {
                            for (size_t i = fs; i <= fe && i < psize; i++) {
                                if (!arg_vals[i].is_null()) {
                                    if (!has_min || value_compare(arg_vals[i], min_val) < 0) {
                                        min_val = arg_vals[i];
                                        has_min = true;
                                    }
                                }
                            }
                        }
                        result = has_min ? min_val : Value::make_null();
                        break;
                    }

                    case WindowFuncSpec::Func::MAX: {
                        auto [fs, fe] = resolve_frame(psize, pos,
                                                      spec.frame_start, spec.frame_end);
                        Value max_val;
                        bool has_max = false;
                        if (fs <= fe) {
                            for (size_t i = fs; i <= fe && i < psize; i++) {
                                if (!arg_vals[i].is_null()) {
                                    if (!has_max || value_compare(arg_vals[i], max_val) > 0) {
                                        max_val = arg_vals[i];
                                        has_max = true;
                                    }
                                }
                            }
                        }
                        result = has_max ? max_val : Value::make_null();
                        break;
                    }
                } // switch

                window_results[row_idx][si] = result;
            } // for each row in partition
        } // for each partition
    } // for each spec

    // Step 5: Build final result rows = original row + window function values appended.
    results_.reserve(num_rows);
    for (size_t ri = 0; ri < num_rows; ri++) {
        Tuple out_row = all_rows[ri];
        for (size_t si = 0; si < num_specs; si++) {
            out_row.push_back(window_results[ri][si]);
        }
        results_.push_back(std::move(out_row));
    }

    cursor_ = 0;
}

bool WindowExec::next(Tuple &out) {
    if (cursor_ >= results_.size()) return false;
    out = results_[cursor_++];
    return true;
}

void WindowExec::close() {
    results_.clear();
    cursor_ = 0;
}

// ─── Hash Join ───
HashJoin::HashJoin(PlanNodePtr left, PlanNodePtr right,
                   int left_key_col, int right_key_col, Schema out_schema)
    : left_(std::move(left)), right_(std::move(right)),
      left_key_col_(left_key_col), right_key_col_(right_key_col),
      out_schema_(std::move(out_schema)),
      left_exhausted_(true), match_cursor_(0), match_list_(nullptr) {}

void HashJoin::open() {
    // Build phase: consume all rows from right side and build hash table
    hash_buckets_.clear();
    hash_index_.clear();
    match_list_ = nullptr;
    match_cursor_ = 0;

    right_->open();
    Tuple rt;
    while (right_->next(rt)) {
        // Extract key from right tuple
        Value key_val;
        if (right_key_col_ >= 0 &&
            right_key_col_ < static_cast<int>(rt.size())) {
            key_val = rt[static_cast<size_t>(right_key_col_)];
        }
        // NULL keys never match, so skip them
        auto [key_str, is_null] = value_to_hash_key(key_val);
        if (is_null) continue;

        auto it = hash_index_.find(key_str);
        if (it == hash_index_.end()) {
            hash_index_[key_str] = hash_buckets_.size();
            hash_buckets_.emplace_back(key_str, std::vector<Tuple>{std::move(rt)});
        } else {
            hash_buckets_[it->second].second.push_back(std::move(rt));
        }
    }
    right_->close();

    // Probe phase: start iterating left side
    left_->open();
    left_exhausted_ = false;
    // We need to pull the first left tuple and find its matches
    // But we do this lazily in next()
    left_exhausted_ = !left_->next(left_tuple_);
    match_list_ = nullptr;
    match_cursor_ = 0;
}

bool HashJoin::next(Tuple &out) {
    for (;;) {
        // If we have a current match list and there are remaining matches, emit
        if (match_list_ != nullptr && match_cursor_ < match_list_->size()) {
            out = left_tuple_;
            out.insert(out.end(),
                       (*match_list_)[match_cursor_].begin(),
                       (*match_list_)[match_cursor_].end());
            ++match_cursor_;
            return true;
        }

        // Advance to next left tuple
        if (left_exhausted_) return false;

        // Try to get next left row (we may already have one loaded if
        // the previous match list was empty)
        bool need_advance = (match_list_ != nullptr || match_cursor_ > 0);
        if (need_advance) {
            left_exhausted_ = !left_->next(left_tuple_);
            if (left_exhausted_) return false;
        }

        // Probe hash table with left key
        Value left_key;
        if (left_key_col_ >= 0 &&
            left_key_col_ < static_cast<int>(left_tuple_.size())) {
            left_key = left_tuple_[static_cast<size_t>(left_key_col_)];
        }
        auto [key_str, is_null] = value_to_hash_key(left_key);
        match_list_ = nullptr;
        match_cursor_ = 0;

        if (!is_null) {
            auto it = hash_index_.find(key_str);
            if (it != hash_index_.end()) {
                match_list_ = &hash_buckets_[it->second].second;
                match_cursor_ = 0;
            }
        }
        // If no matches found (null key or no hash hit), loop will advance
        // to next left tuple since match_list_ is null or empty
        if (match_list_ == nullptr) {
            // Mark that we need to advance on next iteration
            match_list_ = nullptr;
            match_cursor_ = 1; // nonzero so need_advance triggers
            continue;
        }
    }
}

void HashJoin::close() {
    left_->close();
    // right_ was already closed in open() after building hash table
    hash_buckets_.clear();
    hash_index_.clear();
    match_list_ = nullptr;
    match_cursor_ = 0;
}

// ─── Merge Join ───
MergeJoin::MergeJoin(PlanNodePtr left, PlanNodePtr right,
                     int left_key_col, int right_key_col, Schema out_schema)
    : left_(std::move(left)), right_(std::move(right)),
      left_key_col_(left_key_col), right_key_col_(right_key_col),
      out_schema_(std::move(out_schema)),
      left_valid_(false), right_valid_(false),
      right_group_cursor_(0), in_group_(false) {}

void MergeJoin::open() {
    left_->open();
    right_->open();
    left_valid_ = left_->next(left_tuple_);
    right_valid_ = right_->next(right_tuple_);
    right_group_.clear();
    right_group_cursor_ = 0;
    in_group_ = false;
}

bool MergeJoin::next(Tuple &out) {
    for (;;) {
        // If we're replaying a buffered right group, emit the next match
        if (in_group_ && right_group_cursor_ < right_group_.size()) {
            out = left_tuple_;
            out.insert(out.end(),
                       right_group_[right_group_cursor_].begin(),
                       right_group_[right_group_cursor_].end());
            ++right_group_cursor_;
            return true;
        }

        // If we just finished replaying a group, advance left and see if
        // the next left row also matches the same group key
        if (in_group_) {
            left_valid_ = left_->next(left_tuple_);
            if (!left_valid_) return false;

            // Check if new left key matches the group key
            Value left_key;
            if (left_key_col_ >= 0 &&
                left_key_col_ < static_cast<int>(left_tuple_.size())) {
                left_key = left_tuple_[static_cast<size_t>(left_key_col_)];
            }
            if (!left_key.is_null() && !right_group_.empty()) {
                Value group_key;
                if (right_key_col_ >= 0 &&
                    right_key_col_ < static_cast<int>(right_group_[0].size())) {
                    group_key = right_group_[0][static_cast<size_t>(right_key_col_)];
                }
                if (!group_key.is_null() && value_compare(left_key, group_key) == 0) {
                    // Replay the same right group for this new left row
                    right_group_cursor_ = 0;
                    continue;
                }
            }
            // Left key no longer matches the group; exit group mode
            in_group_ = false;
        }

        // Standard merge advance
        if (!left_valid_ || !right_valid_) return false;

        // Extract keys
        Value left_key;
        if (left_key_col_ >= 0 &&
            left_key_col_ < static_cast<int>(left_tuple_.size())) {
            left_key = left_tuple_[static_cast<size_t>(left_key_col_)];
        }
        Value right_key;
        if (right_key_col_ >= 0 &&
            right_key_col_ < static_cast<int>(right_tuple_.size())) {
            right_key = right_tuple_[static_cast<size_t>(right_key_col_)];
        }

        // Handle NULLs: NULL never matches, advance whichever side has NULL
        if (left_key.is_null()) {
            left_valid_ = left_->next(left_tuple_);
            continue;
        }
        if (right_key.is_null()) {
            right_valid_ = right_->next(right_tuple_);
            continue;
        }

        int cmp = value_compare(left_key, right_key);
        if (cmp < 0) {
            // Left key < right key: advance left
            left_valid_ = left_->next(left_tuple_);
        } else if (cmp > 0) {
            // Left key > right key: advance right
            right_valid_ = right_->next(right_tuple_);
        } else {
            // Keys match! Buffer all right rows with this key value
            right_group_.clear();
            right_group_.push_back(right_tuple_);

            // Read ahead to collect all right rows with the same key
            while (true) {
                Tuple next_right;
                bool have_next = right_->next(next_right);
                if (!have_next) {
                    right_valid_ = false;
                    break;
                }
                Value next_key;
                if (right_key_col_ >= 0 &&
                    right_key_col_ < static_cast<int>(next_right.size())) {
                    next_key = next_right[static_cast<size_t>(right_key_col_)];
                }
                if (next_key.is_null() || value_compare(right_key, next_key) != 0) {
                    // Different key or null: save this tuple as the new
                    // right cursor position and stop buffering
                    right_tuple_ = std::move(next_right);
                    right_valid_ = true;
                    break;
                }
                right_group_.push_back(std::move(next_right));
            }

            in_group_ = true;
            right_group_cursor_ = 0;
            // Loop back to emit from the group
        }
    }
}

void MergeJoin::close() {
    left_->close();
    right_->close();
    right_group_.clear();
    right_group_cursor_ = 0;
    in_group_ = false;
}

} // namespace tdb::sql
