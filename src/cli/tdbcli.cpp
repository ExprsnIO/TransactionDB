/*
 * tdbcli — Interactive SQL shell for TDB
 *
 * Usage:
 *   tdbcli                     # interactive mode, in-memory database
 *   tdbcli /path/to/db         # interactive mode, named database
 *   tdbcli -e "SQL"            # execute single statement and exit
 *   tdbcli -f script.sql       # execute SQL file and exit
 *   tdbcli --pack src out      # pack a database into a single .tdb v2 file
 *   tdbcli --help              # show help
 *   tdbcli --version           # show version
 *
 * Interactive commands:
 *   \q, \quit, exit            # quit
 *   \dt                        # list tables
 *   \dv                        # list views
 *   \di                        # list indexes
 *   \ds                        # list sequences
 *   \d <table>                 # describe table columns
 *   \timing                    # toggle query timing display
 *   \import <file> <table>     # import CSV into table
 *   \export <table> <file>     # export table to CSV
 *   \dump [file]               # dump database as SQL
 */

#include "tdb/database.h"
#include "tdb/migrate.h"
#include "tdb/dbfile.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <unistd.h>
#include <sys/stat.h>

// ─── ANSI colors ───
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_CYAN    "\033[36m"

static bool g_running = true;
static bool g_show_timing = false;
static bool g_use_color = true;

static void signal_handler(int) { g_running = false; }

// ─── Formatting helpers ───

static std::string truncate(const std::string &s, size_t max_width) {
    if (s.size() <= max_width) return s;
    if (max_width < 4) return s.substr(0, max_width);
    return s.substr(0, max_width - 3) + "...";
}

static void print_result_table(const tdb::sql::ResultSet &result) {
    if (result.columns.empty() && result.rows.empty()) return;

    // Compute column widths
    std::vector<size_t> widths;
    for (auto &col : result.columns) {
        widths.push_back(col.name.size());
    }
    // Ensure widths vector matches max columns
    for (auto &row : result.rows) {
        while (widths.size() < row.size()) widths.push_back(0);
        for (size_t i = 0; i < row.size(); i++) {
            size_t len = row[i].to_string().size();
            if (i < widths.size() && len > widths[i]) widths[i] = len;
        }
    }

    // Cap at 40 chars per column
    for (auto &w : widths) w = std::min(w, (size_t)40);

    // Print header
    if (!result.columns.empty()) {
        if (g_use_color) std::cout << C_BOLD;
        for (size_t i = 0; i < result.columns.size(); i++) {
            if (i > 0) std::cout << " | ";
            std::string name = result.columns[i].name;
            std::cout << truncate(name, widths[i]);
            if (name.size() < widths[i])
                std::cout << std::string(widths[i] - name.size(), ' ');
        }
        if (g_use_color) std::cout << C_RESET;
        std::cout << "\n";

        // Separator
        for (size_t i = 0; i < result.columns.size(); i++) {
            if (i > 0) std::cout << "-+-";
            std::cout << std::string(widths[i], '-');
        }
        std::cout << "\n";
    }

    // Print rows
    for (auto &row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); i++) {
            if (i > 0) std::cout << " | ";
            std::string val = row[i].to_string();
            if (row[i].is_null() && g_use_color) {
                std::cout << C_DIM << truncate(val, widths[i]) << C_RESET;
            } else {
                std::cout << truncate(val, widths[i]);
            }
            if (val.size() < widths[i])
                std::cout << std::string(widths[i] - val.size(), ' ');
        }
        std::cout << "\n";
    }

    // Row count
    if (g_use_color) std::cout << C_DIM;
    std::cout << "(" << result.rows.size() << " row"
              << (result.rows.size() != 1 ? "s" : "") << ")";
    if (g_use_color) std::cout << C_RESET;
    std::cout << "\n";
}

static void print_affected(const tdb::sql::ResultSet &result) {
    if (result.rows_affected > 0) {
        std::cout << result.rows_affected << " row"
                  << (result.rows_affected != 1 ? "s" : "") << " affected\n";
    }
}

// ─── Meta-commands ───

