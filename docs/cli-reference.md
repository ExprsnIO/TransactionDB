# CLI Reference

TDB ships with two command-line tools: `tdbcli` (simple readline shell) and `tdb-tui` (rich terminal UI).

## tdbcli

### Usage

```
tdbcli                        Interactive mode, in-memory database
tdbcli <path>                 Interactive mode, named database (auto-saves)
tdbcli -e "SQL"               Execute single statement and exit
tdbcli -f script.sql          Execute SQL file and exit
tdbcli --pack <src> <out>     Pack database into .tdb v2 file
tdbcli --no-color             Disable ANSI colors
tdbcli --version              Show version
tdbcli --help                 Show help
```

### Interactive Commands

| Command | Description |
|---------|-------------|
| `\q`, `\quit`, `exit` | Quit |
| `\dt` | List tables |
| `\dv` | List views |
| `\di` | List indexes |
| `\ds` | List sequences |
| `\d <table>` | Describe table columns |
| `\timing` | Toggle query timing display |
| `\import <file> <table>` | Import CSV into table |
| `\export <table> <file>` | Export table to CSV |
| `\save [file]` | Save database to file |
| `\pack <file> [--force]` | Pack into .tdb v2 format |
| `\dump [file]` | Export database as SQL dump |
| `\status` | Show database info (tables, rows) |
| `\clear` | Clear screen |
| `\help` | Show help |

### File Mode

The `-f` flag executes SQL files with special handling:
- **PL/SQL block tracking** -- `AS BEGIN...END` blocks are accumulated across lines without splitting on internal semicolons
- **String-literal awareness** -- semicolons inside quoted strings don't split statements
- **Comment skipping** -- lines starting with `--` are ignored

### Output Format

Results are displayed in aligned table format:

```
name  | age | city
------+-----+--------
Alice | 30  | NYC
Bob   | 25  | LA
(2 rows)
```

DML operations show affected rows:
```
3 rows affected
```

---

## tdb-tui

The rich TUI shell provides syntax highlighting, tab completion, and a status bar.

### Features

- **Syntax highlighting** -- SQL keywords (purple), strings (green), numbers (blue), comments (gray)
- **Tab completion** -- keywords, function names, table names, column names
- **Command history** -- persistent across sessions (`~/.tdb_history`)
- **Multi-line SQL** -- automatic continuation for incomplete statements
- **Status bar** -- shows database path, table count, timing
- **Themes** -- `\theme dark` or `\theme light`

### Commands

All `tdbcli` commands plus:

| Command | Description |
|---------|-------------|
| `\schema [table]` | Show full schema or table details |
| `\browse <table> [limit]` | Browse table data interactively |
| `\exec <file>` | Execute SQL file |
| `\open <path>` | Open database |
| `\close` | Close current database |
| `\reset` | Reset database (drop all objects) |
| `\explain <SQL>` | Show query plan |
| `\stats` | Show statistics (function counts, timings) |
| `\info` | Show database info |
| `\history` | Show command history |
| `\lua` | Enter Lua REPL mode |
| `\sql` | Return to SQL mode (from Lua) |
| `\theme dark\|light` | Switch color theme |

### Lua REPL

Enter Lua mode with `\lua`:

```
tdb> \lua
Entered Lua REPL mode. Type \sql to return to SQL mode.
lua> rows = db.query("SELECT * FROM users")
lua> for _, row in ipairs(rows) do print(row.name) end
lua> db.execute("INSERT INTO logs (msg) VALUES ('test')")
lua> db.now()
lua> db.user()
lua> \sql
tdb>
```

Available Lua functions:
- `db.execute(sql)` -- Execute SQL, returns rows_affected
- `db.query(sql)` -- Execute SQL, returns array of row tables
- `db.now()` -- Current time in milliseconds
- `db.user()` -- Current user name
- `db.log(msg)` -- Log a message
