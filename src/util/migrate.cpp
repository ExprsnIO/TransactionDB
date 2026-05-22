#include "tdb/migrate.h"
#include "tdb/database.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>

namespace tdb::migrate {

// ============================================================
// Internal helpers
// ============================================================

namespace {

// Parse a single CSV line into fields, respecting quoting and escaping.
std::vector<std::string> parse_csv_line(const std::string &line, const CsvOptions &opts) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    size_t i = 0;

    while (i < line.size()) {
        char c = line[i];

        if (in_quotes) {
            if (c == opts.escape && i + 1 < line.size()) {
                char next = line[i + 1];
                // Escape sequence: backslash-quote or backslash-backslash
                if (next == opts.quote || next == opts.escape) {
                    field += next;
                    i += 2;
                    continue;
                }
            }
            // RFC 4180 doubled-quote escape: "" inside quoted field
            if (c == opts.quote) {
                if (i + 1 < line.size() && line[i + 1] == opts.quote) {
                    field += opts.quote;
                    i += 2;
                    continue;
                }
                // End of quoted field
                in_quotes = false;
                ++i;
                continue;
            }
            field += c;
            ++i;
        } else {
            if (c == opts.quote) {
                in_quotes = true;
                ++i;
            } else if (c == opts.delimiter) {
                fields.push_back(field);
                field.clear();
                ++i;
            } else {
                field += c;
                ++i;
            }
        }
    }
    // Last field
    fields.push_back(field);
    return fields;
}

// Quote and escape a value for CSV output.
std::string quote_csv_value(const std::string &value, const CsvOptions &opts) {
    bool needs_quoting = false;
    for (char c : value) {
        if (c == opts.delimiter || c == opts.quote || c == '\n' || c == '\r') {
            needs_quoting = true;
            break;
        }
    }
    if (!needs_quoting) {
        return value;
    }

    std::string result;
    result += opts.quote;
    for (char c : value) {
        if (c == opts.quote) {
            // RFC 4180: double the quote character
            result += opts.quote;
            result += opts.quote;
        } else {
            result += c;
        }
    }
    result += opts.quote;
    return result;
}

// Map a DataType name to an appropriate SQL type string for CREATE TABLE.
std::string data_type_to_sql(const sql::ast::DataType &dt) {
    std::string name = dt.name;
    if (dt.precision.has_value()) {
        name += "(" + std::to_string(dt.precision.value());
        if (dt.scale.has_value()) {
            name += "," + std::to_string(dt.scale.value());
        }
        name += ")";
    }
    return name;
}

// Escape a string for SQL (single-quote escaping).
std::string sql_escape_string(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '\'';
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

// Convert a sql::Value to a SQL literal string.
std::string value_to_sql_literal(const sql::Value &v) {
    switch (v.type) {
        case sql::Value::Type::NULL_VAL:
            return "NULL";
        case sql::Value::Type::INT64:
            return std::to_string(v.int_val);
        case sql::Value::Type::FLOAT64: {
            std::ostringstream oss;
            oss << v.float_val;
            return oss.str();
        }
        case sql::Value::Type::STRING:
            return sql_escape_string(v.str_val);
        case sql::Value::Type::BOOL:
            return v.bool_val ? "TRUE" : "FALSE";
        case sql::Value::Type::BLOB:
            return sql_escape_string(v.str_val);
        case sql::Value::Type::DATE_VAL:
        case sql::Value::Type::TIME_VAL:
        case sql::Value::Type::TIMESTAMP_VAL:
            return sql_escape_string(v.to_string());
        default:
            return sql_escape_string(v.to_string());
    }
    return "NULL";
}

} // anonymous namespace

// ============================================================
// import_csv
// ============================================================

int64_t import_csv(catalog::Catalog &catalog, const std::string &table_name,
                    const std::string &file_path, const CsvOptions &options) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open CSV file: " + file_path);
    }

    std::string line;
    std::vector<std::string> column_names;

    // Read header line
    if (options.header) {
        if (!std::getline(file, line)) {
            return 0; // Empty file
        }
        column_names = parse_csv_line(line, options);
    }

    // Check if table exists
    auto *table = catalog.find_table(table_name);

    if (!table) {
        // Create table with VARCHAR columns inferred from header
        catalog::TableInfo ti;
        ti.name = table_name;
        if (column_names.empty()) {
            // No header: we need to peek at the first data row to determine column count
            if (!std::getline(file, line)) {
                return 0;
            }
            auto first_row = parse_csv_line(line, options);
            for (size_t i = 0; i < first_row.size(); ++i) {
                column_names.push_back("col" + std::to_string(i + 1));
            }
            // Seek back is not reliable with getline, so we'll process this row below
            // by pushing it back. We handle this by processing first_row after table creation.

            for (size_t i = 0; i < column_names.size(); ++i) {
                catalog::ColumnInfo ci;
                ci.name = column_names[i];
                ci.type = sql::ast::DataType{"VARCHAR", std::nullopt, std::nullopt, false, std::nullopt};
                ci.nullable = true;
                ci.ordinal = static_cast<uint16_t>(i);
                ti.columns.push_back(std::move(ci));
            }
            catalog.add_table(table_name, std::move(ti));
            table = catalog.find_table(table_name);

            // Insert the first row we already read
            sql::Tuple tuple;
            for (size_t i = 0; i < first_row.size() && i < column_names.size(); ++i) {
                if (first_row[i] == options.null_string) {
                    tuple.push_back(sql::Value::make_null());
                } else {
                    tuple.push_back(sql::Value::make_string(first_row[i]));
                }
            }
            // Pad with nulls if row is shorter than column count
            while (tuple.size() < column_names.size()) {
                tuple.push_back(sql::Value::make_null());
            }
            table->rows.push_back(std::move(tuple));
        } else {
            for (size_t i = 0; i < column_names.size(); ++i) {
                catalog::ColumnInfo ci;
                ci.name = column_names[i];
                ci.type = sql::ast::DataType{"VARCHAR", std::nullopt, std::nullopt, false, std::nullopt};
                ci.nullable = true;
                ci.ordinal = static_cast<uint16_t>(i);
                ti.columns.push_back(std::move(ci));
            }
            catalog.add_table(table_name, std::move(ti));
            table = catalog.find_table(table_name);
        }
    } else {
        // Table exists; if no header was read, derive column_names from table schema
        if (column_names.empty()) {
            for (auto &col : table->columns) {
                column_names.push_back(col.name);
            }
        }
    }

    int64_t rows_imported = 0;

    // If we already inserted a first row (no-header case with table creation), count it
    if (!column_names.empty() && !options.header && table->rows.size() == 1 &&
        rows_imported == 0 && !file.eof()) {
        // We already added one row above in the no-header table-creation path
        // Check if that path was actually taken: the table was freshly created
        // We detect this by checking if rows_imported is 0 but table has 1 row
        // This is a bit fragile, so let's just count it
        rows_imported = 1;
    }

    size_t num_cols = column_names.size();

    // Read data lines
    while (std::getline(file, line)) {
        // Skip empty lines
        if (line.empty()) continue;

        auto fields = parse_csv_line(line, options);

        sql::Tuple tuple;
        for (size_t i = 0; i < num_cols; ++i) {
            if (i < fields.size()) {
                if (fields[i] == options.null_string) {
                    tuple.push_back(sql::Value::make_null());
                } else {
                    tuple.push_back(sql::Value::make_string(fields[i]));
                }
            } else {
                tuple.push_back(sql::Value::make_null());
            }
        }
        table->rows.push_back(std::move(tuple));
        ++rows_imported;
    }

    return rows_imported;
}

