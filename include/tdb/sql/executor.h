#ifndef TDB_SQL_EXECUTOR_H
#define TDB_SQL_EXECUTOR_H

#include "tdb/sql/ast.h"
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <functional>
#include <cstdint>
#include <unordered_map>

namespace tdb::sql {

// ─── Tuple: a row of values ───
struct Value {
    enum class Type {
        NULL_VAL, INT64, FLOAT64, STRING, BOOL, BLOB,
        DATE_VAL,       // days since epoch (2000-01-01), stored in int_val
        TIME_VAL,       // microseconds since midnight, stored in int_val
        TIMESTAMP_VAL,  // microseconds since epoch (2000-01-01 00:00:00), stored in int_val
        // ─── Extended types added in Batch 5 ─────────────────────────────
        DECIMAL,        // canonical numeric string in str_val, scale in int_val
        UUID,           // 36-char canonical form "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" in str_val
        VARBINARY,      // raw bytes in str_val, distinguished from BLOB only by intent
        INTERVAL,       // months in int_val, microseconds in int_val_2
        ENUM_VAL,       // ordinal in int_val, type name in str_val (label resolved via catalog)
        BIT_VAL,        // packed bits stored as '0'/'1' string in str_val, bit count = str_val.size()
        JSON_VAL,       // JSON text in str_val
        XML_VAL,        // XML text in str_val
        COMPOSITE,      // user-defined composite/record; fields in `composite_fields`, type name in str_val
        TIMESTAMP_TZ,   // microseconds since UTC epoch in int_val, offset minutes in int_val_2
        GEOMETRY,       // canonical WKT/EWKT in str_val; SRID in int_val; dim (2 or 3) in int_val_2
        ARRAY,          // ordered, possibly-heterogeneous; elements in composite_fields
        MULTISET,       // unordered with multiplicity; elements in composite_fields (order not significant)
    };
    Type type = Type::NULL_VAL;
    int64_t int_val = 0;
    int64_t int_val_2 = 0;    // secondary integer (interval µs, TZ offset minutes, decimal scale, ...)
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;
    // Composite / record types hold their field values here. Heap-allocated only
    // when actually used, so non-composite Values stay cheap.
    std::shared_ptr<std::vector<Value>> composite_fields;

    bool is_null() const { return type == Type::NULL_VAL; }

    static Value make_null() { return Value{}; }
    static Value make_int(int64_t v) { Value val; val.type = Type::INT64; val.int_val = v; return val; }
    static Value make_float(double v) { Value val; val.type = Type::FLOAT64; val.float_val = v; return val; }
    static Value make_string(std::string v) { Value val; val.type = Type::STRING; val.str_val = std::move(v); return val; }
    static Value make_bool(bool v) { Value val; val.type = Type::BOOL; val.bool_val = v; return val; }

    // ─── New factories (Batch 5) ─────────────────────────────────────
    static Value make_decimal(std::string canonical, int scale) {
        Value v; v.type = Type::DECIMAL; v.str_val = std::move(canonical); v.int_val = scale; return v;
    }
    static Value make_uuid(std::string canonical) {
        Value v; v.type = Type::UUID; v.str_val = std::move(canonical); return v;
    }
    static Value make_varbinary(std::string bytes) {
        Value v; v.type = Type::VARBINARY; v.str_val = std::move(bytes); return v;
    }
    static Value make_blob(std::string bytes) {
        Value v; v.type = Type::BLOB; v.str_val = std::move(bytes); return v;
    }
    static Value make_interval(int64_t months, int64_t microseconds) {
        Value v; v.type = Type::INTERVAL; v.int_val = months; v.int_val_2 = microseconds; return v;
    }
    static Value make_enum(std::string type_name, int64_t ordinal) {
        Value v; v.type = Type::ENUM_VAL; v.str_val = std::move(type_name); v.int_val = ordinal; return v;
    }
    static Value make_bit(std::string bits) {
        Value v; v.type = Type::BIT_VAL; v.str_val = std::move(bits); return v;
    }
    static Value make_json(std::string text) {
        Value v; v.type = Type::JSON_VAL; v.str_val = std::move(text); return v;
    }
    static Value make_xml(std::string text) {
        Value v; v.type = Type::XML_VAL; v.str_val = std::move(text); return v;
    }
    static Value make_composite(std::string type_name, std::vector<Value> fields) {
        Value v; v.type = Type::COMPOSITE; v.str_val = std::move(type_name);
        v.composite_fields = std::make_shared<std::vector<Value>>(std::move(fields));
        return v;
    }
    static Value make_timestamp_tz(int64_t utc_micros, int offset_minutes) {
        Value v; v.type = Type::TIMESTAMP_TZ; v.int_val = utc_micros; v.int_val_2 = offset_minutes; return v;
    }
    static Value make_geometry(std::string wkt, int srid = 0, int dim = 2) {
        Value v; v.type = Type::GEOMETRY; v.str_val = std::move(wkt);
        v.int_val = srid; v.int_val_2 = dim; return v;
    }
    static Value make_array(std::vector<Value> elements) {
        Value v; v.type = Type::ARRAY;
        v.composite_fields = std::make_shared<std::vector<Value>>(std::move(elements));
        return v;
    }
    static Value make_multiset(std::vector<Value> elements) {
        Value v; v.type = Type::MULTISET;
        v.composite_fields = std::make_shared<std::vector<Value>>(std::move(elements));
        return v;
    }

