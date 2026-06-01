# SQL Compatibility Matrix: TransactionDB vs SQLite

TransactionDB (`tdb`) deliberately mirrors SQLite's *C API* ergonomics
(`open`/`prepare`/`step`/`bind`/`column`/`finalize`), but its *SQL dialect* is an
independent, still-growing subset with some deliberate divergences and a few
extensions SQLite does not have. This document maps the SQL surface against SQLite
so you know what ports cleanly and what does not.

> **Status:** TransactionDB is early-development (0.1.0). This matrix reflects the
> engine as it currently behaves, verified by running queries through `tdb_shell`
> and by reading `src/sql/`. Expect the surface to grow.

## How to read this

| Mark | Meaning |
| ---- | ------- |
| ✅ | Supported and behaves like SQLite (or close enough to port without thinking about it). |
| ⚠️ | Partial — parsed but **not enforced/executed**, or only a subset works. Read the note. |
| ❌ | Not supported (parse error, or silently misinterpreted). |
| ➕ | **Extension** — TransactionDB does this; SQLite does not. See [Extensions](#extensions-beyond-sqlite). |

Each row was checked empirically against the built `tdb_shell` and cross-referenced
with the parser/executor sources. "Parsed but not enforced" is called out explicitly
because such constructs **succeed silently** — a portability trap worth knowing about.

---

## The one big difference: typing model

This is the most important divergence and it is not a row in a table.

**SQLite is dynamically typed** with *type affinity*: a column type is a hint, and a
column can hold a value of any storage class. Inserting `'hello'` into an `INTEGER`
column succeeds and stores the text.

**TransactionDB is strictly typed.** Column declarations are enforced at write time:

```sql
CREATE TABLE t(a INTEGER);
INSERT INTO t VALUES('hello');   -- Error: column a: not an integer
INSERT INTO t VALUES(3.7);       -- Error: column a: real has fractional part
INSERT INTO t VALUES('123');     -- OK -> stored as integer 123 (lossless coercion)
```

Lossless string→number coercion is allowed; lossy or nonsensical conversions are
rejected with `TDB_MISMATCH`. Code written against SQLite's permissive typing may hit
errors here. Conversely, schemas are more self-enforcing.

`typeof()` reports the storage class (`integer`/`real`/`text`/`blob`/`null`), and
`BOOLEAN`/`TRUE`/`FALSE` collapse to integer `1`/`0` as in SQLite.

---

## Statements

| Statement | tdb | SQLite | Notes |
| --------- | :-: | :----: | ----- |
| `SELECT` | ✅ | ✅ | Full clause set (see below). |
| `INSERT ... VALUES` | ✅ | ✅ | Multi-row `VALUES (...),(...)` supported. |
| `INSERT ... SELECT` | ✅ | ✅ | |
| `UPDATE ... SET ... WHERE` | ✅ | ✅ | |
| `DELETE ... WHERE` | ✅ | ✅ | |
| `CREATE TABLE` | ✅ | ✅ | `IF NOT EXISTS`, `TEMP`/`TEMPORARY`. |
| `CREATE TABLE ... AS SELECT` | ✅ | ✅ | Schema is derived from the SELECT projection. |
| `DROP TABLE` | ✅ | ✅ | `IF EXISTS`. |
| `ALTER TABLE` | ✅ | ✅ | `ADD COLUMN`, `DROP COLUMN`, `RENAME COLUMN`, `RENAME TO`. |
| `CREATE INDEX` / `DROP INDEX` | ✅ | ✅ | `UNIQUE`, `IF [NOT] EXISTS`, `USING BTREE/RTREE/...`. |
| `CREATE VIEW` / `DROP VIEW` | ✅ | ✅ | Selecting through a view works. |
| `CREATE MATERIALIZED VIEW` | ➕ | ❌ | Parsed/flagged; SQLite has no materialized views. |
| `BEGIN` / `COMMIT` / `ROLLBACK` | ✅ | ✅ | `BEGIN [TRANSACTION]`. |
| `SAVEPOINT` / `RELEASE` / `ROLLBACK TO` | ✅ | ✅ | Nested savepoint rollback verified. |
| `EXPLAIN` | ⚠️ | ✅ | Prints a textual plan (e.g. `SCAN t`). Format is tdb's own, **not** SQLite's opcode listing. |
| `EXPLAIN QUERY PLAN` | ⚠️ | ✅ | Accepted; behaves like bare `EXPLAIN` (tdb plan format). |
| `VACUUM` | ⚠️ | ✅ | Accepted; compaction semantics differ from SQLite. |
| `PRAGMA ...` | ❌ | ✅ | Unrecognized statement. No pragmas at all. |
| `ATTACH` / `DETACH` | ❌ | ✅ | Unrecognized statement. |
| `CREATE TRIGGER` / `DROP TRIGGER` | ❌ | ✅ | `expected TABLE`. |
| `CREATE FUNCTION` / `PROCEDURE`, `CALL` | ➕⚠️ | ❌ | Stored routines, **Lua-backed** (`LANGUAGE LUA`); only when built with `TDB_BUILD_LUA=ON`. |
| `PREPARE p AS ...` (SQL-level) | ⚠️ | ❌ | Parsed; `statement type not yet executable`. (Note: API-level prepared statements work fully.) |
| `TRUNCATE` | ❌ | ❌ | Neither engine has it (use `DELETE`). |

---

## Data types

TransactionDB has a **rich, strict type system** — a broader catalog of declared
types than SQLite's five storage classes, but enforced rather than advisory.

| Type | tdb | SQLite | Notes |
| ---- | :-: | :----: | ----- |
| `INTEGER` / `INT` / `BIGINT` / `SMALLINT` / `TINYINT` | ✅ | ✅ (`INTEGER` affinity) | `INTEGER PRIMARY KEY` aliases the rowid, as in SQLite. |
| `REAL` / `DOUBLE` / `FLOAT` | ✅ | ✅ (`REAL` affinity) | |
| `DECIMAL(p,s)` / `NUMERIC(p,s)` | ✅ | ⚠️ (`NUMERIC` affinity, no real precision) | tdb tracks precision/scale. |
| `TEXT` / `CLOB` / `CHAR(n)` / `VARCHAR(n)` | ✅ | ✅ (`TEXT` affinity) | tdb enforces length bounds. |
| `BLOB` / `BINARY(n)` / `VARBINARY(n)` | ✅ | ✅ | Including `x'AABB'` literals. |
| `BOOLEAN` / `BOOL` | ✅ | ⚠️ (stored as int) | `TRUE`/`FALSE` → `1`/`0`. |
| `DATE` / `TIME` / `TIMESTAMP` / `DATETIME` | ⚠️ | ⚠️ | Type declarable & storable, but **no date/time functions** (see below). |
| `JSON` | ⚠️➕ | ⚠️ (`JSON1` ext) | Type exists; **no JSON functions** (`json_extract`, etc.). |
| `UUID` | ➕ | ❌ | Declared type. |
| `GEOMETRY` / `GEOGRAPHY` / `POINT` | ➕ | ❌ | Spatial types with `ST_*` functions. |
| Blob literal `x'AABB'` | ✅ | ✅ | Decoded to bytes at parse time. |

---

## SELECT clauses & set operations

| Feature | tdb | SQLite | Notes |
| ------- | :-: | :----: | ----- |
| Projection, `*`, column aliases (`AS`) | ✅ | ✅ | |
| `WHERE` | ✅ | ✅ | |
| `GROUP BY` / `HAVING` | ✅ | ✅ | |
| `ORDER BY` (multi-key, `ASC`/`DESC`) | ✅ | ✅ | |
| `LIMIT` / `OFFSET` | ✅ | ✅ | `LIMIT -1 OFFSET n` works (no limit, skip n). |
| `DISTINCT` | ✅ | ✅ | Incl. `COUNT(DISTINCT x)`. |
| `UNION` / `UNION ALL` | ✅ | ✅ | |
| `INTERSECT` / `EXCEPT` | ✅ | ✅ | |
| Scalar / `IN` / `EXISTS` subqueries | ✅ | ✅ | |
| Correlated subqueries | ✅ | ✅ | Re-run per outer row. |
| Subqueries in `FROM` (derived tables) | ✅ | ✅ | Require an alias. |
| CTEs — `WITH name AS (...)` | ✅ | ✅ | Non-recursive. |
| `WITH RECURSIVE` | ❌ | ✅ | Keyword parsed; recursion blocked at runtime (`no such table`). |
| Window functions (`OVER`, `PARTITION BY`, `ROW_NUMBER`, …) | ❌ | ✅ | No window syntax at all. |
| `VALUES` as a standalone row source | ⚠️ | ✅ | `VALUES` works inside `INSERT`; not as a top-level table. |

### Joins

| Join | tdb | SQLite | Notes |
| ---- | :-: | :----: | ----- |
| `INNER JOIN` / `JOIN ... ON` | ✅ | ✅ | Nested-loop; index-seeked when ON is an equijoin on an indexed column. |
| Comma / `CROSS JOIN` | ✅ | ✅ | Cartesian product. |
| `LEFT [OUTER] JOIN` | ✅ | ✅ | Unmatched right side → `NULL` (verified). |
| `RIGHT [OUTER] JOIN` | ✅ | ❌ | tdb supports it; **SQLite historically did not** (added only in 3.39+). |
| `FULL [OUTER] JOIN` | ✅ | ❌ | tdb supports it; **SQLite only since 3.39**. |
| `JOIN ... USING (col)` | ✅ | ✅ | Desugared to an equi-`ON` at parse time. |
| `NATURAL JOIN` | ✅ | ✅ | Join condition synthesized from shared column names. |

---

## Operators & expressions

| Feature | tdb | SQLite | Notes |
| ------- | :-: | :----: | ----- |
| Arithmetic `+ - * / %` | ✅ | ✅ | |
| Comparison `= != <> < <= > >=` | ✅ | ✅ | |
| `AND` / `OR` / `NOT` | ✅ | ✅ | |
| `IS NULL` / `IS NOT NULL` | ✅ | ✅ | `NULL = NULL` is NULL (unknown), as expected. |
| `LIKE` / `NOT LIKE` / `ESCAPE` | ✅ | ✅ | `%`, `_` wildcards. |
| `GLOB` | ✅ | ✅ | Case-sensitive `*`, `?`, `[...]`. |
| `IN (...)` / `NOT IN (...)` | ✅ | ✅ | List or subquery. |
| `BETWEEN` / `NOT BETWEEN` | ✅ | ✅ | |
| String concat `||` | ✅ | ✅ | |
| `CASE WHEN ... THEN ... ELSE ... END` | ✅ | ✅ | Simple and searched. |
| `CAST(x AS type)` | ✅ | ✅ | Uses tdb's strict coercion rules. |
| Bitwise `& | ~ << >>` | ✅ | ✅ | |
| `COLLATE` in expressions | ❌ | ✅ | `COLLATE` parsed only in column defs; `expr COLLATE NOCASE` errors. |

---

## Functions

### Aggregate functions

| Function | tdb | SQLite | Notes |
| -------- | :-: | :----: | ----- |
| `COUNT(*)`, `COUNT(expr)`, `COUNT(DISTINCT)` | ✅ | ✅ | |
| `SUM` / `AVG` / `MIN` / `MAX` | ✅ | ✅ | |
| `TOTAL` | ✅ | ✅ | Returns `0.0` (not NULL) over no rows. |
| `GROUP_CONCAT(x[, sep])` | ✅ | ✅ | Default separator is `,`. |

### Scalar functions

| Function | tdb | SQLite | Notes |
| -------- | :-: | :----: | ----- |
| `LENGTH`, `UPPER`, `LOWER` | ✅ | ✅ | |
| `SUBSTR(s,pos[,len])` | ✅ | ✅ | |
| `TRIM` / `LTRIM` / `RTRIM` | ✅ | ✅ | 1- or 2-arg form (custom trim chars in the 2nd). |
| `REPLACE(s,from,to)` | ✅ | ✅ | |
| `INSTR(s,sub)` | ✅ | ✅ | 1-based. |
| `ABS`, `ROUND(x[,n])` | ✅ | ✅ | |
| `COALESCE`, `IFNULL`, `NULLIF` | ✅ | ✅ | |
| `TYPEOF` | ✅ | ✅ | |
| `MIN(a,b,...)` / `MAX(a,b,...)` (scalar form) | ✅ | ✅ | Multi-arg scalar variant. |
| `HEX`, `UNHEX` | ✅ | ✅ | From the built-in crypto suite. |
| `QUOTE`, `CHAR`, `UNICODE`, `RANDOM`, `RANDOMBLOB` | ✅ | ✅ | |
| `PRINTF`/`FORMAT` | ✅ | ✅ | `%d %u %x %o %f %g %e %s %c %q %Q %%` + width/precision. |
| `LAST_INSERT_ROWID`, `CHANGES`, `TOTAL_CHANGES`, `SQLITE_VERSION` | ✅ | ✅ | Also exposed as `tdb_last_insert_rowid()` / `tdb_changes()` / `tdb_total_changes()` in the C API. |
| Date/time: `DATE`, `TIME`, `DATETIME`, `STRFTIME`, `JULIANDAY`, `UNIXEPOCH` | ✅ | ✅ | `'now'`, ISO timestamps, JD, and `+/-N day|hour|minute|second|month|year` / `start of day|month|year` / `unixepoch` modifiers. |
| `CURRENT_DATE`, `CURRENT_TIME`, `CURRENT_TIMESTAMP` (as functions) | ✅ | ✅ | |
| JSON: `json_object`, `json_array`, `json_extract`, `json_type`, `json_valid`, `json_array_length`, `json` | ✅ | ⚠️ (`JSON1`) | Minimal but functional. `$.path` / `$[idx]` accessors. JSON-typed text composes cleanly. |

---

## Constraints

This is a sharp-edges section: several constraints **parse and accept rows that
violate them** — they are recognized syntactically but not yet enforced.

| Constraint | tdb | SQLite | Notes |
| ---------- | :-: | :----: | ----- |
| `PRIMARY KEY` (single & composite) | ✅ | ✅ | `INTEGER PRIMARY KEY` ↔ rowid. |
| `NOT NULL` | ✅ | ✅ | **Enforced** (`NOT NULL constraint failed`). |
| `AUTOINCREMENT` | ✅ | ✅ | Monotonic ids verified. |
| `DEFAULT` | ✅ | ✅ | Applied for columns omitted in `INSERT`. |
| `UNIQUE` | ⚠️ | ✅ | Parsed but **not enforced** for non-PK columns. |
| `CHECK (...)` | ✅ | ✅ | Evaluated on INSERT/UPDATE; failure raises `CHECK constraint failed`. |
| `FOREIGN KEY` / `REFERENCES` | ⚠️ | ✅ (opt-in) | Parsed but **no referential integrity** — orphan rows accepted. |
| `COLLATE` (in column def) | ⚠️ | ✅ | `BINARY`/`NOCASE` parsed; not usable in expressions. |
| Named `CONSTRAINT` | ⚠️ | ✅ | Captured, not fully used. |
| `GENERATED ALWAYS AS (...) STORED` | ✅ | ✅ | Computed column verified (`a*2`). `VIRTUAL` also parsed. |

> **Portability note:** because `UNIQUE`, `CHECK`, `DEFAULT`, and `FOREIGN KEY` are
> accepted but not enforced/applied, a schema that *relies* on them for correctness
> behaves differently than under SQLite. Don't assume the database is guarding these
> invariants yet.

---

## Parameters & DML extras

| Feature | tdb | SQLite | Notes |
| ------- | :-: | :----: | ----- |
| Positional `?` | ✅ | ✅ | 1-based binding. |
| Numbered `?N` | ✅ | ✅ | |
| Named `:name` | ✅ | ✅ | |
| Named `$name` / `$N` | ✅➕ | ⚠️ | PostgreSQL-style; SQLite accepts `$` but tdb leans into it. |
| Named `@name` | ✅ | ✅ | Accepted by the lexer as a parameter. |
| `RETURNING (...)` on INSERT/UPDATE/DELETE | ✅ | ✅ (3.35+) | Verified. |
| `INSERT OR REPLACE` / `OR IGNORE` | ✅ | ✅ | |
| `ON CONFLICT (col) DO NOTHING` | ✅ | ✅ | |
| `ON CONFLICT (col) DO UPDATE SET ...` | ✅ | ✅ | UPSERT verified; optional `WHERE`. |
| Quoted identifiers `"x"`, `` `x` ``, `[x]` | ✅ | ✅ | All three styles. |

---

## Transactions & isolation

| Feature | tdb | SQLite | Notes |
| ------- | :-: | :----: | ----- |
| `BEGIN`/`COMMIT`/`ROLLBACK` | ✅ | ✅ | |
| Savepoints | ✅ | ✅ | |
| Concurrency model | ➕ | — | **MVCC**: single writer + many concurrent snapshot readers. SQLite uses database-level locking (WAL allows 1 writer + readers, but no per-row MVCC). |
| Isolation levels | ➕ | ❌ | `READ COMMITTED`, `SNAPSHOT` (default), `SERIALIZABLE`. SQLite has no `SET TRANSACTION ISOLATION`. |

---

## Extensions beyond SQLite

Features TransactionDB has that SQLite does not:

- **MVCC concurrency** with selectable isolation levels (`SNAPSHOT` default).
- **`RIGHT`/`FULL OUTER JOIN`** (SQLite only gained these in 3.39).
- **Spatial type system + PostGIS-flavored functions:** `GEOMETRY`/`GEOGRAPHY`/`POINT`
  types and `ST_GeomFromText`, `ST_Point`, `ST_AsText`, `ST_X`/`ST_Y`,
  `ST_Distance`, `ST_DWithin`, `ST_Contains`/`ST_Within`, `ST_Intersects`, plus
  `USING RTREE/GIST/SPATIAL` indexes.
- **`CREATE MATERIALIZED VIEW`.**
- **Stored routines** — `CREATE FUNCTION`/`PROCEDURE ... LANGUAGE LUA`, invoked with
  `CALL` or as scalar functions (requires `TDB_BUILD_LUA=ON`).
- **PostgreSQL-style syntax** — `$name`/`$N` parameters and dollar-quoted strings
  (`$$ ... $$`, `$tag$ ... $tag$`).
- **SQL:2011 system-versioned tables** — `WITH SYSTEM VERSIONING`, `PERIOD FOR
  SYSTEM_TIME`, and `SELECT ... FOR SYSTEM_TIME AS OF <version>` (temporal querying;
  `FOR SYSTEM_TIME ALL` not yet accepted).
- **Strict declared typing** with a wider type catalog (`DECIMAL(p,s)`, `UUID`,
  bounded `VARCHAR`/`CHAR`, `BINARY`/`VARBINARY`, etc.).
- **`current_version()`** — the engine's transaction/version clock.

---

## Quick portability checklist (SQLite → TransactionDB)

Things most likely to bite when porting SQLite SQL:

1. **Strict typing.** Rows that violate declared column types are rejected, not coerced.
2. **Partially enforced constraints.** `NOT NULL`, `CHECK`, and `DEFAULT` are now
   enforced. `UNIQUE` (on non-PK columns) and `FOREIGN KEY` are still parsed but
   not yet enforced.
3. **No `PRAGMA`, `ATTACH`, triggers, window functions, or recursive CTEs.**
4. **`EXPLAIN`** prints tdb's own plan format, not SQLite opcodes. `EXPLAIN
   QUERY PLAN` is accepted but uses the same format.

---

*Verified against `tdb_shell` (built from this tree) and `src/sql/`
(`tdb_lexer.c`, `tdb_parser.c`, `tdb_ast.h`, `tdb_exec.c`,
`tdb_exec_dml.inc`, `tdb_exec_select.inc`, `tdb_exec_stream.inc`) and
`src/value/tdb_sqltype.c`. As the engine is pre-1.0, treat ⚠️ rows as
moving targets.*
