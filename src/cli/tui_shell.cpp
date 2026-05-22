// tui_shell.cpp — TDB Interactive SQL Shell (TUI)
// Copyright 2026 Rick Holland. Apache 2.0 License.
//
// A rich text-based interactive SQL shell for TDB with syntax highlighting,
// tab completion, command history, formatted output, Lua REPL, and more.
// Uses POSIX termios for raw terminal control — no ncurses dependency.

#include "tdb/database.h"
#include "tdb/migrate.h"
#include "tdb/dbfile.h"
#include "tdb/script.h"
#include "tdb/sql/lexer.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <csignal>
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>
#include <sys/stat.h>
#include <map>
#include <set>
#include <numeric>
#include <cassert>
#include <cerrno>
#include <climits>
#include <pwd.h>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
static const char* TDB_TUI_VERSION = "1.1.0";

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class TuiShell;
static TuiShell* g_shell = nullptr;

// ---------------------------------------------------------------------------
// Key codes
// ---------------------------------------------------------------------------
enum Key {
    KEY_NONE      = 0,
    KEY_CTRL_A    = 1,
    KEY_CTRL_B    = 2,
    KEY_CTRL_C    = 3,
    KEY_CTRL_D    = 4,
    KEY_CTRL_E    = 5,
    KEY_CTRL_F    = 6,
    KEY_CTRL_K    = 11,
    KEY_CTRL_L    = 12,
    KEY_ENTER     = 13,
    KEY_CTRL_N    = 14,
    KEY_CTRL_P    = 16,
    KEY_CTRL_U    = 21,
    KEY_CTRL_W    = 23,
    KEY_ESCAPE    = 27,
    KEY_TAB       = 9,
    KEY_BACKSPACE = 127,
    // Extended keys (above byte range)
    KEY_UP        = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_SHIFT_TAB,
};

// ---------------------------------------------------------------------------
// Color theme
// ---------------------------------------------------------------------------
struct ColorTheme {
    std::string name;
    // SQL syntax colors
    std::string keyword;
    std::string function;
    std::string string_lit;
    std::string number_lit;
    std::string comment;
    std::string identifier;
    std::string type_name;
    std::string op;
    // UI colors
    std::string prompt;
    std::string continuation;
    std::string error;
    std::string success;
    std::string status_fg;
    std::string status_bg;
    std::string header;
    std::string border;
    std::string null_val;
    std::string selection;
    std::string reset;
};

static ColorTheme makeDarkTheme() {
    ColorTheme t;
    t.name        = "dark";
    t.keyword     = "\033[38;5;141m";
    t.function    = "\033[38;5;75m";
    t.string_lit  = "\033[38;5;114m";
    t.number_lit  = "\033[38;5;208m";
    t.comment     = "\033[38;5;243m";
    t.identifier  = "\033[38;5;252m";
    t.type_name   = "\033[38;5;81m";
    t.op          = "\033[38;5;252m";
    t.prompt      = "\033[38;5;141m\033[1m";
    t.continuation= "\033[38;5;243m";
    t.error       = "\033[38;5;196m";
    t.success     = "\033[38;5;114m";
    t.status_fg   = "\033[38;5;252m";
    t.status_bg   = "\033[48;5;236m";
    t.header      = "\033[1m\033[38;5;75m";
    t.border      = "\033[38;5;243m";
    t.null_val    = "\033[38;5;243m\033[3m";
    t.selection   = "\033[48;5;238m\033[38;5;75m";
    t.reset       = "\033[0m";
    return t;
}

static ColorTheme makeLightTheme() {
    ColorTheme t;
    t.name        = "light";
    t.keyword     = "\033[38;5;55m";
    t.function    = "\033[38;5;26m";
    t.string_lit  = "\033[38;5;28m";
    t.number_lit  = "\033[38;5;130m";
    t.comment     = "\033[38;5;245m";
    t.identifier  = "\033[38;5;234m";
    t.type_name   = "\033[38;5;30m";
    t.op          = "\033[38;5;234m";
    t.prompt      = "\033[38;5;55m\033[1m";
    t.continuation= "\033[38;5;245m";
    t.error       = "\033[38;5;160m";
    t.success     = "\033[38;5;28m";
    t.status_fg   = "\033[38;5;234m";
    t.status_bg   = "\033[48;5;253m";
    t.header      = "\033[1m\033[38;5;26m";
    t.border      = "\033[38;5;245m";
    t.null_val    = "\033[38;5;245m\033[3m";
    t.selection   = "\033[48;5;253m\033[38;5;26m";
    t.reset       = "\033[0m";
    return t;
}

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------
static std::string getHomePath() {
    const char* home = getenv("HOME");
    if (home) return std::string(home);
    struct passwd* pw = getpwuid(getuid());
    if (pw) return std::string(pw->pw_dir);
    return ".";
}

static std::string historyPath() {
    return getHomePath() + "/.tdb_history";
}

static std::string toUpper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static std::string trimRight(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static std::string trimLeft(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start);
}

static std::string trim(const std::string& s) {
    return trimLeft(trimRight(s));
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool startsWithCI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

// Count display width of a string (simple: count non-continuation UTF-8 bytes)
static size_t displayWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        // Skip ANSI escape sequences
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') ++i;
            continue;
        }
        // Skip UTF-8 continuation bytes
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

// Strip ANSI escape codes from a string
__attribute__((unused))
static std::string stripAnsi(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if ((unsigned char)s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') ++i;
            continue;
        }
        r += s[i];
    }
    return r;
}

// Repeat a string n times
static std::string repeatStr(const std::string& s, size_t n) {
    std::string r;
    r.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) r += s;
    return r;
}

// Check if a character is part of a word
static bool isWordChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// ---------------------------------------------------------------------------
// SQL keywords, functions, and type names for highlighting and completion
// ---------------------------------------------------------------------------
static const std::vector<std::string>& sqlKeywords() {
    static std::vector<std::string> kw = {
        "ABORT", "ACTION", "ADD", "AFTER", "ALL", "ALTER", "ALWAYS", "ANALYZE",
        "AND", "AS", "ASC", "ATTACH", "AUTOINCREMENT",
        "BEFORE", "BEGIN", "BETWEEN", "BY",
        "CASCADE", "CASE", "CAST", "CHECK", "COLLATE", "COLUMN", "COMMIT",
        "CONFLICT", "CONSTRAINT", "CREATE", "CROSS", "CURRENT", "CURRENT_DATE",
        "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "DATABASE", "DEFAULT", "DEFERRABLE", "DEFERRED", "DELETE", "DESC",
        "DETACH", "DISTINCT", "DO", "DROP",
        "EACH", "ELSE", "END", "ESCAPE", "EXCEPT", "EXCLUDE", "EXCLUSIVE",
        "EXISTS", "EXPLAIN",
        "FAIL", "FILTER", "FIRST", "FOLLOWING", "FOR", "FOREIGN", "FROM", "FULL",
        "GENERATED", "GLOB", "GROUP", "GROUPS",
        "HAVING",
        "IF", "IGNORE", "IMMEDIATE", "IN", "INDEX", "INDEXED", "INITIALLY",
        "INNER", "INSERT", "INSTEAD", "INTERSECT", "INTO", "IS", "ISNULL",
        "JOIN",
        "KEY",
        "LAST", "LEFT", "LIKE", "LIMIT",
        "MATCH", "MATERIALIZED",
        "NATURAL", "NO", "NOT", "NOTHING", "NOTNULL", "NULL", "NULLS",
        "OF", "OFFSET", "ON", "OR", "ORDER", "OTHERS", "OUTER", "OVER",
        "PARTITION", "PLAN", "PRAGMA", "PRECEDING", "PRIMARY",
        "QUERY",
        "RAISE", "RANGE", "RECURSIVE", "REFERENCES", "REFRESH", "REGEXP",
        "REINDEX", "RELEASE", "RENAME", "REPLACE", "RESTRICT", "RETURNING",
        "RIGHT", "ROLLBACK", "ROW", "ROWS",
        "SAVEPOINT", "SELECT", "SEQUENCE", "SET", "TABLE", "TEMP", "TEMPORARY",
        "THEN", "TIES", "TO", "TRANSACTION", "TRIGGER",
        "UNBOUNDED", "UNION", "UNIQUE", "UPDATE", "USING",
        "VACUUM", "VALUES", "VIEW", "VIRTUAL",
        "WHEN", "WHERE", "WINDOW", "WITH", "WITHOUT",
    };
    return kw;
}

static const std::vector<std::string>& sqlTypeNames() {
    static std::vector<std::string> tn = {
        "INT", "INTEGER", "TINYINT", "SMALLINT", "MEDIUMINT", "BIGINT",
        "UNSIGNED", "INT2", "INT4", "INT8",
        "REAL", "DOUBLE", "FLOAT", "NUMERIC", "DECIMAL",
        "TEXT", "VARCHAR", "CHAR", "NCHAR", "NVARCHAR", "CLOB",
        "BLOB", "BINARY", "VARBINARY",
        "BOOLEAN", "BOOL",
        "DATE", "DATETIME", "TIMESTAMP", "TIME",
        "JSON", "JSONB", "XML", "UUID", "INTERVAL",
        "SERIAL", "BIGSERIAL",
        "MONEY", "BIT", "VARBIT",
        "POINT", "LINE", "POLYGON", "CIRCLE", "BOX", "PATH",
        "INET", "CIDR", "MACADDR",
        "ARRAY", "RECORD",
    };
    return tn;
}