static void cmd_list_tables(tdb::Database &db) {
    auto result = db.execute("SELECT table_name, table_type FROM information_schema.tables");
    if (result.success) {
        print_result_table(result);
    } else {
        // Fallback: list from catalog directly
        auto tables = db.catalog().list_tables();
        std::cout << "Tables:\n";
        for (auto &t : tables) std::cout << "  " << t << "\n";
    }
}

static void cmd_list_views(tdb::Database &db) {
    auto views = db.catalog().list_views();
    auto matviews = db.catalog().list_materialized_views();
    std::cout << C_BOLD << "Views:" << C_RESET << "\n";
    for (auto &v : views) std::cout << "  " << v << "\n";
    for (auto &v : matviews) std::cout << "  " << v << " (materialized)\n";
    if (views.empty() && matviews.empty()) std::cout << "  (none)\n";
}

static void cmd_list_indexes(tdb::Database &db) {
    auto indexes = db.catalog().list_indexes();
    std::cout << C_BOLD << "Indexes:" << C_RESET << "\n";
    for (auto &i : indexes) {
        auto *idx = db.catalog().find_index(i);
        if (idx) {
            std::cout << "  " << i << " ON " << idx->table;
            if (idx->unique) std::cout << " (UNIQUE)";
            std::cout << " [";
            for (size_t j = 0; j < idx->columns.size(); j++) {
                if (j > 0) std::cout << ", ";
                std::cout << idx->columns[j];
            }
            std::cout << "]\n";
        }
    }
    if (indexes.empty()) std::cout << "  (none)\n";
}

static void cmd_list_sequences(tdb::Database &db) {
    auto result = db.execute("SELECT sequence_name FROM information_schema.sequences");
    if (result.success && !result.rows.empty()) {
        print_result_table(result);
    } else {
        std::cout << "  (none)\n";
    }
}

static void cmd_describe_table(tdb::Database &db, const std::string &table_name) {
    auto *table = db.catalog().find_table(table_name);
    if (!table) {
        std::cout << C_RED << "Table '" << table_name << "' not found" << C_RESET << "\n";
        return;
    }

    std::cout << C_BOLD << "Table: " << table_name << C_RESET << "\n";
    tdb::sql::ResultSet result;
    result.columns = {{"Column", ""}, {"Type", ""}, {"Nullable", ""}, {"Key", ""}, {"Extra", ""}};
    for (auto &col : table->columns) {
        std::string key;
        if (col.primary_key) key = "PRI";
        else if (col.unique) key = "UNI";

        std::string extra;
        if (col.auto_increment) extra = "auto_increment";
        if (col.generated) extra = "generated";
        if (col.encrypted) extra += extra.empty() ? "encrypted" : " encrypted";

        result.rows.push_back({
            tdb::sql::Value::make_string(col.name),
            tdb::sql::Value::make_string(col.type.name),
            tdb::sql::Value::make_string(col.nullable ? "YES" : "NO"),
            tdb::sql::Value::make_string(key),
            tdb::sql::Value::make_string(extra),
        });
    }
    print_result_table(result);

    // Show row count
    std::cout << C_DIM << "Rows: " << table->rows.size() << C_RESET << "\n";
}

static void cmd_import_csv(tdb::Database &db, const std::string &args) {
    // Parse: \import <file> <table>
    std::istringstream iss(args);
    std::string file, table;
    iss >> file >> table;
    if (file.empty() || table.empty()) {
        std::cout << "Usage: \\import <file.csv> <table_name>\n";
        return;
    }
    tdb::migrate::CsvOptions opts;
    auto count = tdb::migrate::import_csv(db.catalog(), table, file, opts);
    if (count >= 0) {
        std::cout << count << " rows imported into " << table << "\n";
    } else {
        std::cout << C_RED << "Import failed" << C_RESET << "\n";
    }
}

static void cmd_export_csv(tdb::Database &db, const std::string &args) {
    std::istringstream iss(args);
    std::string table, file;
    iss >> table >> file;
    if (file.empty() || table.empty()) {
        std::cout << "Usage: \\export <table_name> <file.csv>\n";
        return;
    }
    tdb::migrate::CsvOptions opts;
    auto count = tdb::migrate::export_csv(db.catalog(), table, file, opts);
    if (count >= 0) {
        std::cout << count << " rows exported from " << table << "\n";
    } else {
        std::cout << C_RED << "Export failed" << C_RESET << "\n";
    }
}