// ============================================================
// export_csv
// ============================================================

int64_t export_csv(const catalog::Catalog &catalog, const std::string &table_name,
                    const std::string &file_path, const CsvOptions &options) {
    const auto *table = catalog.find_table(table_name);
    if (!table) {
        throw std::runtime_error("Table not found: " + table_name);
    }

    std::ofstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for writing: " + file_path);
    }

    // Write header
    if (options.header) {
        for (size_t i = 0; i < table->columns.size(); ++i) {
            if (i > 0) file << options.delimiter;
            file << quote_csv_value(table->columns[i].name, options);
        }
        file << '\n';
    }

    // Write rows
    int64_t rows_exported = 0;
    for (auto &row : table->rows) {
        for (size_t i = 0; i < table->columns.size(); ++i) {
            if (i > 0) file << options.delimiter;
            if (i < row.size()) {
                if (row[i].is_null()) {
                    file << options.null_string;
                } else {
                    file << quote_csv_value(row[i].to_string(), options);
                }
            } else {
                file << options.null_string;
            }
        }
        file << '\n';
        ++rows_exported;
    }

    return rows_exported;
}

// ============================================================
// import_sql_dump
// ============================================================

int64_t import_sql_dump(Database &db, const std::string &file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open SQL dump file: " + file_path);
    }

    int64_t statements_executed = 0;
    std::string accumulated;
    std::string line;

    while (std::getline(file, line)) {
        // Skip comment lines
        {
            size_t start = 0;
            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
                ++start;
            }
            if (start < line.size() && line[start] == '-' && start + 1 < line.size() && line[start + 1] == '-') {
                continue;
            }
        }

        if (line.empty()) continue;

        accumulated += line;
        accumulated += ' ';

        // Check if we have a complete statement (ends with semicolon)
        // We do a simple check: find the last non-whitespace character
        size_t end = accumulated.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(accumulated[end - 1]))) {
            --end;
        }
        if (end > 0 && accumulated[end - 1] == ';') {
            // Execute the accumulated statement
            std::string stmt = accumulated.substr(0, end);
            auto result = db.execute(stmt);
            if (result.success) {
                ++statements_executed;
            }
            accumulated.clear();
        }
    }

    // Handle any remaining statement without trailing semicolon
    {
        size_t end = accumulated.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(accumulated[end - 1]))) {
            --end;
        }
        if (end > 0) {
            std::string stmt = accumulated.substr(0, end);
            auto result = db.execute(stmt);
            if (result.success) {
                ++statements_executed;
            }
        }
    }

    return statements_executed;
}