static const std::vector<std::string>& sqlFunctions() {
    static std::vector<std::string> fn = {
        // Aggregate
        "COUNT", "SUM", "AVG", "MIN", "MAX", "GROUP_CONCAT", "TOTAL",
        "STRING_AGG", "ARRAY_AGG", "JSON_AGG", "BOOL_AND", "BOOL_OR",
        "STDDEV", "STDDEV_POP", "STDDEV_SAMP", "VARIANCE", "VAR_POP", "VAR_SAMP",
        "BIT_AND", "BIT_OR", "BIT_XOR", "EVERY", "PERCENTILE_CONT",
        "PERCENTILE_DISC", "MODE", "REGR_SLOPE", "REGR_INTERCEPT", "CORR",
        "COVAR_POP", "COVAR_SAMP",
        // Window
        "ROW_NUMBER", "RANK", "DENSE_RANK", "PERCENT_RANK", "CUME_DIST",
        "NTILE", "LAG", "LEAD", "FIRST_VALUE", "LAST_VALUE", "NTH_VALUE",
        // String
        "LENGTH", "UPPER", "LOWER", "TRIM", "LTRIM", "RTRIM", "SUBSTR",
        "SUBSTRING", "REPLACE", "INSTR", "REVERSE", "REPEAT", "LPAD", "RPAD",
        "LEFT", "RIGHT", "CONCAT", "CONCAT_WS", "CHAR_LENGTH", "OCTET_LENGTH",
        "POSITION", "OVERLAY", "TRANSLATE", "ASCII", "CHR", "INITCAP",
        "SPLIT_PART", "REGEXP_REPLACE", "REGEXP_MATCHES", "REGEXP_SUBSTR",
        "STARTS_WITH", "ENDS_WITH", "MD5", "SHA256", "SHA512", "ENCODE", "DECODE",
        "TO_HEX", "FROM_HEX", "BASE64_ENCODE", "BASE64_DECODE",
        "QUOTE_LITERAL", "QUOTE_IDENT", "FORMAT",
        // Math
        "ABS", "ROUND", "CEIL", "CEILING", "FLOOR", "TRUNC", "TRUNCATE",
        "MOD", "POWER", "SQRT", "CBRT", "EXP", "LN", "LOG", "LOG2", "LOG10",
        "SIGN", "PI", "DEGREES", "RADIANS",
        "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN", "ATAN2",
        "SINH", "COSH", "TANH",
        "RANDOM", "SETSEED", "GCD", "LCM", "FACTORIAL",
        "WIDTH_BUCKET", "DIV",
        // Date/Time
        "NOW", "DATE", "TIME", "DATETIME", "JULIANDAY", "STRFTIME",
        "DATE_PART", "DATE_TRUNC", "EXTRACT", "AGE", "MAKE_DATE", "MAKE_TIME",
        "MAKE_TIMESTAMP", "TO_DATE", "TO_TIMESTAMP", "TO_CHAR",
        "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP", "LOCALTIME",
        "LOCALTIMESTAMP", "CLOCK_TIMESTAMP", "TIMEOFDAY",
        "DATE_ADD", "DATE_SUB", "DATEDIFF", "TIMEDIFF",
        "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
        "DAYOFWEEK", "DAYOFYEAR", "WEEK", "QUARTER",
        // Type conversion
        "CAST", "TYPEOF", "COALESCE", "NULLIF", "IIF", "IFNULL", "NVL", "NVL2",
        "GREATEST", "LEAST", "DECODE",
        // JSON
        "JSON", "JSON_EXTRACT", "JSON_INSERT", "JSON_REPLACE", "JSON_SET",
        "JSON_REMOVE", "JSON_TYPE", "JSON_VALID", "JSON_ARRAY", "JSON_OBJECT",
        "JSON_ARRAY_LENGTH", "JSON_EACH", "JSON_TREE", "JSON_GROUP_ARRAY",
        "JSON_GROUP_OBJECT", "JSON_QUOTE", "JSON_UNQUOTE",
        "JSON_PATCH", "JSON_MERGE_PATCH", "JSON_MERGE_PRESERVE",
        "JSONB_EXTRACT", "JSONB_INSERT", "JSONB_SET",
        // XML / XPath / XQuery
        "XMLPARSE", "XMLSERIALIZE", "XPATH", "XPATH_EXISTS", "XQUERY",
        "XML_ELEMENT", "XML_FOREST", "XML_AGG", "XML_COMMENT", "XML_PI",
        "XMLTABLE", "XMLNAMESPACES",
        // Misc
        "HEX", "UNHEX", "ZEROBLOB", "QUOTE", "UNICODE", "CHAR",
        "GLOB", "LIKE", "PRINTF", "TOTAL_CHANGES", "CHANGES",
        "LAST_INSERT_ROWID", "ROWID",
        "GENERATE_SERIES", "UNNEST",
        "UUID_GENERATE_V4", "GEN_RANDOM_UUID",
        "ENCRYPT", "DECRYPT", "HMAC",
        "TDIGEST_CREATE", "TDIGEST_ADD", "TDIGEST_QUANTILE",
    };
    return fn;
}

static const std::vector<std::string>& metaCommands() {
    static std::vector<std::string> mc = {
        "\\dt", "\\dv", "\\di", "\\ds", "\\dm", "\\dp", "\\d",
        "\\timing", "\\import", "\\export", "\\dump", "\\save", "\\pack",
        "\\status", "\\stats", "\\clear", "\\help", "\\quit", "\\exit",
        "\\lua", "\\sql", "\\theme", "\\exec", "\\browse", "\\history",
        "\\reset", "\\open", "\\close", "\\info", "\\schema", "\\explain",
    };
    return mc;
}

// Build a set of SQL keywords (upper) for fast lookup
static const std::set<std::string>& sqlKeywordSet() {
    static std::set<std::string> s;
    if (s.empty()) {
        for (auto& k : sqlKeywords()) s.insert(k);
    }
    return s;
}

static const std::set<std::string>& sqlTypeNameSet() {
    static std::set<std::string> s;
    if (s.empty()) {
        for (auto& t : sqlTypeNames()) s.insert(t);
    }
    return s;
}