static void cmd_dump(tdb::Database &db, const std::string &args) {
    std::string file = args;
    // Trim whitespace
    while (!file.empty() && file.back() == ' ') file.pop_back();
    while (!file.empty() && file.front() == ' ') file.erase(file.begin());

    if (file.empty()) file = "tdb_dump.sql";
    auto count = tdb::migrate::export_sql_dump(db.catalog(), file);
    if (count >= 0) {
        std::cout << count << " statements written to " << file << "\n";
    } else {
        std::cout << C_RED << "Dump failed" << C_RESET << "\n";
    }
}

// ─── \pack — write a self-contained single-file .tdb v2 and verify it ───

static bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Pack the current database to `out`. If `force` is false and `out` already
// exists, refuses. Verifies the result reopens as a valid v2 file and prints
// an object summary. Returns true on success.
static bool pack_database(tdb::Database &db, const std::string &out, bool force) {
    if (out.empty()) {
        std::cout << "Usage: \\pack <out.tdb> [--force]\n";
        return false;
    }
    if (!force && file_exists(out)) {
        std::cout << C_RED << "Refusing to overwrite '" << out
                  << "' (use --force)" << C_RESET << "\n";
        return false;
    }

    if (!db.save_as(out)) {
        std::cout << C_RED << "Pack failed: could not write '" << out << "'"
                  << C_RESET << "\n";
        return false;
    }

    // Verify: reopen into a throwaway catalog.
    int ver = tdb::dbfile::detect_version(out);
    if (ver != 2) {
        std::cout << C_RED << "Pack verification failed: '" << out
                  << "' is not a v2 file" << C_RESET << "\n";
        return false;
    }
    tdb::catalog::Catalog verify;
    if (!tdb::dbfile::load(out, verify)) {
        std::cout << C_RED << "Pack verification failed: '" << out
                  << "' did not reload cleanly" << C_RESET << "\n";
        return false;
    }

    // Object summary.
    size_t n_tables = verify.list_tables().size();
    size_t n_docs   = verify.list_documents().size();
    size_t n_scripts= verify.list_scripts().size();
    size_t n_trigs  = verify.list_triggers().size();
    size_t n_users  = verify.list_users().size();
    size_t n_idx    = verify.list_indexes().size();

    struct stat st;
    long sz = (stat(out.c_str(), &st) == 0) ? (long)st.st_size : -1;

    if (g_use_color) std::cout << C_GREEN;
    std::cout << "Packed -> " << out;
    if (g_use_color) std::cout << C_RESET;
    std::cout << "\n  " << n_tables << " table(s), "
              << n_idx << " index(es), "
              << n_docs << " document(s), "
              << n_scripts << " script(s), "
              << n_trigs << " trigger(s), "
              << n_users << " user(s)";
    if (sz >= 0) std::cout << "  [" << sz << " bytes]";
    std::cout << "\n";
    return true;
}

static void cmd_pack(tdb::Database &db, const std::string &args) {
    // Parse: \pack <out.tdb> [--force]
    std::istringstream iss(args);
    std::string out, opt;
    iss >> out >> opt;
    bool force = (opt == "--force");
    pack_database(db, out, force);
}

// ─── Help ───