    // Date/time factory methods
    static Value make_date(int year, int month, int day);
    static Value make_time(int hour, int minute, int second);
    static Value make_timestamp(int year, int month, int day,
                                int hour, int minute, int second);

    // Date component extraction (valid for DATE_VAL and TIMESTAMP_VAL)
    int date_year() const;
    int date_month() const;
    int date_day() const;

    // Time component extraction (valid for TIME_VAL and TIMESTAMP_VAL)
    int time_hour() const;
    int time_minute() const;
    int time_second() const;

    // Comparison: returns <0 if *this < other, 0 if equal, >0 if *this > other
    int compare(const Value &other) const;

    std::string to_string() const;
};

using Tuple = std::vector<Value>;

// ─── Schema: column names + types for a result set ───
struct Column {
    std::string name;
    std::string table;
};
using Schema = std::vector<Column>;

// ─── Volcano Iterator Interface ───
class PlanNode {
public:
    virtual ~PlanNode() = default;
    virtual void open() = 0;
    virtual bool next(Tuple &out) = 0;
    virtual void close() = 0;
    virtual Schema schema() const = 0;
};

using PlanNodePtr = std::unique_ptr<PlanNode>;

// ─── Sequential Scan ───
// Scans an in-memory table (vector of tuples)
class SeqScan : public PlanNode {
public:
    SeqScan(std::string table_name, Schema schema,
            std::vector<Tuple> *data);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return schema_; }
private:
    std::string table_name_;
    Schema schema_;
    std::vector<Tuple> *data_;
    size_t cursor_;
};

// ─── Filter (WHERE clause) ───
class Filter : public PlanNode {
public:
    using Predicate = std::function<bool(const Tuple &)>;
    Filter(PlanNodePtr child, Predicate pred);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return child_->schema(); }
private:
    PlanNodePtr child_;
    Predicate pred_;
};

// ─── Projection (SELECT list) ───
class Projection : public PlanNode {
public:
    Projection(PlanNodePtr child, std::vector<int> col_indices, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr child_;
    std::vector<int> col_indices_;
    Schema out_schema_;
};

// ─── Nested Loop Join ───
class NestedLoopJoin : public PlanNode {
public:
    using JoinPredicate = std::function<bool(const Tuple &, const Tuple &)>;
    NestedLoopJoin(PlanNodePtr left, PlanNodePtr right, JoinPredicate pred, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr left_;
    PlanNodePtr right_;
    JoinPredicate pred_;
    Schema out_schema_;
    Tuple left_tuple_;
    bool left_exhausted_;
};

// ─── Sort ───
class Sort : public PlanNode {
public:
    using Comparator = std::function<bool(const Tuple &, const Tuple &)>;
    Sort(PlanNodePtr child, Comparator cmp);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return child_->schema(); }
private:
    PlanNodePtr child_;
    Comparator cmp_;
    std::vector<Tuple> sorted_;
    size_t cursor_;
};

// ─── Limit ───
class Limit : public PlanNode {
public:
    Limit(PlanNodePtr child, size_t limit, size_t offset = 0);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return child_->schema(); }
private:
    PlanNodePtr child_;
    size_t limit_;
    size_t offset_;
    size_t emitted_;
    size_t skipped_;
};

// ─── Hash Aggregate ───
class HashAggregate : public PlanNode {
public:
    struct AggOp {
        enum class Type { COUNT, SUM, AVG, MIN, MAX, COUNT_STAR };
        Type type;
        int col_index; // -1 for COUNT(*)
    };