static const std::set<std::string>& sqlFunctionSet() {
    static std::set<std::string> s;
    if (s.empty()) {
        for (auto& f : sqlFunctions()) s.insert(f);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------
class Terminal {
public:
    Terminal() : raw_enabled_(false), rows_(24), cols_(80) {}

    bool enableRaw() {
        if (raw_enabled_) return true;
        if (tcgetattr(STDIN_FILENO, &orig_termios_) == -1) return false;
        struct termios raw = orig_termios_;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1; // 100ms timeout
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;
        raw_enabled_ = true;
        updateSize();
        return true;
    }

    void disableRaw() {
        if (!raw_enabled_) return;
        // Reset scroll region
        std::string s = "\033[r";
        ::write(STDOUT_FILENO, s.c_str(), s.size());
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
        raw_enabled_ = false;
    }

    void updateSize() {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            cols_ = ws.ws_col;
            rows_ = ws.ws_row;
        }
    }

    int rows() const { return rows_; }
    int cols() const { return cols_; }

    int readKey() {
        char c;
        int nread;
        while ((nread = (int)::read(STDIN_FILENO, &c, 1)) == 0) {
            // timeout, no input
        }
        if (nread == -1) return KEY_NONE;

        if (c == '\x1b') {
            // Escape sequence
            char seq[5];
            if (::read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESCAPE;
            if (::read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESCAPE;

            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    if (::read(STDIN_FILENO, &seq[2], 1) != 1) return KEY_ESCAPE;
                    if (seq[2] == '~') {
                        switch (seq[1]) {
                            case '1': return KEY_HOME;
                            case '3': return KEY_DELETE;
                            case '4': return KEY_END;
                            case '5': return KEY_PAGE_UP;
                            case '6': return KEY_PAGE_DOWN;
                            case '7': return KEY_HOME;
                            case '8': return KEY_END;
                        }
                    }
                    // Handle CSI 1;2Z etc for shift-tab
                    if (seq[1] == '1' && seq[2] == ';') {
                        char mod, code;
                        if (::read(STDIN_FILENO, &mod, 1) == 1 &&
                            ::read(STDIN_FILENO, &code, 1) == 1) {
                            // Shift+Tab = CSI 1;2Z but that's rare
                        }
                    }
                } else {
                    switch (seq[1]) {
                        case 'A': return KEY_UP;
                        case 'B': return KEY_DOWN;
                        case 'C': return KEY_RIGHT;
                        case 'D': return KEY_LEFT;
                        case 'H': return KEY_HOME;
                        case 'F': return KEY_END;
                        case 'Z': return KEY_SHIFT_TAB;
                    }
                }
            } else if (seq[0] == 'O') {
                switch (seq[1]) {
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
            return KEY_ESCAPE;
        }

        // Handle UTF-8 multi-byte sequences
        if ((unsigned char)c >= 0xC0) {
            // Multi-byte UTF-8: read remaining bytes and return the codepoint
            // For simplicity, treat as a single character event
            unsigned char uc = (unsigned char)c;
            int extra = 0;
            if ((uc & 0xE0) == 0xC0) extra = 1;
            else if ((uc & 0xF0) == 0xE0) extra = 2;
            else if ((uc & 0xF8) == 0xF0) extra = 3;

            int codepoint = uc & (0x3F >> extra);
            for (int i = 0; i < extra; ++i) {
                char cb;
                if (::read(STDIN_FILENO, &cb, 1) != 1) break;
                codepoint = (codepoint << 6) | ((unsigned char)cb & 0x3F);
            }
            return codepoint;
        }

        return (unsigned char)c;
    }

    void write(const std::string& s) {
        ::write(STDOUT_FILENO, s.c_str(), s.size());
    }

private:
    struct termios orig_termios_;
    bool raw_enabled_;
    int rows_, cols_;
};

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
class History {
public:
    History() : pos_(0), max_size_(1000) {}

    void load() {
        std::ifstream f(historyPath());
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            // Decode multi-line: \x01 -> \n
            std::string decoded;
            for (size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '\x01') decoded += '\n';
                else decoded += line[i];
            }
            if (!decoded.empty()) entries_.push_back(decoded);
        }
        if (entries_.size() > max_size_) {
            entries_.erase(entries_.begin(),
                           entries_.begin() + (entries_.size() - max_size_));
        }
        pos_ = (int)entries_.size();
    }

    void save() {
        std::ofstream f(historyPath());
        if (!f.is_open()) return;
        size_t start = 0;
        if (entries_.size() > max_size_) start = entries_.size() - max_size_;
        for (size_t i = start; i < entries_.size(); ++i) {
            std::string encoded;
            for (char c : entries_[i]) {
                if (c == '\n') encoded += '\x01';
                else encoded += c;
            }
            f << encoded << '\n';
        }
    }

    void add(const std::string& entry) {
        if (entry.empty()) return;
        // Remove duplicate if it's the last entry
        if (!entries_.empty() && entries_.back() == entry) {
            pos_ = (int)entries_.size();
            return;
        }
        entries_.push_back(entry);
        if (entries_.size() > max_size_) entries_.erase(entries_.begin());
        pos_ = (int)entries_.size();
    }

    std::string prev(const std::string& current) {
        if (entries_.empty()) return current;
        if (pos_ == (int)entries_.size()) saved_ = current;
        if (pos_ > 0) --pos_;
        return entries_[pos_];
    }

    std::string next(const std::string& /*current*/) {
        if (pos_ >= (int)entries_.size() - 1) {
            pos_ = (int)entries_.size();
            return saved_;
        }
        ++pos_;
        return entries_[pos_];
    }

    void resetPos() { pos_ = (int)entries_.size(); }

    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
    std::string saved_;
    int pos_;
    size_t max_size_;
};

// ---------------------------------------------------------------------------
// Syntax Highlighter
// ---------------------------------------------------------------------------
class Highlighter {
public:
    Highlighter(const ColorTheme& theme) : theme_(theme) {}

    std::string highlight(const std::string& input) const {
        if (input.empty()) return "";

        std::string result;
        result.reserve(input.size() * 3);

        try {
            tdb::sql::Lexer lexer(input);
            size_t last_pos = 0;

            while (true) {
                auto token = lexer.next_token();
                // Determine the position and value from the token
                std::string val = token.value;
                auto type = token.type;

                // Check for end of input
                if (type == tdb::sql::TokenType::END_OF_INPUT || val.empty()) {
                    // Append any remaining text
                    if (last_pos < input.size()) {
                        result += theme_.identifier;
                        result += input.substr(last_pos);
                        result += theme_.reset;
                    }
                    break;
                }

                // Find where this token appears in the input (from last_pos onward)
                size_t tok_pos = input.find(val, last_pos);
                if (tok_pos == std::string::npos) {
                    // Token not found at expected position — skip
                    // Try to continue
                    continue;
                }

                // Append any whitespace/gap between last token and this one
                if (tok_pos > last_pos) {
                    result += input.substr(last_pos, tok_pos - last_pos);
                }

                // Colorize based on token type
                std::string color = classifyColor(type, val);
                result += color;
                result += val;
                result += theme_.reset;

                last_pos = tok_pos + val.size();
            }
        } catch (...) {
            // If lexer throws, fall back to unhighlighted
            return theme_.identifier + input + theme_.reset;
        }

        return result;
    }

private:
    std::string classifyColor(tdb::sql::TokenType type, const std::string& val) const {
        std::string upper = toUpper(val);

        switch (type) {
            case tdb::sql::TokenType::STRING_LITERAL:
                return theme_.string_lit;
            case tdb::sql::TokenType::INTEGER_LITERAL:
            case tdb::sql::TokenType::FLOAT_LITERAL:
                return theme_.number_lit;
            case tdb::sql::TokenType::INVALID:
                return theme_.comment;
            default:
                break;
        }

        // Check if it's a keyword
        if (sqlKeywordSet().count(upper)) return theme_.keyword;
        // Check if it's a type name
        if (sqlTypeNameSet().count(upper)) return theme_.type_name;
        // Check if it's a function name
        if (sqlFunctionSet().count(upper)) return theme_.function;

        // Operators and punctuation
        if (type == tdb::sql::TokenType::PLUS ||
            type == tdb::sql::TokenType::SEMICOLON ||
            type == tdb::sql::TokenType::COMMA ||
            type == tdb::sql::TokenType::LPAREN ||
            type == tdb::sql::TokenType::RPAREN ||
            type == tdb::sql::TokenType::DOT ||
            type == tdb::sql::TokenType::STAR) {
            return theme_.op;
        }

        return theme_.identifier;
    }

    const ColorTheme& theme_;
};

// ---------------------------------------------------------------------------
// Tab Completer
// ---------------------------------------------------------------------------
class Completer {
public:
    Completer() : completion_idx_(-1) {}

    void setDatabase(tdb::Database* db) { db_ = db; }

    void buildStaticCompletions() {
        // SQL keywords (lowercase for user convenience)
        for (auto& k : sqlKeywords()) {
            all_completions_.push_back(toUpper(k));
        }
        // Functions
        for (auto& f : sqlFunctions()) {
            all_completions_.push_back(toUpper(f));
        }
        // Type names
        for (auto& t : sqlTypeNames()) {
            all_completions_.push_back(toUpper(t));
        }
        // Meta-commands
        for (auto& m : metaCommands()) {
            all_completions_.push_back(m);
        }
    }

    void refreshDatabaseCompletions() {
        db_completions_.clear();
        if (!db_) return;
        try {
            auto& cat = db_->catalog();
            for (auto& t : cat.list_tables()) {
                db_completions_.push_back(t);
            }
            for (auto& v : cat.list_views()) {
                db_completions_.push_back(v);
            }
            for (auto& idx : cat.list_indexes()) {
                db_completions_.push_back(idx);
            }
        } catch (...) {}
    }

    struct CompletionResult {
        std::vector<std::string> matches;
        std::string prefix;    // the word being completed
        size_t prefix_start;   // position in input where prefix starts
    };

    CompletionResult complete(const std::string& input, size_t cursor) {
        CompletionResult res;
        res.prefix_start = cursor;

        // Find the word being typed
        size_t word_start = cursor;
        while (word_start > 0 && (isWordChar(input[word_start - 1]) || input[word_start - 1] == '\\')) {
            --word_start;
        }
        res.prefix_start = word_start;
        res.prefix = input.substr(word_start, cursor - word_start);

        if (res.prefix.empty()) return res;

        std::string prefix_upper = toUpper(res.prefix);
        bool is_meta = (res.prefix[0] == '\\');

        // Collect matching completions
        for (auto& c : all_completions_) {
            if (is_meta) {
                if (startsWithCI(c, res.prefix)) {
                    res.matches.push_back(c);
                }
            } else {
                std::string c_upper = toUpper(c);
                if (startsWith(c_upper, prefix_upper)) {
                    res.matches.push_back(c);
                }
            }
        }

        // Database objects
        for (auto& c : db_completions_) {
            std::string c_upper = toUpper(c);
            if (startsWith(c_upper, prefix_upper)) {
                res.matches.push_back(c);
            }
        }

        // Sort and deduplicate
        std::sort(res.matches.begin(), res.matches.end());
        res.matches.erase(std::unique(res.matches.begin(), res.matches.end()),
                          res.matches.end());

        return res;
    }

    void resetIndex() { completion_idx_ = -1; }

private:
    tdb::Database* db_ = nullptr;
    std::vector<std::string> all_completions_;
    std::vector<std::string> db_completions_;
    int completion_idx_;
};

// ---------------------------------------------------------------------------
// Line Editor
// ---------------------------------------------------------------------------
class LineEditor {
public:
    LineEditor() : cursor_(0) {}

    void clear() {
        buffer_.clear();
        cursor_ = 0;
    }

    void set(const std::string& s) {
        buffer_ = s;
        cursor_ = s.size();
    }

    const std::string& buffer() const { return buffer_; }
    size_t cursor() const { return cursor_; }

    void insertChar(int ch) {
        if (ch < 32 && ch != '\n' && ch != '\t') return;

        // Handle UTF-8
        std::string s;
        if (ch < 0x80) {
            s += (char)ch;
        } else if (ch < 0x800) {
            s += (char)(0xC0 | (ch >> 6));
            s += (char)(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            s += (char)(0xE0 | (ch >> 12));
            s += (char)(0x80 | ((ch >> 6) & 0x3F));
            s += (char)(0x80 | (ch & 0x3F));
        } else if (ch < 0x110000) {
            s += (char)(0xF0 | (ch >> 18));
            s += (char)(0x80 | ((ch >> 12) & 0x3F));
            s += (char)(0x80 | ((ch >> 6) & 0x3F));
            s += (char)(0x80 | (ch & 0x3F));
        }

        buffer_.insert(cursor_, s);
        cursor_ += s.size();
    }

    void backspace() {
        if (cursor_ == 0) return;
        // Handle UTF-8: move back past continuation bytes
        size_t del_start = cursor_ - 1;
        while (del_start > 0 && ((unsigned char)buffer_[del_start] & 0xC0) == 0x80) {
            --del_start;
        }
        buffer_.erase(del_start, cursor_ - del_start);
        cursor_ = del_start;
    }

    void deleteChar() {
        if (cursor_ >= buffer_.size()) return;
        size_t del_end = cursor_ + 1;
        while (del_end < buffer_.size() && ((unsigned char)buffer_[del_end] & 0xC0) == 0x80) {
            ++del_end;
        }
        buffer_.erase(cursor_, del_end - cursor_);
    }

    void moveLeft() {
        if (cursor_ == 0) return;
        --cursor_;
        while (cursor_ > 0 && ((unsigned char)buffer_[cursor_] & 0xC0) == 0x80) {
            --cursor_;
        }
    }

    void moveRight() {
        if (cursor_ >= buffer_.size()) return;
        ++cursor_;
        while (cursor_ < buffer_.size() && ((unsigned char)buffer_[cursor_] & 0xC0) == 0x80) {
            ++cursor_;
        }
    }

    void moveHome() { cursor_ = 0; }

    void moveEnd() { cursor_ = buffer_.size(); }

    void killToEnd() {
        buffer_.erase(cursor_);
    }

    void killToStart() {
        buffer_.erase(0, cursor_);
        cursor_ = 0;
    }

    void killWord() {
        if (cursor_ == 0) return;
        size_t end = cursor_;
        // Skip trailing whitespace
        while (cursor_ > 0 && buffer_[cursor_ - 1] == ' ') --cursor_;
        // Skip word
        while (cursor_ > 0 && buffer_[cursor_ - 1] != ' ') --cursor_;
        buffer_.erase(cursor_, end - cursor_);
    }

    void replaceWord(size_t start, size_t len, const std::string& replacement) {
        buffer_.erase(start, len);
        buffer_.insert(start, replacement);
        cursor_ = start + replacement.size();
    }

    // Get line count and current line/col for multi-line display
    struct Position {
        int line;
        int col;
        int total_lines;
    };

    Position getPosition() const {
        Position p = {0, 0, 1};
        for (size_t i = 0; i < buffer_.size(); ++i) {
            if (i == cursor_) {
                p.line = p.total_lines - 1;
                // col is counted from the last newline
            }
            if (buffer_[i] == '\n') {
                p.total_lines++;
            }
        }
        // Calculate column
        size_t line_start = buffer_.rfind('\n', cursor_ > 0 ? cursor_ - 1 : 0);
        if (line_start == std::string::npos || cursor_ == 0) {
            p.col = (int)cursor_;
        } else {
            p.col = (int)(cursor_ - line_start - 1);
        }
        if (cursor_ == buffer_.size()) {
            p.line = p.total_lines - 1;
            size_t ls = buffer_.rfind('\n');
            if (ls == std::string::npos) p.col = (int)cursor_;
            else p.col = (int)(cursor_ - ls - 1);
        }
        return p;
    }

    // Split buffer into lines
    std::vector<std::string> lines() const {
        std::vector<std::string> result;
        std::istringstream ss(buffer_);
        std::string line;
        while (std::getline(ss, line)) {
            result.push_back(line);
        }
        if (result.empty()) result.push_back("");
        // If buffer ends with newline, add empty line
        if (!buffer_.empty() && buffer_.back() == '\n') {
            result.push_back("");
        }
        return result;
    }

private:
    std::string buffer_;
    size_t cursor_;
};

// ---------------------------------------------------------------------------
// Result Formatter
// ---------------------------------------------------------------------------
class ResultFormatter {
public:
    ResultFormatter(const ColorTheme& theme, int max_width)
        : theme_(theme), max_width_(max_width) {}

    std::string format(const tdb::sql::ResultSet& result) {
        if (result.columns.empty()) return "";

        size_t num_cols = result.columns.size();
        size_t num_rows = result.rows.size();

        // Calculate column widths
        std::vector<size_t> widths(num_cols);
        std::vector<bool> is_numeric(num_cols, false);

        for (size_t c = 0; c < num_cols; ++c) {
            widths[c] = result.columns[c].name.size();
        }

        for (size_t r = 0; r < num_rows; ++r) {
            for (size_t c = 0; c < num_cols && c < result.rows[r].size(); ++c) {
                std::string val = result.rows[r][c].to_string();
                if (result.rows[r][c].is_null()) {
                    widths[c] = std::max(widths[c], (size_t)4);
                } else {
                    widths[c] = std::max(widths[c], val.size());
                }
            }
        }

        // Detect numeric columns (check first non-null value)
        for (size_t c = 0; c < num_cols; ++c) {
            for (size_t r = 0; r < num_rows; ++r) {
                if (c < result.rows[r].size() && !result.rows[r][c].is_null()) {
                    const std::string v = result.rows[r][c].to_string();
                    bool numeric = !v.empty();
                    for (char ch : v) {
                        if (!isdigit((unsigned char)ch) && ch != '.' && ch != '-' && ch != '+' && ch != 'e' && ch != 'E') {
                            numeric = false;
                            break;
                        }
                    }
                    is_numeric[c] = numeric;
                    break;
                }
            }
        }

        // Clamp column widths
        size_t total = 1; // leading border
        for (size_t c = 0; c < num_cols; ++c) {
            total += widths[c] + 3; // " val " + border
        }
        if ((int)total > max_width_ && max_width_ > 20) {
            // Reduce widths proportionally
            double scale = (double)(max_width_ - 1) / (double)total;
            for (size_t c = 0; c < num_cols; ++c) {
                size_t min_w = std::min(widths[c], std::max(result.columns[c].name.size(), (size_t)4));
                widths[c] = std::max(min_w, (size_t)(widths[c] * scale));
            }
        }

        std::ostringstream out;

        // In raw terminal mode, \n alone doesn't return the carriage.
        // Use \r\n for correct output.
        const char* nl = "\r\n";

        // Top border
        out << theme_.border;
        out << "\xe2\x94\x8c"; // ┌
        for (size_t c = 0; c < num_cols; ++c) {
            out << repeatStr("\xe2\x94\x80", widths[c] + 2); // ─
            out << (c < num_cols - 1 ? "\xe2\x94\xac" : "\xe2\x94\x90"); // ┬ or ┐
        }
        out << theme_.reset << nl;

        // Header
        out << theme_.border << "\xe2\x94\x82" << theme_.reset; // │
        for (size_t c = 0; c < num_cols; ++c) {
            out << " " << theme_.header;
            std::string h = result.columns[c].name;
            if (h.size() > widths[c]) h = h.substr(0, widths[c]);
            out << h;
            if (h.size() < widths[c]) out << std::string(widths[c] - h.size(), ' ');
            out << theme_.reset << " ";
            out << theme_.border << "\xe2\x94\x82" << theme_.reset; // │
        }
        out << nl;

        // Header separator
        out << theme_.border;
        out << "\xe2\x94\x9c"; // ├
        for (size_t c = 0; c < num_cols; ++c) {
            out << repeatStr("\xe2\x94\x80", widths[c] + 2); // ─
            out << (c < num_cols - 1 ? "\xe2\x94\xbc" : "\xe2\x94\xa4"); // ┼ or ┤
        }
        out << theme_.reset << nl;

        // Data rows
        for (size_t r = 0; r < num_rows; ++r) {
            out << theme_.border << "\xe2\x94\x82" << theme_.reset; // │
            for (size_t c = 0; c < num_cols; ++c) {
                std::string val;
                bool is_null = true;
                if (c < result.rows[r].size()) {
                    is_null = result.rows[r][c].is_null();
                    val = result.rows[r][c].to_string();
                }
                std::string display_val = val;
                if (display_val.size() > widths[c]) {
                    display_val = display_val.substr(0, widths[c] - 1) + "\xe2\x80\xa6"; // …
                }

                size_t pad = 0;
                if (display_val.size() < widths[c]) pad = widths[c] - display_val.size();

                out << " ";
                if (is_null) {
                    if (is_numeric[c] && pad > 0) out << std::string(pad, ' ');
                    out << theme_.null_val << "NULL" << theme_.reset;
                    if (!is_numeric[c] && pad > 0) out << std::string(pad, ' ');
                } else if (is_numeric[c]) {
                    if (pad > 0) out << std::string(pad, ' ');
                    out << theme_.number_lit << display_val << theme_.reset;
                } else {
                    out << display_val;
                    if (pad > 0) out << std::string(pad, ' ');
                }
                out << " ";
                out << theme_.border << "\xe2\x94\x82" << theme_.reset; // │
            }
            out << nl;
        }

        // Bottom border
        out << theme_.border;
        out << "\xe2\x94\x94"; // └
        for (size_t c = 0; c < num_cols; ++c) {
            out << repeatStr("\xe2\x94\x80", widths[c] + 2); // ─
            out << (c < num_cols - 1 ? "\xe2\x94\xb4" : "\xe2\x94\x98"); // ┴ or ┘
        }
        out << theme_.reset << nl;

        // Row count
        out << theme_.success << "(" << num_rows << " row" << (num_rows != 1 ? "s" : "") << ")" << theme_.reset << nl;

        return out.str();
    }

private:
    const ColorTheme& theme_;
    int max_width_;
};

// ---------------------------------------------------------------------------
// TUI Shell
// ---------------------------------------------------------------------------
class TuiShell {
public:
    TuiShell() :
        running_(false),
        timing_(false),
        lua_mode_(false),
        query_count_(0),
        total_query_time_ms_(0.0),
        last_query_time_ms_(0.0),
        last_row_count_(0),
        show_completion_menu_(false),
        completion_menu_idx_(0),
        db_(nullptr)
    {
        theme_ = makeDarkTheme();
        completer_.buildStaticCompletions();
    }

    ~TuiShell() {
        terminal_.disableRaw();
    }

    int run(int argc, char* argv[]) {
        // Parse args
        std::string db_path;
        std::string exec_sql;
        bool batch_mode = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                printUsage();
                return 0;
            } else if (arg == "--version" || arg == "-v") {
                printf("TDB Shell v%s\n", TDB_TUI_VERSION);
                return 0;
            } else if ((arg == "--exec" || arg == "-e") && i + 1 < argc) {
                exec_sql = argv[++i];
                batch_mode = true;
            } else if (arg == "--light") {
                theme_ = makeLightTheme();
            } else if (arg == "--dark") {
                theme_ = makeDarkTheme();
            } else if (arg[0] != '-') {
                db_path = arg;
            }
        }

        // Open/create database
        db_ = new tdb::Database();
        if (!db_path.empty()) {
            try {
                db_->open(db_path);
                db_path_ = db_path;
            } catch (const std::exception& e) {
                fprintf(stderr, "Error opening '%s': %s\n", db_path.c_str(), e.what());
                delete db_;
                return 1;
            }
        }

        completer_.setDatabase(db_);
        completer_.refreshDatabaseCompletions();

        // Batch mode
        if (batch_mode) {
            return executeBatch(exec_sql);
        }

        // Interactive mode
        if (!isatty(STDIN_FILENO)) {
            return executePipe();
        }

        return interactiveLoop();
    }

private:
    // ----- Batch mode -----
    int executeBatch(const std::string& sql) {
        try {
            auto result = db_->execute(sql);
            if (!result.columns.empty()) {
                // Print as tab-separated
                for (size_t c = 0; c < result.columns.size(); ++c) {
                    if (c > 0) printf("\t");
                    printf("%s", result.columns[c].name.c_str());
                }
                printf("\n");
                for (auto& row : result.rows) {
                    for (size_t c = 0; c < row.size(); ++c) {
                        if (c > 0) printf("\t");
                        printf("%s", row[c].to_string().c_str());
                    }
                    printf("\n");
                }
            }
            if (!result.error_message.empty()) {
                printf("%s\n", result.error_message.c_str());
            }
            return 0;
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            return 1;
        }
    }

    int executePipe() {
        std::string sql;
        std::string line;
        while (std::getline(std::cin, line)) {
            sql += line + "\n";
        }
        if (!sql.empty()) return executeBatch(sql);
        return 0;
    }

    // ----- Interactive loop -----
    int interactiveLoop() {
        if (!terminal_.enableRaw()) {
            fprintf(stderr, "Failed to enable raw terminal mode\n");
            return 1;
        }

        // Setup signal handlers
        g_shell = this;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handleSigwinch;
        sigaction(SIGWINCH, &sa, nullptr);

        sa.sa_handler = handleSigint;
        sigaction(SIGINT, &sa, nullptr);

        // Set scroll region (protect status bar at bottom)
        setupScrollRegion();

        // Load history
        history_.load();

        running_ = true;

        // Print banner
        printBanner();

        // Main loop
        while (running_) {
            renderLine();

            int key = terminal_.readKey();
            if (key == KEY_NONE) continue;

            handleKey(key);
        }

        // Save history
        history_.save();

        // Clean up
        clearStatusBar();
        terminal_.write("\033[r"); // Reset scroll region
        terminal_.write("\r\n");
        terminal_.disableRaw();

        delete db_;
        db_ = nullptr;

        return 0;
    }

    void setupScrollRegion() {
        terminal_.updateSize();
        int rows = terminal_.rows();
        // Set scroll region to all rows except the last
        std::ostringstream cmd;
        cmd << "\033[1;" << (rows - 1) << "r";
        terminal_.write(cmd.str());
        // Move cursor to home
        terminal_.write("\033[H");
    }

    void handleSigwinchImpl() {
        terminal_.updateSize();
        setupScrollRegion();
        renderStatusBar();
    }

    static void handleSigwinch(int) {
        if (g_shell) g_shell->handleSigwinchImpl();
    }

    static void handleSigint(int) {
        if (g_shell) {
            g_shell->editor_.clear();
            g_shell->show_completion_menu_ = false;
        }
    }

    // ----- Key handling -----
    void handleKey(int key) {
        // Close completion menu on most keys
        if (key != KEY_TAB && key != KEY_SHIFT_TAB &&
            key != KEY_UP && key != KEY_DOWN &&
            show_completion_menu_ && key != KEY_ENTER) {
            show_completion_menu_ = false;
        }

        switch (key) {
            case KEY_CTRL_D:
                if (editor_.buffer().empty()) {
                    running_ = false;
                }
                break;

            case KEY_CTRL_C:
                editor_.clear();
                show_completion_menu_ = false;
                multi_line_buffer_.clear();
                writeOutput("\r\n");
                break;

            case KEY_ENTER:
                if (show_completion_menu_ && !completion_matches_.empty()) {
                    // Accept highlighted completion
                    applyCompletion(completion_matches_[completion_menu_idx_]);
                    show_completion_menu_ = false;
                    break;
                }
                handleEnter();
                break;

            case KEY_TAB:
                handleTab();
                break;

            case KEY_SHIFT_TAB:
                if (show_completion_menu_ && !completion_matches_.empty()) {
                    completion_menu_idx_--;
                    if (completion_menu_idx_ < 0) completion_menu_idx_ = (int)completion_matches_.size() - 1;
                }
                break;

            case KEY_BACKSPACE:
                editor_.backspace();
                completer_.resetIndex();
                break;

            case KEY_DELETE:
                editor_.deleteChar();
                break;

            case KEY_LEFT:
            case KEY_CTRL_B:
                editor_.moveLeft();
                break;

            case KEY_RIGHT:
            case KEY_CTRL_F:
                editor_.moveRight();
                break;

            case KEY_HOME:
            case KEY_CTRL_A:
                editor_.moveHome();
                break;

            case KEY_END:
            case KEY_CTRL_E:
                editor_.moveEnd();
                break;

            case KEY_UP:
            case KEY_CTRL_P:
                if (show_completion_menu_ && !completion_matches_.empty()) {
                    completion_menu_idx_--;
                    if (completion_menu_idx_ < 0) completion_menu_idx_ = (int)completion_matches_.size() - 1;
                } else {
                    editor_.set(history_.prev(editor_.buffer()));
                }
                break;

            case KEY_DOWN:
            case KEY_CTRL_N:
                if (show_completion_menu_ && !completion_matches_.empty()) {
                    completion_menu_idx_++;
                    if (completion_menu_idx_ >= (int)completion_matches_.size()) completion_menu_idx_ = 0;
                } else {
                    editor_.set(history_.next(editor_.buffer()));
                }
                break;

            case KEY_CTRL_K:
                editor_.killToEnd();
                break;

            case KEY_CTRL_U:
                editor_.killToStart();
                break;

            case KEY_CTRL_W:
                editor_.killWord();
                break;

            case KEY_CTRL_L:
                // Clear screen
                terminal_.write("\033[2J\033[H");
                setupScrollRegion();
                break;

            case KEY_ESCAPE:
                show_completion_menu_ = false;
                break;

            case KEY_PAGE_UP:
            case KEY_PAGE_DOWN:
                // Could implement scroll-back buffer here
                break;

            default:
                if (key >= 32 || key > 127) { // printable or UTF-8
                    editor_.insertChar(key);
                    completer_.resetIndex();
                }
                break;
        }
    }

    void handleEnter() {
        std::string input = editor_.buffer();
        std::string trimmed = trim(input);

        if (trimmed.empty() && multi_line_buffer_.empty()) {
            writeOutput("\r\n");
            return;
        }

        // Check if this is a meta-command (starts with backslash)
        // Meta-commands work in both SQL and Lua mode
        if (multi_line_buffer_.empty() && !trimmed.empty() && trimmed[0] == '\\') {
            writeOutput("\r\n");
            history_.add(trimmed);
            executeMetaCommand(trimmed);
            editor_.clear();
            return;
        }

        // Lua REPL mode: execute each line immediately
        if (lua_mode_) {
            writeOutput("\r\n");
            if (!trimmed.empty()) {
                history_.add(trimmed);
                executeLua(trimmed);
            }
            editor_.clear();
            return;
        }

        // SQL mode: accumulate multi-line input
        if (!multi_line_buffer_.empty()) {
            multi_line_buffer_ += "\n" + input;
        } else {
            multi_line_buffer_ = input;
        }

        // Check if SQL is complete (ends with semicolon, ignoring trailing whitespace)
        std::string full = trimRight(multi_line_buffer_);
        if (!full.empty() && full.back() == ';') {
            writeOutput("\r\n");
            history_.add(multi_line_buffer_);
            executeSQL(multi_line_buffer_);
            multi_line_buffer_.clear();
            editor_.clear();
        } else {
            // Continue on next line
            writeOutput("\r\n");
            editor_.clear();
        }
    }

    void handleTab() {
        if (show_completion_menu_ && !completion_matches_.empty()) {
            // Cycle through completions
            completion_menu_idx_++;
            if (completion_menu_idx_ >= (int)completion_matches_.size()) completion_menu_idx_ = 0;
            return;
        }

        auto result = completer_.complete(editor_.buffer(), editor_.cursor());
        if (result.matches.empty()) return;

        if (result.matches.size() == 1) {
            // Single match: apply it
            applyCompletion(result.matches[0]);
        } else {
            // Multiple matches: find common prefix and show menu
            std::string common = result.matches[0];
            for (size_t i = 1; i < result.matches.size(); ++i) {
                size_t j = 0;
                while (j < common.size() && j < result.matches[i].size() &&
                       tolower((unsigned char)common[j]) == tolower((unsigned char)result.matches[i][j])) {
                    ++j;
                }
                common = common.substr(0, j);
            }

            // Apply common prefix if longer than current prefix
            if (common.size() > result.prefix.size()) {
                editor_.replaceWord(result.prefix_start, result.prefix.size(), common);
            }

            // Show completion menu
            completion_matches_ = result.matches;
            completion_prefix_ = result.prefix;
            completion_prefix_start_ = result.prefix_start;
            completion_menu_idx_ = 0;
            show_completion_menu_ = true;
        }
    }

    void applyCompletion(const std::string& completion) {
        auto result = completer_.complete(editor_.buffer(), editor_.cursor());
        editor_.replaceWord(result.prefix_start, editor_.cursor() - result.prefix_start, completion);
        show_completion_menu_ = false;

        // Add trailing space for keywords, opening paren for functions
        std::string upper = toUpper(completion);
        if (sqlFunctionSet().count(upper)) {
            editor_.insertChar('(');
        } else if (!startsWith(completion, "\\")) {
            editor_.insertChar(' ');
        }
    }

    // ----- Rendering -----
    void renderLine() {
        std::ostringstream out;
        int cols = terminal_.cols();

        // Save cursor, move to beginning of line
        out << "\r";
        out << "\033[2K"; // Clear entire line

        // Determine prompt
        std::string prompt_text;
        std::string prompt_color;
        if (lua_mode_) {
            prompt_text = "lua> ";
            prompt_color = "\033[38;5;208m\033[1m"; // bold orange
        } else if (!multi_line_buffer_.empty()) {
            prompt_text = "  -> ";
            prompt_color = theme_.continuation;
        } else {
            prompt_text = "tdb> ";
            prompt_color = theme_.prompt;
        }

        out << prompt_color << prompt_text << theme_.reset;

        // Syntax-highlight the current input
        std::string input = editor_.buffer();
        std::string highlighted;
        if (!lua_mode_ && !input.empty()) {
            Highlighter hl(theme_);
            std::string full_input = multi_line_buffer_.empty() ? input : (multi_line_buffer_ + "\n" + input);
            // Only highlight the current line portion
            highlighted = hl.highlight(input);
        } else {
            highlighted = input;
        }

        out << highlighted;

        // Position cursor correctly
        size_t prompt_len = prompt_text.size();
        size_t cursor_col = prompt_len + editor_.cursor();

        // Handle line wrapping
        int cursor_row_offset = (int)(cursor_col / cols);
        int cursor_col_pos = (int)(cursor_col % cols);

        // Move cursor to correct position
        out << "\r";
        if (cursor_col_pos > 0 || cursor_row_offset == 0) {
            out << "\033[" << (cursor_col_pos + 1) << "G";
        }

        // Render completion menu if active
        if (show_completion_menu_ && !completion_matches_.empty()) {
            renderCompletionMenu(out);
        }

        // Render status bar
        renderStatusBarTo(out);

        terminal_.write(out.str());
    }

    void renderCompletionMenu(std::ostringstream& out) {
        int max_show = std::min((int)completion_matches_.size(), 12);
        int start = 0;
        if (completion_menu_idx_ >= max_show) {
            start = completion_menu_idx_ - max_show + 1;
        }

        // Calculate max width
        size_t max_w = 0;
        for (int i = start; i < start + max_show && i < (int)completion_matches_.size(); ++i) {
            max_w = std::max(max_w, completion_matches_[i].size());
        }
        max_w += 4;

        // Save cursor position
        out << "\0337"; // Save cursor

        // Move down one line
        out << "\n";

        for (int i = start; i < start + max_show && i < (int)completion_matches_.size(); ++i) {
            out << "\r\033[2K"; // Clear line
            if (i == completion_menu_idx_) {
                out << theme_.selection;
            } else {
                out << theme_.status_bg << theme_.status_fg;
            }
            out << "  " << completion_matches_[i];
            size_t pad = max_w - completion_matches_[i].size() - 2;
            if (pad > 0 && pad < 200) out << std::string(pad, ' ');
            out << theme_.reset;
            if (i < start + max_show - 1) out << "\n";
        }

        // Show count if there are more
        if ((int)completion_matches_.size() > max_show) {
            out << "\n\r\033[2K";
            out << theme_.comment << "  (" << completion_matches_.size() << " total)" << theme_.reset;
        }

        // Restore cursor
        out << "\0338"; // Restore cursor
    }

    void renderStatusBar() {
        std::ostringstream out;
        renderStatusBarTo(out);
        terminal_.write(out.str());
    }

    void renderStatusBarTo(std::ostringstream& out) {
        int rows = terminal_.rows();
        int cols = terminal_.cols();

        // Save cursor position
        out << "\0337";

        // Move to status bar row (last row)
        out << "\033[" << rows << ";1H";

        // Build status content
        std::ostringstream status;
        status << " [TDB v" << TDB_TUI_VERSION << "]";
        status << " \xe2\x94\x82 "; // │

        // Database path
        if (db_path_.empty()) {
            status << "db: (in-memory)";
        } else {
            // Show just filename
            std::string name = db_path_;
            size_t slash = name.rfind('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            status << "db: " << name;
        }

        // Table count
        try {
            size_t table_count = db_->catalog().list_tables().size();
            status << " \xe2\x94\x82 " << table_count << " table" << (table_count != 1 ? "s" : "");
        } catch (...) {
            status << " \xe2\x94\x82 0 tables";
        }

        // Last query info
        if (query_count_ > 0) {
            status << " \xe2\x94\x82 Last: " << std::fixed << std::setprecision(2) << last_query_time_ms_ << "ms";
            status << " \xe2\x94\x82 " << last_row_count_ << " row" << (last_row_count_ != 1 ? "s" : "");
        }

        // Mode
        status << " \xe2\x94\x82 " << (lua_mode_ ? "Lua" : "SQL");

        // Timing
        if (timing_) {
            status << " \xe2\x94\x82 TIMING";
        }

        std::string status_str = status.str();

        // Pad to full width
        size_t display_w = displayWidth(status_str);
        if ((int)display_w < cols) {
            status_str += std::string(cols - display_w, ' ');
        }

        out << theme_.status_bg << theme_.status_fg << status_str << theme_.reset;

        // Restore cursor
        out << "\0338";
    }

    void clearStatusBar() {
        int rows = terminal_.rows();
        std::ostringstream out;
        out << "\0337";
        out << "\033[" << rows << ";1H";
        out << "\033[2K";
        out << "\0338";
        terminal_.write(out.str());
    }

    // Write output (scrolls within the scroll region)
    void writeOutput(const std::string& s) {
        terminal_.write(s);
    }

    // ----- SQL execution -----
    void executeSQL(const std::string& sql) {
        auto start = std::chrono::high_resolution_clock::now();

        try {
            auto result = db_->execute(sql);
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();

            query_count_++;
            last_query_time_ms_ = ms;
            total_query_time_ms_ += ms;

            if (!result.columns.empty()) {
                ResultFormatter fmt(theme_, terminal_.cols());
                std::string table = fmt.format(result);
                writeOutput(table);
                last_row_count_ = (int)result.rows.size();
            } else if (!result.error_message.empty()) {
                writeOutput(theme_.success + result.error_message + theme_.reset + "\r\n");
                last_row_count_ = 0;
            } else {
                writeOutput(theme_.success + "OK" + theme_.reset + "\r\n");
                last_row_count_ = 0;
            }

            if (timing_) {
                std::ostringstream ts;
                ts << theme_.comment << "Time: " << std::fixed << std::setprecision(3) << ms << " ms" << theme_.reset << "\r\n";
                writeOutput(ts.str());
            }

            // Refresh completions (tables may have changed)
            completer_.refreshDatabaseCompletions();

        } catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            query_count_++;
            last_query_time_ms_ = ms;
            total_query_time_ms_ += ms;
            last_row_count_ = 0;

            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    // ----- Lua execution -----
    void executeLua(const std::string& code) {
        auto start = std::chrono::high_resolution_clock::now();

        try {
            tdb::script::ScriptEngineOptions opts;
            opts.execute_fn = [this](const std::string& sql) { return db_->execute(sql); };
            tdb::script::ScriptEngine engine(opts);
            tdb::catalog::ScriptInfo script_info;
            script_info.name = "repl";
            script_info.lua_source = code;

            auto result = engine.call(script_info, {});
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();

            query_count_++;
            last_query_time_ms_ = ms;
            total_query_time_ms_ += ms;

            if (!result.is_null()) {
                writeOutput(theme_.success + result.to_string() + theme_.reset + "\r\n");
            } else {
                writeOutput(theme_.success + "OK" + theme_.reset + "\r\n");
            }
            last_row_count_ = 0;

            if (timing_) {
                std::ostringstream ts;
                ts << theme_.comment << "Time: " << std::fixed << std::setprecision(3) << ms << " ms" << theme_.reset << "\r\n";
                writeOutput(ts.str());
            }

        } catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            query_count_++;
            last_query_time_ms_ = ms;
            total_query_time_ms_ += ms;
            last_row_count_ = 0;

            writeOutput(theme_.error + "Lua Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    // ----- Meta-commands -----
    void executeMetaCommand(const std::string& cmd) {
        std::string trimmed = trim(cmd);
        std::string lower = toLower(trimmed);

        // Parse command and arguments
        std::string command;
        std::string args;
        size_t space = trimmed.find(' ');
        if (space != std::string::npos) {
            command = toLower(trimmed.substr(0, space));
            args = trim(trimmed.substr(space + 1));
        } else {
            command = lower;
        }

        if (command == "\\quit" || command == "\\exit" || command == "\\q") {
            running_ = false;
        }
        else if (command == "\\help" || command == "\\h" || command == "\\?") {
            printHelp();
        }
        else if (command == "\\dt") {
            listTables();
        }
        else if (command == "\\dv") {
            listViews();
        }
        else if (command == "\\di") {
            listIndexes();
        }
        else if (command == "\\ds") {
            listSequences();
        }
        else if (command == "\\dm") {
            listMaterializedViews();
        }
        else if (command == "\\dp") {
            listPrepared();
        }
        else if (startsWith(command, "\\d") && command.size() == 2) {
            // \d with argument: describe table
            if (!args.empty()) {
                describeTable(args);
            } else {
                // \d alone: list all objects
                listAllObjects();
            }
        }
        else if (startsWith(command, "\\d") && command.size() > 2) {
            // e.g., \dtablename (without space between \d and the name)
            // Only if it's not one of the known \dX commands
            static const std::set<std::string> known_d_cmds = {
                "\\dt", "\\dv", "\\di", "\\ds", "\\dm", "\\dp"
            };
            if (known_d_cmds.find(command) == known_d_cmds.end()) {
                // Treat the rest as a table name
                std::string table_name = command.substr(2);
                if (!args.empty()) table_name += " " + args;
                table_name = trim(table_name);
                if (!table_name.empty()) {
                    describeTable(table_name);
                }
            }
        }
        else if (command == "\\timing") {
            timing_ = !timing_;
            writeOutput(theme_.success + "Timing is " + (timing_ ? "on" : "off") + "." + theme_.reset + "\r\n");
        }
        else if (command == "\\clear" || command == "\\cls") {
            terminal_.write("\033[2J\033[H");
            setupScrollRegion();
        }
        else if (command == "\\stats") {
            showStats();
        }
        else if (command == "\\status" || command == "\\info") {
            showStatus();
        }
        else if (command == "\\theme") {
            if (args == "dark") {
                theme_ = makeDarkTheme();
                writeOutput(theme_.success + "Switched to dark theme." + theme_.reset + "\r\n");
            } else if (args == "light") {
                theme_ = makeLightTheme();
                writeOutput(theme_.success + "Switched to light theme." + theme_.reset + "\r\n");
            } else {
                writeOutput("Usage: \\theme dark|light\r\n");
                writeOutput("Current theme: " + theme_.name + "\r\n");
            }
        }
        else if (command == "\\lua") {
            lua_mode_ = true;
            writeOutput(theme_.success + "Entered Lua REPL mode. Type \\sql to return to SQL mode." + theme_.reset + "\r\n");
        }
        else if (command == "\\sql") {
            lua_mode_ = false;
            writeOutput(theme_.success + "Returned to SQL mode." + theme_.reset + "\r\n");
        }
        else if (command == "\\history") {
            showHistory();
        }
        else if (command == "\\save") {
            if (args.empty()) {
                if (db_path_.empty()) {
                    writeOutput(theme_.error + "No database path set. Use: \\save <path>" + theme_.reset + "\r\n");
                } else {
                    try {
                        db_->save();
                        writeOutput(theme_.success + "Database saved to " + db_path_ + theme_.reset + "\r\n");
                    } catch (const std::exception& e) {
                        writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
                    }
                }
            } else {
                try {
                    db_->save_as(args);
                    db_path_ = args;
                    writeOutput(theme_.success + "Database saved to " + args + theme_.reset + "\r\n");
                } catch (const std::exception& e) {
                    writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
                }
            }
        }
        else if (command == "\\open") {
            if (args.empty()) {
                writeOutput(theme_.error + "Usage: \\open <path>" + theme_.reset + "\r\n");
            } else {
                try {
                    db_->open(args);
                    db_path_ = args;
                    completer_.refreshDatabaseCompletions();
                    writeOutput(theme_.success + "Opened database: " + args + theme_.reset + "\r\n");
                } catch (const std::exception& e) {
                    writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
                }
            }
        }
        else if (command == "\\close") {
            try {
                db_->close();
                db_path_.clear();
                completer_.refreshDatabaseCompletions();
                writeOutput(theme_.success + "Database closed." + theme_.reset + "\r\n");
            } catch (const std::exception& e) {
                writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
            }
        }
        else if (command == "\\import") {
            handleImport(args);
        }
        else if (command == "\\export") {
            handleExport(args);
        }
        else if (command == "\\dump") {
            handleDump(args);
        }
        else if (command == "\\pack") {
            handlePack(args);
        }
        else if (command == "\\exec") {
            handleExecFile(args);
        }
        else if (command == "\\schema") {
            if (args.empty()) {
                showFullSchema();
            } else {
                describeTable(args);
            }
        }
        else if (command == "\\browse") {
            if (args.empty()) {
                writeOutput(theme_.error + "Usage: \\browse <table> [limit]" + theme_.reset + "\r\n");
            } else {
                handleBrowse(args);
            }
        }
        else if (command == "\\reset") {
            delete db_;
            db_ = new tdb::Database();
            db_path_.clear();
            completer_.setDatabase(db_);
            completer_.refreshDatabaseCompletions();
            query_count_ = 0;
            total_query_time_ms_ = 0;
            last_query_time_ms_ = 0;
            last_row_count_ = 0;
            writeOutput(theme_.success + "Database reset." + theme_.reset + "\r\n");
        }
        else if (command == "\\explain") {
            if (args.empty()) {
                writeOutput(theme_.error + "Usage: \\explain <SQL>" + theme_.reset + "\r\n");
            } else {
                executeSQL("EXPLAIN " + args);
            }
        }
        else {
            writeOutput(theme_.error + "Unknown command: " + command + ". Type \\help for help." + theme_.reset + "\r\n");
        }
    }

    // ----- Meta-command implementations -----

    void listTables() {
        try {
            auto tables = db_->catalog().list_tables();
            if (tables.empty()) {
                writeOutput(theme_.comment + "No tables found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Tables:" + theme_.reset + "\r\n");
            for (auto& t : tables) {
                try {
                    auto schema = db_->catalog().get_table_schema(t);
                    writeOutput("  " + t + " (" + std::to_string(schema.size()) + " columns)\r\n");
                } catch (...) {
                    writeOutput("  " + t + "\r\n");
                }
            }
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void listViews() {
        try {
            auto views = db_->catalog().list_views();
            if (views.empty()) {
                writeOutput(theme_.comment + "No views found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Views:" + theme_.reset + "\r\n");
            for (auto& v : views) {
                writeOutput("  " + v + "\r\n");
            }
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void listIndexes() {
        try {
            auto indexes = db_->catalog().list_indexes();
            if (indexes.empty()) {
                writeOutput(theme_.comment + "No indexes found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Indexes:" + theme_.reset + "\r\n");
            for (auto& idx : indexes) {
                writeOutput("  " + idx + "\r\n");
            }
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void listSequences() {
        try {
            auto result = db_->execute("SELECT name FROM information_schema.sequences");
            if (result.rows.empty()) {
                writeOutput(theme_.comment + "No sequences found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Sequences:" + theme_.reset + "\r\n");
            for (auto& row : result.rows) {
                if (!row.empty()) writeOutput("  " + row[0].to_string() + "\r\n");
            }
        } catch (...) {
            writeOutput(theme_.comment + "No sequences found." + theme_.reset + "\r\n");
        }
    }

    void listMaterializedViews() {
        try {
            // Try to query information_schema or catalog
            auto result = db_->execute("SELECT name FROM information_schema.views WHERE is_materialized = 'YES'");
            if (result.rows.empty()) {
                writeOutput(theme_.comment + "No materialized views found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Materialized Views:" + theme_.reset + "\r\n");
            for (auto& row : result.rows) {
                if (!row.empty()) writeOutput("  " + row[0].to_string() + "\r\n");
            }
        } catch (...) {
            writeOutput(theme_.comment + "No materialized views found." + theme_.reset + "\r\n");
        }
    }

    void listPrepared() {
        try {
            auto result = db_->execute("SELECT name FROM information_schema.prepared_statements");
            if (result.rows.empty()) {
                writeOutput(theme_.comment + "No prepared statements found." + theme_.reset + "\r\n");
                return;
            }
            writeOutput(theme_.header + "Prepared Statements:" + theme_.reset + "\r\n");
            for (auto& row : result.rows) {
                if (!row.empty()) writeOutput("  " + row[0].to_string() + "\r\n");
            }
        } catch (...) {
            writeOutput(theme_.comment + "No prepared statements found." + theme_.reset + "\r\n");
        }
    }

    void listAllObjects() {
        listTables();
        listViews();
        listIndexes();
        listSequences();
    }

    void describeTable(const std::string& name) {
        try {
            auto schema = db_->catalog().get_table_schema(name);
            if (schema.empty()) {
                writeOutput(theme_.error + "Table '" + name + "' not found." + theme_.reset + "\r\n");
                return;
            }

            writeOutput(theme_.header + "Table: " + name + theme_.reset + "\r\n");

            // Build result for formatting — Schema only provides column names
            tdb::sql::ResultSet result;
            result.columns = {{"Column",""}};

            for (auto& col : schema) {
                tdb::sql::Tuple row;
                row.push_back(tdb::sql::Value::make_string(col.name));
                result.rows.push_back(row);
            }

            ResultFormatter fmt(theme_, terminal_.cols());
            writeOutput(fmt.format(result));

        } catch (const std::exception& e) {
            // Try as a view
            try {
                auto result = db_->execute("SELECT * FROM " + name + " LIMIT 0");
                writeOutput(theme_.header + "View/Table: " + name + theme_.reset + "\r\n");
                if (!result.columns.empty()) {
                    writeOutput("Columns: ");
                    for (size_t i = 0; i < result.columns.size(); ++i) {
                        if (i > 0) writeOutput(", ");
                        writeOutput(result.columns[i].name);
                    }
                    writeOutput("\r\n");
                }
            } catch (const std::exception& e2) {
                writeOutput(theme_.error + "Error: " + e2.what() + theme_.reset + "\r\n");
            }
        }
    }

    void showStats() {
        std::ostringstream out;
        out << "\r\n";
        out << theme_.header << "Database Statistics" << theme_.reset << "\r\n";

        // Path
        out << "  Path:           " << (db_path_.empty() ? "(in-memory)" : db_path_) << "\r\n";

        // Object counts
        try {
            out << "  Tables:         " << db_->catalog().list_tables().size() << "\r\n";
        } catch (...) { out << "  Tables:         0\r\n"; }

        try {
            out << "  Views:          " << db_->catalog().list_views().size() << "\r\n";
        } catch (...) { out << "  Views:          0\r\n"; }

        try {
            out << "  Indexes:        " << db_->catalog().list_indexes().size() << "\r\n";
        } catch (...) { out << "  Indexes:        0\r\n"; }

        try {
            auto seq_result = db_->execute("SELECT COUNT(*) FROM information_schema.sequences");
            size_t seq_count = 0;
            if (!seq_result.rows.empty() && !seq_result.rows[0].empty())
                seq_count = std::stoul(seq_result.rows[0][0].to_string());
            out << "  Sequences:      " << seq_count << "\r\n";
        } catch (...) { out << "  Sequences:      0\r\n"; }

        // Total row count
        size_t total_rows = 0;
        try {
            for (auto& t : db_->catalog().list_tables()) {
                try {
                    auto r = db_->execute("SELECT COUNT(*) FROM " + t);
                    if (!r.rows.empty() && !r.rows[0].empty()) {
                        total_rows += std::stoul(r.rows[0][0].to_string());
                    }
                } catch (...) {}
            }
        } catch (...) {}
        out << "  Total rows:     " << total_rows << "\r\n";

        // Session performance
        out << "\r\n";
        out << theme_.header << "Session Performance" << theme_.reset << "\r\n";
        out << "  Queries:        " << query_count_ << "\r\n";
        out << "  Total time:     " << std::fixed << std::setprecision(2) << total_query_time_ms_ << " ms\r\n";
        if (query_count_ > 0) {
            out << "  Avg time:       " << std::fixed << std::setprecision(2) << (total_query_time_ms_ / query_count_) << " ms\r\n";
        } else {
            out << "  Avg time:       0.00 ms\r\n";
        }
        out << "\r\n";

        writeOutput(out.str());
    }

    void showStatus() {
        std::ostringstream out;
        out << theme_.header << "TDB Status" << theme_.reset << "\r\n";
        out << "  Version:        " << TDB_TUI_VERSION << "\r\n";
        out << "  Database:       " << (db_path_.empty() ? "(in-memory)" : db_path_) << "\r\n";
        out << "  Mode:           " << (lua_mode_ ? "Lua" : "SQL") << "\r\n";
        out << "  Theme:          " << theme_.name << "\r\n";
        out << "  Timing:         " << (timing_ ? "on" : "off") << "\r\n";
        out << "  History:        " << history_.entries().size() << " entries\r\n";
        out << "  Terminal:       " << terminal_.cols() << "x" << terminal_.rows() << "\r\n";
        out << "\r\n";
        writeOutput(out.str());
    }

    void showHistory() {
        auto& entries = history_.entries();
        if (entries.empty()) {
            writeOutput(theme_.comment + "No history." + theme_.reset + "\r\n");
            return;
        }

        // Show last 50 entries
        int start = std::max(0, (int)entries.size() - 50);
        for (int i = start; i < (int)entries.size(); ++i) {
            std::ostringstream out;
            out << theme_.comment << std::setw(5) << (i + 1) << theme_.reset << "  ";
            std::string entry = entries[i];
            // Replace newlines for display
            for (auto& c : entry) {
                if (c == '\n') c = ' ';
            }
            if (entry.size() > (size_t)(terminal_.cols() - 8)) {
                entry = entry.substr(0, terminal_.cols() - 11) + "...";
            }
            out << entry << "\r\n";
            writeOutput(out.str());
        }
    }

    void showFullSchema() {
        try {
            auto tables = db_->catalog().list_tables();
            for (auto& t : tables) {
                describeTable(t);
                writeOutput("\r\n");
            }
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void handleImport(const std::string& args) {
        // Parse: \import csv <file> <table> OR \import sql <file>
        std::istringstream ss(args);
        std::string format, path, table;
        ss >> format >> path;

        if (format.empty() || path.empty()) {
            writeOutput("Usage: \\import csv <file> <table>\r\n");
            writeOutput("       \\import sql <file>\r\n");
            return;
        }

        if (toLower(format) == "csv") {
            ss >> table;
            if (table.empty()) {
                writeOutput(theme_.error + "CSV import requires table name." + theme_.reset + "\r\n");
                return;
            }
            try {
                tdb::migrate::import_csv(db_->catalog(), table, path);
                writeOutput(theme_.success + "Imported CSV into " + table + theme_.reset + "\r\n");
                completer_.refreshDatabaseCompletions();
            } catch (const std::exception& e) {
                writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
            }
        } else if (toLower(format) == "sql") {
            try {
                tdb::migrate::import_sql_dump(*db_, path);
                writeOutput(theme_.success + "Imported SQL from " + path + theme_.reset + "\r\n");
                completer_.refreshDatabaseCompletions();
            } catch (const std::exception& e) {
                writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
            }
        } else {
            writeOutput(theme_.error + "Unknown format: " + format + ". Use 'csv' or 'sql'." + theme_.reset + "\r\n");
        }
    }

    void handleExport(const std::string& args) {
        // Parse: \export csv <table> <file> OR \export sql <file>
        std::istringstream ss(args);
        std::string format, arg1, arg2;
        ss >> format >> arg1 >> arg2;

        if (format.empty() || arg1.empty()) {
            writeOutput("Usage: \\export csv <table> <file>\r\n");
            writeOutput("       \\export sql <file>\r\n");
            return;
        }

        if (toLower(format) == "csv") {
            if (arg2.empty()) {
                writeOutput(theme_.error + "CSV export requires table name and file path." + theme_.reset + "\r\n");
                return;
            }
            try {
                tdb::migrate::export_csv(db_->catalog(), arg1, arg2);
                writeOutput(theme_.success + "Exported " + arg1 + " to " + arg2 + theme_.reset + "\r\n");
            } catch (const std::exception& e) {
                writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
            }
        } else if (toLower(format) == "sql") {
            try {
                tdb::migrate::export_sql_dump(db_->catalog(), arg1);
                writeOutput(theme_.success + "Exported SQL to " + arg1 + theme_.reset + "\r\n");
            } catch (const std::exception& e) {
                writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
            }
        } else {
            writeOutput(theme_.error + "Unknown format: " + format + theme_.reset + "\r\n");
        }
    }

    void handleDump(const std::string& args) {
        std::string path = args.empty() ? "dump.sql" : args;
        try {
            tdb::migrate::export_sql_dump(db_->catalog(), path);
            writeOutput(theme_.success + "Database dumped to " + path + theme_.reset + "\r\n");
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void handlePack(const std::string& args) {
        std::string path = args.empty() ? (db_path_.empty() ? "database.tdb" : db_path_) : args;
        try {
            tdb::dbfile::save(path, db_->catalog());
            writeOutput(theme_.success + "Database packed to " + path + theme_.reset + "\r\n");
        } catch (const std::exception& e) {
            writeOutput(theme_.error + "Error: " + e.what() + theme_.reset + "\r\n");
        }
    }

    void handleExecFile(const std::string& args) {
        if (args.empty()) {
            writeOutput(theme_.error + "Usage: \\exec <file.sql>" + theme_.reset + "\r\n");
            return;
        }

        std::ifstream f(args);
        if (!f.is_open()) {
            writeOutput(theme_.error + "Cannot open file: " + args + theme_.reset + "\r\n");
            return;
        }

        std::ostringstream ss;
        ss << f.rdbuf();
        std::string sql = ss.str();

        if (sql.empty()) {
            writeOutput(theme_.comment + "File is empty." + theme_.reset + "\r\n");
            return;
        }

        writeOutput(theme_.comment + "Executing " + args + "..." + theme_.reset + "\r\n");
        executeSQL(sql);
    }

    void handleBrowse(const std::string& args) {
        std::istringstream ss(args);
        std::string table;
        int limit = 20;
        ss >> table;
        if (ss >> limit) {
            // user provided limit
        }

        std::string sql = "SELECT * FROM " + table + " LIMIT " + std::to_string(limit);
        executeSQL(sql);
    }

    // ----- Banner -----
    void printBanner() {
        std::ostringstream out;
        out << theme_.prompt;
        out << " _____ ____  ____  \r\n";
        out << "|_   _|  _ \\| __ ) \r\n";
        out << "  | | | | | |  _ \\ \r\n";
        out << "  | | | |_| | |_) |\r\n";
        out << "  |_| |____/|____/ \r\n";
        out << theme_.reset;
        out << "\r\n";
        out << theme_.function << "TDB SQL Database Shell" << theme_.reset;
        out << " v" << TDB_TUI_VERSION << "\r\n";
        out << theme_.comment << "Type \\help for help, \\quit to exit." << theme_.reset << "\r\n";
        out << theme_.comment << "SQL statements end with ; — multi-line supported." << theme_.reset << "\r\n";
        out << "\r\n";
        writeOutput(out.str());
    }

    void printHelp() {
        std::ostringstream out;
        out << "\r\n";
        out << theme_.header << "TDB Shell Commands" << theme_.reset << "\r\n";
        out << "\r\n";
        out << theme_.keyword << "  General:" << theme_.reset << "\r\n";
        out << "    \\help, \\h, \\?       Show this help\r\n";
        out << "    \\quit, \\exit, \\q    Exit the shell\r\n";
        out << "    \\clear, \\cls        Clear the screen\r\n";
        out << "    \\timing             Toggle query timing display\r\n";
        out << "    \\history            Show command history\r\n";
        out << "    \\theme <name>       Switch theme (dark, light)\r\n";
        out << "\r\n";
        out << theme_.keyword << "  Database:" << theme_.reset << "\r\n";
        out << "    \\open <path>        Open a database file\r\n";
        out << "    \\close              Close current database\r\n";
        out << "    \\save [path]        Save database to file\r\n";
        out << "    \\reset              Reset to empty in-memory database\r\n";
        out << "    \\status, \\info      Show database status\r\n";
        out << "    \\stats              Show detailed statistics\r\n";
        out << "\r\n";
        out << theme_.keyword << "  Schema Browser:" << theme_.reset << "\r\n";
        out << "    \\dt                 List tables\r\n";
        out << "    \\dv                 List views\r\n";
        out << "    \\di                 List indexes\r\n";
        out << "    \\ds                 List sequences\r\n";
        out << "    \\dm                 List materialized views\r\n";
        out << "    \\dp                 List prepared statements\r\n";
        out << "    \\d                  List all objects\r\n";
        out << "    \\d <table>          Describe a table's schema\r\n";
        out << "    \\schema [table]     Show full schema or describe table\r\n";
        out << "    \\browse <table> [n] Browse table data (default 20 rows)\r\n";
        out << "\r\n";
        out << theme_.keyword << "  Data Transfer:" << theme_.reset << "\r\n";
        out << "    \\import csv <file> <table>   Import CSV into table\r\n";
        out << "    \\import sql <file>           Execute SQL file\r\n";
        out << "    \\export csv <table> <file>   Export table as CSV\r\n";
        out << "    \\export sql <file>           Export database as SQL\r\n";
        out << "    \\dump [file]                 Dump database to SQL file\r\n";
        out << "    \\pack [file]                 Pack database to binary file\r\n";
        out << "    \\exec <file>                 Execute SQL from file\r\n";
        out << "\r\n";
        out << theme_.keyword << "  Modes:" << theme_.reset << "\r\n";
        out << "    \\lua                Enter Lua REPL mode\r\n";
        out << "    \\sql                Return to SQL mode\r\n";
        out << "    \\explain <sql>      Show query execution plan\r\n";
        out << "\r\n";
        out << theme_.keyword << "  Keyboard Shortcuts:" << theme_.reset << "\r\n";
        out << "    Ctrl+A / Home       Move to start of line\r\n";
        out << "    Ctrl+E / End        Move to end of line\r\n";
        out << "    Ctrl+K              Kill text to end of line\r\n";
        out << "    Ctrl+U              Kill text to start of line\r\n";
        out << "    Ctrl+W              Kill previous word\r\n";
        out << "    Ctrl+L              Clear screen\r\n";
        out << "    Ctrl+C              Cancel current input\r\n";
        out << "    Ctrl+D              Exit (on empty line)\r\n";
        out << "    Tab                 Auto-complete\r\n";
        out << "    Up/Down             Navigate history\r\n";
        out << "\r\n";

        writeOutput(out.str());
    }

    void printUsage() {
        printf("Usage: tdb-shell [OPTIONS] [database]\n\n");
        printf("Options:\n");
        printf("  -h, --help       Show this help\n");
        printf("  -v, --version    Show version\n");
        printf("  -e, --exec SQL   Execute SQL and exit\n");
        printf("  --dark           Use dark theme (default)\n");
        printf("  --light          Use light theme\n");
        printf("\nExamples:\n");
        printf("  tdb-shell                      Start with in-memory database\n");
        printf("  tdb-shell mydb.tdb             Open existing database\n");
        printf("  tdb-shell -e 'SELECT 1+1'      Execute SQL and exit\n");
    }

    // ----- Members -----
    Terminal terminal_;
    LineEditor editor_;
    History history_;
    Completer completer_;
    ColorTheme theme_;

    bool running_;
    bool timing_;
    bool lua_mode_;
    int query_count_;
    double total_query_time_ms_;
    double last_query_time_ms_;
    int last_row_count_;

    std::string db_path_;
    std::string multi_line_buffer_;

    // Completion menu state
    bool show_completion_menu_;
    int completion_menu_idx_;
    std::vector<std::string> completion_matches_;
    std::string completion_prefix_;
    size_t completion_prefix_start_;

    tdb::Database* db_;
};

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    TuiShell shell;
    return shell.run(argc, argv);
}