static void print_help() {
    std::cout <<
        "tdbcli — TDB Interactive SQL Shell\n"
        "\n"
        "Usage:\n"
        "  tdbcli                       Interactive mode (in-memory database)\n"
        "  tdbcli <path>                Interactive mode with named database\n"
        "  tdbcli -e \"SQL\"              Execute SQL and exit\n"
        "  tdbcli -f script.sql         Execute SQL file and exit\n"
        "  tdbcli --pack <src> <out>    Pack a database into a single .tdb v2 file\n"
        "  tdbcli --version             Show version\n"
        "  tdbcli --help                Show this help\n"
        "\n"
        "Interactive Commands:\n"
        "  \\q, \\quit, exit             Quit the shell\n"
        "  \\dt                         List tables\n"
        "  \\dv                         List views\n"
        "  \\di                         List indexes\n"
        "  \\ds                         List sequences\n"
        "  \\d <table>                  Describe table columns\n"
        "  \\timing                     Toggle query timing display\n"
        "  \\import <file> <table>      Import CSV file into table\n"
        "  \\export <table> <file>      Export table to CSV file\n"
        "  \\save [file]                Save database to file\n"
        "  \\pack <file> [--force]      Pack database into a single .tdb v2 file\n"
        "  \\dump [file]                Export database as SQL dump\n"
        "  \\status                     Show database info\n"
        "  \\clear                      Clear screen\n"
        "  \\help                       Show this help\n"
        "\n"
        "SQL statements are terminated with ';' and can span multiple lines.\n";
}

static void print_version() {
    std::cout << "tdbcli v1.0.0 (TDB SQL Database Engine)\n"
              << "Built with C11/C++20 on " << __DATE__ << "\n";
}

static void print_banner(const std::string &db_path) {
    if (g_use_color) {
        std::cout << C_CYAN << C_BOLD
                  << "  ╔═══════════════════════════════╗\n"
                  << "  ║      TDB v1.0.0-GM CLI        ║\n"
                  << "  ║  Type \\help for commands       ║\n"
                  << "  ║  Type \\q to quit               ║\n"
                  << "  ╚═══════════════════════════════╝\n"
                  << C_RESET;
    } else {
        std::cout << "TDB v1.0.0-GM CLI. Type \\help for commands, \\q to quit.\n";
    }
    if (!db_path.empty()) {
        std::cout << "  Database: " << db_path << "\n";
        std::cout << "  (auto-saves on exit)\n";
    } else {
        std::cout << "  In-memory mode (use \\save <file> to persist)\n";
    }
    std::cout << "\n";
}

// ─── Execute SQL (handles multi-line input) ───

static void execute_sql(tdb::Database &db, const std::string &sql) {
    auto result = db.execute(sql);
    if (!result.success) {
        if (g_use_color) std::cout << C_RED;
        std::cout << "ERROR: " << result.error_message;
        if (g_use_color) std::cout << C_RESET;
        std::cout << "\n";
        return;
    }

    // Display result
    if (!result.columns.empty() || !result.rows.empty()) {
        print_result_table(result);
    } else if (result.rows_affected > 0) {
        print_affected(result);
    } else {
        std::cout << "OK\n";
    }

    // Timing
    if (g_show_timing) {
        if (g_use_color) std::cout << C_DIM;
        std::cout << "Time: " << db.last_execution_time_ms() << " ms";
        if (g_use_color) std::cout << C_RESET;
        std::cout << "\n";
    }
}

// ─── Execute SQL file ───