    HashAggregate(PlanNodePtr child, std::vector<int> group_cols,
                  std::vector<AggOp> agg_ops, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr child_;
    std::vector<int> group_cols_;
    std::vector<AggOp> agg_ops_;
    Schema out_schema_;
    std::vector<Tuple> results_;
    size_t cursor_;
};

// ─── Insert Executor ───
class InsertExec : public PlanNode {
public:
    InsertExec(std::string table_name, std::vector<Tuple> *table_data,
               std::vector<Tuple> rows);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override;
private:
    std::string table_name_;
    std::vector<Tuple> *table_data_;
    std::vector<Tuple> rows_;
    bool done_;
};

// ─── Update Executor ───
class UpdateExec : public PlanNode {
public:
    using UpdateFn = std::function<void(Tuple &)>;
    UpdateExec(std::string table_name, std::vector<Tuple> *table_data,
               std::function<bool(const Tuple &)> predicate, UpdateFn update_fn);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override;
private:
    std::string table_name_;
    std::vector<Tuple> *table_data_;
    std::function<bool(const Tuple &)> predicate_;
    UpdateFn update_fn_;
    bool done_;
};

// ─── Delete Executor ───
class DeleteExec : public PlanNode {
public:
    DeleteExec(std::string table_name, std::vector<Tuple> *table_data,
               std::function<bool(const Tuple &)> predicate);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override;
private:
    std::string table_name_;
    std::vector<Tuple> *table_data_;
    std::function<bool(const Tuple &)> predicate_;
    bool done_;
};

// ─── Window Function Specification ───
struct WindowFuncSpec {
    enum class Func {
        ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG, LEAD,
        FIRST_VALUE, LAST_VALUE, NTH_VALUE, PERCENT_RANK, CUME_DIST,
        SUM, COUNT, AVG, MIN, MAX
    };
    Func func;
    int arg_col_index = -1;    // column index for aggregate window funcs
    int ntile_buckets = 1;     // for NTILE
    int lag_lead_offset = 1;   // for LAG/LEAD
    Value lag_lead_default;    // default value for LAG/LEAD
    int nth_value_n = 1;       // for NTH_VALUE (1-based)
    std::vector<int> partition_cols;
    std::vector<std::pair<int, bool>> order_cols; // col_index, ascending
    // Frame specification (simplified ROWS frame)
    // frame_start: -1 = UNBOUNDED PRECEDING, 0 = CURRENT ROW, >0 = n PRECEDING
    // frame_end:    0 = CURRENT ROW, -1 = UNBOUNDED FOLLOWING, >0 = n FOLLOWING
    int frame_start = -1;
    int frame_end = 0;
    std::string result_name;   // output column name
};

// ─── Window Executor ───
class WindowExec : public PlanNode {
public:
    WindowExec(PlanNodePtr child, std::vector<WindowFuncSpec> specs, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr child_;
    std::vector<WindowFuncSpec> specs_;
    Schema out_schema_;
    std::vector<Tuple> results_;
    size_t cursor_;
};

// ─── Hash Join ───
// Build hash table from right side, probe with left side
class HashJoin : public PlanNode {
public:
    HashJoin(PlanNodePtr left, PlanNodePtr right,
             int left_key_col, int right_key_col, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr left_;
    PlanNodePtr right_;
    int left_key_col_;
    int right_key_col_;
    Schema out_schema_;
    // Hash table: key string -> list of matching right-side tuples
    std::vector<std::pair<std::string, std::vector<Tuple>>> hash_buckets_;
    std::unordered_map<std::string, size_t> hash_index_;
    // Probe state
    Tuple left_tuple_;
    bool left_exhausted_;
    size_t match_cursor_;            // index within current match list
    std::vector<Tuple> *match_list_; // pointer into hash_buckets_ entry
};

// ─── Merge Join ───
// Assumes both inputs are pre-sorted on join key
class MergeJoin : public PlanNode {
public:
    MergeJoin(PlanNodePtr left, PlanNodePtr right,
              int left_key_col, int right_key_col, Schema out_schema);
    void open() override;
    bool next(Tuple &out) override;
    void close() override;
    Schema schema() const override { return out_schema_; }
private:
    PlanNodePtr left_;
    PlanNodePtr right_;
    int left_key_col_;
    int right_key_col_;
    Schema out_schema_;
    // Merge state
    Tuple left_tuple_;
    Tuple right_tuple_;
    bool left_valid_;
    bool right_valid_;
    // For handling duplicates: when left key == right key and there are
    // multiple right rows with the same key, we buffer them and replay
    // for each matching left row.
    std::vector<Tuple> right_group_;  // buffered right rows with same key
    size_t right_group_cursor_;       // position within right_group_
    bool in_group_;                   // currently replaying a group
};

// ─── Query Statistics ───
struct QueryStats {
    double planning_time_ms = 0;
    double execution_time_ms = 0;
    int64_t rows_scanned = 0;
    int64_t rows_returned = 0;
    std::string plan_description;
};

// ─── Result set for returning to caller ───
struct ResultSet {
    bool success = false;
    std::string error_message;
    Schema columns;
    std::vector<Tuple> rows;
    int64_t rows_affected = 0;
};

} // namespace tdb::sql

#endif // TDB_SQL_EXECUTOR_H