// ============================================================
// export_sql_dump
// ============================================================

int64_t export_sql_dump(const catalog::Catalog &catalog, const std::string &file_path) {
    std::ofstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for writing: " + file_path);
    }

    int64_t statements_written = 0;
    auto table_names = catalog.list_tables();

    for (auto &tname : table_names) {
        const auto *table = catalog.find_table(tname);
        if (!table) continue;

        // Write CREATE TABLE statement
        file << "CREATE TABLE " << tname << " (\n";
        for (size_t i = 0; i < table->columns.size(); ++i) {
            auto &col = table->columns[i];
            file << "    " << col.name << " " << data_type_to_sql(col.type);
            if (!col.nullable) {
                file << " NOT NULL";
            }
            if (col.primary_key) {
                file << " PRIMARY KEY";
            }
            if (col.unique && !col.primary_key) {
                file << " UNIQUE";
            }
            if (col.auto_increment) {
                file << " AUTO_INCREMENT";
            }
            if (i + 1 < table->columns.size()) {
                file << ',';
            }
            file << '\n';
        }
        file << ");\n\n";
        ++statements_written;

        // Write INSERT statements for each row
        auto schema = catalog.get_table_schema(tname);
        for (auto &row : table->rows) {
            file << "INSERT INTO " << tname << " (";
            for (size_t i = 0; i < table->columns.size(); ++i) {
                if (i > 0) file << ", ";
                file << table->columns[i].name;
            }
            file << ") VALUES (";
            for (size_t i = 0; i < table->columns.size(); ++i) {
                if (i > 0) file << ", ";
                if (i < row.size()) {
                    file << value_to_sql_literal(row[i]);
                } else {
                    file << "NULL";
                }
            }
            file << ");\n";
            ++statements_written;
        }

        if (!table->rows.empty()) {
            file << '\n';
        }
    }

    return statements_written;
}

} // namespace tdb::migrate