static void execute_file(tdb::Database &db, const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open file '" << path << "'\n";
        return;
    }

    std::string line, buffer;
    int block_depth = 0; // Track BEGIN/END nesting for PL/SQL blocks
    while (std::getline(file, line)) {
        // Skip comments
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.erase(trimmed.begin());
        if (trimmed.empty() || trimmed.substr(0, 2) == "--") continue;

        buffer += line + " ";

        // Simple PL/SQL block depth tracking:
        // A line containing "AS BEGIN" or standalone "BEGIN" (after CREATE PROCEDURE etc.) opens a block.
        // A line starting with "END" (standalone, not END IF/END LOOP) closes it.
        std::string upper = trimmed;
        for (auto &c : upper) c = (char)std::toupper((unsigned char)c);

        // Detect block open: "AS BEGIN" or "AS$" followed by BEGIN on next line
        if (upper.find("AS BEGIN") != std::string::npos) {
            block_depth++;
        } else if (block_depth == 0 && upper == "BEGIN") {
            // Standalone BEGIN after a procedure header that ended on the previous line
            // Check if the buffer contains CREATE PROCEDURE/FUNCTION/TRIGGER
            std::string buf_upper = buffer;
            for (auto &c : buf_upper) c = (char)std::toupper((unsigned char)c);
            if (buf_upper.find("CREATE") != std::string::npos &&
                (buf_upper.find("PROCEDURE") != std::string::npos ||
                 buf_upper.find("FUNCTION") != std::string::npos ||
                 buf_upper.find("TRIGGER") != std::string::npos)) {
                block_depth++;
            }
        }

        // Detect block close: line is "END" or "END;" (standalone, not END IF/END LOOP)
        if (block_depth > 0) {
            std::string trimmed_upper = upper;
            while (!trimmed_upper.empty() && trimmed_upper.back() == ';') trimmed_upper.pop_back();
            while (!trimmed_upper.empty() && trimmed_upper.back() == ' ') trimmed_upper.pop_back();
            if (trimmed_upper == "END") {
                block_depth--;
            }
        }

        // Check for statement terminator — respect string literals and PL/SQL blocks
        // Find the last unquoted semicolon on the line
        bool has_unquoted_semi = false;
        {
            bool in_str = false;
            for (char ch : line) {
                if (ch == '\'') in_str = !in_str;
                if (ch == ';' && !in_str) { has_unquoted_semi = true; break; }
            }
        }
        if (has_unquoted_semi && block_depth <= 0) {
            while (!buffer.empty() && (buffer.back() == ' ' || buffer.back() == ';'))
                buffer.pop_back();
            if (!buffer.empty()) {
                execute_sql(db, buffer);
            }
            buffer.clear();
            block_depth = 0;
        }
    }
    // Execute any remaining buffer
    if (!buffer.empty()) {
        while (!buffer.empty() && (buffer.back() == ' ' || buffer.back() == ';'))
            buffer.pop_back();
        if (!buffer.empty()) execute_sql(db, buffer);
    }
}

// ─── Handle meta-command ───

static bool handle_meta(tdb::Database &db, const std::string &cmd) {
    if (cmd == "\\q" || cmd == "\\quit" || cmd == "exit" || cmd == "quit")
        return false; // signal exit

    if (cmd == "\\dt") { cmd_list_tables(db); return true; }
    if (cmd == "\\dv") { cmd_list_views(db); return true; }
    if (cmd == "\\di") { cmd_list_indexes(db); return true; }
    if (cmd == "\\ds") { cmd_list_sequences(db); return true; }
    if (cmd == "\\help" || cmd == "\\?") { print_help(); return true; }
    if (cmd == "\\clear") { std::cout << "\033[2J\033[H"; return true; }
    if (cmd == "\\timing") {
        g_show_timing = !g_show_timing;
        std::cout << "Timing is " << (g_show_timing ? "on" : "off") << ".\n";
        return true;
    }

    if (cmd.substr(0, 3) == "\\d " && cmd.size() > 3) {
        cmd_describe_table(db, cmd.substr(3));
        return true;
    }
    if (cmd.substr(0, 8) == "\\import ") {
        cmd_import_csv(db, cmd.substr(8));
        return true;
    }
    if (cmd.substr(0, 8) == "\\export ") {
        cmd_export_csv(db, cmd.substr(8));
        return true;
    }
    if (cmd.substr(0, 5) == "\\dump") {
        cmd_dump(db, cmd.size() > 5 ? cmd.substr(5) : "");
        return true;
    }
    if (cmd.substr(0, 6) == "\\pack ") {
        cmd_pack(db, cmd.substr(6));
        return true;
    }
    if (cmd == "\\pack") {
        std::cout << "Usage: \\pack <out.tdb> [--force]\n";
        return true;
    }
    if (cmd == "\\save") {
        if (db.path().empty()) {
            std::cout << "No database file set. Use \\save <filename> or start tdbcli with a path.\n";
        } else {
            if (db.save()) std::cout << "Database saved to " << db.path() << "\n";
            else std::cout << C_RED << "Save failed" << C_RESET << "\n";
        }
        return true;
    }
    if (cmd.substr(0, 6) == "\\save ") {
        std::string file = cmd.substr(6);
        while (!file.empty() && file.front() == ' ') file.erase(file.begin());
        if (file.empty()) {
            std::cout << "Usage: \\save <filename>\n";
        } else {
            if (db.save_as(file)) std::cout << "Database saved to " << file << "\n";
            else std::cout << C_RED << "Save failed" << C_RESET << "\n";
        }
        return true;
    }
    if (cmd == "\\status") {
        std::cout << "Database: " << (db.path().empty() ? "(in-memory)" : db.path()) << "\n";
        auto tables = db.catalog().list_tables();
        int64_t total_rows = 0;
        for (auto &t : tables) {
            auto *ti = db.catalog().find_table(t);
            if (ti) total_rows += (int64_t)ti->rows.size();
        }
        std::cout << "Tables: " << tables.size() << ", Total rows: " << total_rows << "\n";
        return true;
    }

    std::cout << "Unknown command: " << cmd << ". Type \\help for help.\n";
    return true;
}

// ─── Main ───

int main(int argc, char *argv[]) {
    std::signal(SIGINT, signal_handler);

    // Check if stdout is a terminal
    g_use_color = isatty(fileno(stdout));

    std::string db_path;
    std::string exec_sql;
    std::string exec_file;
    std::string pack_src, pack_out;
    bool pack_mode = false;
    bool pack_force = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_help(); return 0; }
        if (arg == "--version" || arg == "-v") { print_version(); return 0; }
        if (arg == "--no-color") { g_use_color = false; continue; }
        if (arg == "--force") { pack_force = true; continue; }
        if ((arg == "-e" || arg == "--execute") && i + 1 < argc) {
            exec_sql = argv[++i]; continue;
        }
        if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            exec_file = argv[++i]; continue;
        }
        if (arg == "--pack" && i + 2 < argc) {
            // --pack <src.tdb> <out.tdb>  : non-interactive, open then pack.
            pack_mode = true;
            pack_src = argv[++i];
            pack_out = argv[++i];
            continue;
        }
        if (arg == "--pack") {
            std::cerr << "Usage: tdbcli --pack <src.tdb> <out.tdb> [--force]\n";
            return 1;
        }
        if (arg[0] != '-') {
            db_path = arg; continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        return 1;
    }

    // Non-interactive pack mode: open source, write a single-file v2 .tdb, exit.
    if (pack_mode) {
        tdb::Database packdb;
        if (!packdb.open(pack_src)) {
            std::cerr << "Error: failed to open source database '" << pack_src << "'\n";
            return 1;
        }
        bool ok = pack_database(packdb, pack_out, pack_force);
        packdb.close();
        return ok ? 0 : 1;
    }

    // Open database
    tdb::Database db;
    if (!db.open(db_path)) {
        std::cerr << "Error: failed to open database"
                  << (db_path.empty() ? "" : " '" + db_path + "'") << "\n";
        return 1;
    }

    // Execute-and-exit modes
    if (!exec_sql.empty()) {
        execute_sql(db, exec_sql);
        db.close();
        return 0;
    }
    if (!exec_file.empty()) {
        execute_file(db, exec_file);
        db.close();
        return 0;
    }

    // Interactive mode
    print_banner(db_path);

    std::string line, buffer;
    bool in_multi_line = false;

    while (g_running) {
        // Prompt
        if (g_use_color) {
            std::cout << (in_multi_line ? C_DIM "  ... " C_RESET : C_GREEN "tdb> " C_RESET);
        } else {
            std::cout << (in_multi_line ? "  ... " : "tdb> ");
        }
        std::cout.flush();

        if (!std::getline(std::cin, line)) break; // EOF
        if (line.empty()) continue;

        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();

        // Handle meta-commands (only when not in multi-line SQL)
        if (!in_multi_line && !line.empty() && (line[0] == '\\' || line == "exit" || line == "quit")) {
            if (!handle_meta(db, line)) break; // \q returns false
            continue;
        }

        // Accumulate SQL
        if (!buffer.empty()) buffer += " ";
        buffer += line;

        // Check for statement terminator
        if (buffer.find(';') != std::string::npos) {
            // Remove trailing semicolons
            while (!buffer.empty() && (buffer.back() == ' ' || buffer.back() == ';'))
                buffer.pop_back();
            if (!buffer.empty()) execute_sql(db, buffer);
            buffer.clear();
            in_multi_line = false;
        } else {
            in_multi_line = true;
        }
    }

    db.close();
    std::cout << "\nGoodbye.\n";
    return 0;
}
