# TDB Roadmap — Version 2.00

Status: draft, May 2026
Owner: Rick Holland
Target window: 6–9 months from kickoff (revised upward from the original 3–6
month focused scope after adding Tracks 3 and 4 — see §2 note)

---

## 1. Framing

v1.x got TDB to ~37K lines and a working SQL surface across catalog, indexes,
PL/SQL, scripting, and persistence. v2.00 is the consolidation release: pay
down the architectural debt that v1 accumulated, then turn the freed surface
area into measurable performance wins. Distribution and the remaining SQL
completeness gaps are tracked but explicitly deferred so v2.00 stays shippable
in 3–6 months.

### Constraints (hard)

- **Single-binary / embedded.** `tdbcli` stays the primary surface. No required
  external services. No daemons. Library embedding remains a first-class use case.
- **Backward-compatible on-disk format.** Every v1.x `.tdb` file opens cleanly
  in v2.00. Format evolution is allowed only behind a version byte plus an
  in-place migration that runs on first open.
- **No new mandatory dependencies.** LLVM, JSC, zstd, OpenSSL, ICU stay
  optional, as today. Anything new ships behind a `TDB_WITH_*` flag.

### Non-goals for v2.00

- Distributed execution, replication, consensus, sharding. Tracked for v3.0.
- Multi-tenant / network server mode.
- Cloud-native storage (S3 page store, etc.).
- Rewriting the script engines.

---

## 2. Themes & priority

| # | Theme | Status in v2.00 | Why |
|---|-------|-----------------|-----|
| 1 | **Architecture refactor** | Primary | `include/tdb/database.h` is 6,600 lines and the choke point for every other change. Force multiplier for everything below. |
| 2 | **Performance & scale** | Primary | Visible user value. Unblocked once the executor is extractable from `database.h`. |
| 3 | **SQL expressiveness — tables, functions, procedures** | Primary | Computed columns, table-valued functions, richer aggregates/scalars, PL/SQL extensions. Lands cleanly on top of the modular executor and DML split. |
| 4 | **Lua full interop** | Primary | Today Lua sees only 6 of 22 `sql::Value` types and can't introspect the catalog or define SQL functions. Closing the gap makes Lua a real extension language. |
| 5 | **SQL completeness fixes** | Opportunistic | Known limitations that fall out naturally during the refactor (FULL/CROSS JOIN, FK in encrypted tables, PL/SQL block parsing). |
| 6 | **Distribution & HA** | Deferred to v3.0 | Conflicts directly with the single-binary/embedded constraint. Needs its own release cycle. |

The 1+2 pairing is the foundation. Doing perf work on top of the current
monolith means every change touches a 6,600-line header; doing the refactor
without a perf payoff makes it look like a cosmetic re-org. Sequencing them
in one release makes both defensible.

**Scope note.** You originally asked for a focused 1–2 theme release. Tracks 3
and 4 push the scope to four primary themes, which is why the window extends
to 6–9 months. The alternative is to ship 1+2 as v2.00 and roll 3+4 into a
v2.5 release six months later. The current draft assumes the wider scope;
flag it if you'd rather cut.

---

## 3. Track 1 — Architecture refactor

### Goal

Break `database.h` into a layered set of translation units with explicit
interfaces, so that:

- A change to one operator does not force a recompile of the world.
- The executor, optimizer, and catalog can be unit-tested without booting a
  full `Database`.
- New SQL features can land in a single file instead of threading through a
  monolithic header.

### Concrete moves

1. **Split `database.h`** (currently ~6,600 lines, ~50 statement types and
   ~200 scalar functions) into:
   - `database/core.{h,cpp}` — lifecycle, txn state, connection state.
   - `database/exec_ddl.cpp` — CREATE/ALTER/DROP for every object type.
   - `database/exec_dml.cpp` — INSERT/UPDATE/DELETE/MERGE, constraint checks,
     trigger firing, RETURNING.
   - `database/exec_query.cpp` — SELECT pipeline: scan, join, group, window,
     order, limit.
   - `database/expr/` — `eval_expr_with_row()` split by family
     (`expr_string.cpp`, `expr_numeric.cpp`, `expr_datetime.cpp`,
     `expr_json.cpp`, `expr_cast.cpp`, …).
   - `database/aggregate.cpp` — `compute_aggregate()` and window aggregates.
   - `database/plsql/` — interpreter, tokenizer, variable substitution.
2. **Extract a `QueryExecutor` interface.** Today `Database` *is* the executor.
   Pull a `class QueryExecutor` out so the optimizer/planner can target it
   without pulling in catalog mutation.
3. **Introduce a real test directory.** `CMakeLists.txt` already references
   `tests/` with `TDB_BUILD_TESTS=ON`, but the directory does not exist.
   Create it with at least one test binary per extracted module. This is the
   refactor's safety net.
4. **Document the dependency graph** in `docs/architecture.md` after each
   split, so the layering does not silently regress.

### Success criteria

- No `.cpp` or `.h` over 1,500 lines in `src/` or `include/tdb/` (excluding
  generated and vendored code).
- `tests/` exists, runs under CTest, and exercises every extracted module.
- Full clean build time on a single core drops by ≥30%.
- Incremental build after a one-function change touches ≤3 translation units.
- All v1.x `.tdb` files still open, and the existing examples
  (`examples/erp_sample.sql`, `erp_sample.tdb`) still run end-to-end.

---

## 4. Track 2 — Performance & scale

### Goal

Measurable, advertised performance gains on the workloads users actually run:
analytical scans, joins on indexed columns, and aggregate queries against
columnar tables.

### Concrete moves

1. **Vectorized expression evaluation.** Today `eval_expr_with_row()` is
   row-at-a-time. Add a batched path that evaluates an expression over a
   column of values at once. Start with arithmetic, comparison, and the top-20
   scalar functions by usage in the existing test corpus.
2. **Planner-driven join ordering.** Replace the current left-deep, written-order
   join evaluation with a cost-based reorderer for ≤8-relation queries, using
   table cardinalities from the catalog plus existing index stats.
3. **Push predicates and projections into columnar scans.** `TableInfo` already
   has a `ColumnStore` path; the executor currently materializes to rows and
   then filters. Filter and project inside the column store and return only
   the needed columns.
4. **Parallel scan + parallel aggregate.** Single-process, thread-pool based.
   Partition table or columnar scans by page range; merge partial aggregates.
   Opt-in via session setting; off by default to preserve embedded semantics.
5. **Optional LLVM expression JIT.** `TDB_WITH_LLVM` already exists for Lua;
   reuse the `IREmitter` to JIT hot SQL expressions. Strictly behind the
   existing optional dep flag.

### Success criteria

A v2.00 perf suite (added under `tests/perf/`) compares against a tagged v1.x
build on the same hardware:

- TPC-H–style Q1 (scan + group + agg) on a 10M-row columnar table: **≥3×** faster.
- Indexed join across two 1M-row tables: **≥1.5×** faster.
- Mixed scalar-function expression sheet (200 functions × 1M rows): **≥2×** faster
  with vectorization on.
- No regression >5% on any query in the v1.x example corpus.

---

## 5. Track 3 — SQL expressiveness (tables, functions, procedures)

### Goal

Make tables, expressions, and procedural code do meaningfully more without
dropping into Lua. The current surface is broad but shallow — many statement
types, but limited composition. This track deepens composition.

### Concrete moves

1. **Computed / generated columns.**
   - `GENERATED ALWAYS AS (<expr>) VIRTUAL` — recomputed on read.
   - `GENERATED ALWAYS AS (<expr>) STORED` — materialized on write, persisted,
     re-evaluated on UPDATE of any input column.
   - Indexable in both forms; participate in partition pruning when STORED.
2. **Table-valued functions (TVFs).**
   - SQL-bodied: `CREATE FUNCTION f(...) RETURNS TABLE(...) AS $$ SELECT ... $$`.
   - PL/SQL-bodied with `PIPE ROW(...)` for streaming results.
   - Lua-bodied — see Track 4.
   - Callable in any `FROM` clause; correlated invocation via `LATERAL`.
3. **`LATERAL` subqueries and `LATERAL` TVF calls.** Cleans up a class of
   queries that today require manually unnesting.
4. **`MERGE` statement.** Standard SQL DML, frequently asked for, drops cleanly
   into the new `exec_dml.cpp`.
5. **Richer aggregates.**
   - `PERCENTILE_CONT`, `PERCENTILE_DISC`, `MODE() WITHIN GROUP`.
   - `ARRAY_AGG`, `STRING_AGG` with `ORDER BY` and `FILTER (WHERE …)`.
   - `GROUPING SETS`, `ROLLUP`, `CUBE` expansion.
   - User-defined aggregates: `CREATE AGGREGATE` with `INIT`/`ACCUM`/`FINAL`
     hooks in SQL or Lua.
6. **Scalar function expansion.** Targets:
   - Regex family: `REGEXP_REPLACE`, `REGEXP_MATCHES`, `REGEXP_SPLIT_TO_*`.
   - JSON path mutators: `JSON_SET`, `JSON_INSERT`, `JSON_REMOVE`, `JSON_MERGE_PATCH`.
   - XML: full `XMLQUERY`, `XMLTABLE`, `XMLSERIALIZE` via the existing
     `tdb_doc` library.
   - Datetime: full timezone-aware arithmetic, `DATE_TRUNC` parity with PG.
7. **PL/SQL extensions.**
   - `%TYPE` and `%ROWTYPE` declarations.
   - Named `EXCEPTION` types with `EXCEPTION WHEN <name> THEN …` handlers.
   - `CURSOR FOR` loops over arbitrary SELECTs.
   - `RECORD` types and inline composite construction.
   - Stored procedures with `IN`, `OUT`, `INOUT` parameters and `CALL` syntax.
   - Function and procedure overloading by parameter type signature.
8. **Packages.** Optional, low-priority within this track: group related
   functions/procedures and shared state under a namespace
   (`pkg.proc_name(...)`). Drop if M8 is tight.

### Success criteria

- All seven items above (1–7) ship, with reference docs and at least one
  example each in `examples/`.
- TPC-H Q15 (recursive view / TVF style) executes end-to-end without manual
  unnesting.
- `examples/erp_sample.sql` is rewritten to use computed columns, a TVF, and
  one user-defined aggregate to demonstrate the new surface.
- Existing PL/SQL bodies in v1.x continue to execute unchanged.

---

## 6. Track 4 — Lua full interop

### Goal

Make Lua a first-class extension language. Today the bridge in
`src/script/script_engine.cpp` exposes only 5 functions (`db.execute`,
`db.query`, `db.now`, `db.user`, `db.log`) and `push_value` handles only 6 of
the 22 `sql::Value` types natively — everything else (DECIMAL, UUID, JSON,
XML, GEOMETRY, ARRAY, COMPOSITE) is stringified, and `pop_value` cannot
round-trip any of them. The JS bridge already exposes catalog introspection
that Lua does not. This track closes those gaps.

### Concrete moves

1. **Full type bridge.** Replace the current `push_value`/`pop_value` pair
   with a typed userdata wrapper for every non-primitive `sql::Value::Type`:
   - DECIMAL → userdata with arithmetic metamethods; preserves scale.
   - UUID → userdata with `tostring` and equality.
   - DATE / TIME / TIMESTAMP → userdata with field accessors and arithmetic
     (today these arrive as integers, losing type identity).
   - JSON → table-like userdata with `__index`/`__newindex` for path access,
     iteration, and round-trip preservation of types JSON can express that
     Lua can't natively (e.g. integer vs float distinction).
   - XML → userdata exposing `:xpath(expr)`, `:xquery(expr)`, and serialization.
   - GEOMETRY → userdata with WKT/EWKT round-trip, SRID accessor, and basic
     predicates (`:contains`, `:intersects`).
   - ARRAY → Lua table with metatable that preserves element types on
     round-trip; element access is 1-based per Lua convention.
   - COMPOSITE → table keyed by field name, with type name accessible via a
     metatable hook.
   All conversions are lossless: a value pulled into Lua and pushed back
   produces the same `sql::Value`.
2. **Catalog introspection** (mirror what JS already exposes):
   - `db.tables()`, `db.views()`, `db.matviews()`, `db.sequences()`,
     `db.indexes()`, `db.tablespaces()`, `db.partitions(table)`,
     `db.triggers()`, `db.procedures()`, `db.functions()`, `db.scripts()`.
   - `db.describe(name)` returns column metadata for any table-like object.
   - `db.catalog` proxy table for live introspection.
3. **Define SQL functions and procedures in Lua.**
   - `db.create_function(name, {params=…, returns=…, body=fn})` registers a
     Lua function callable from SQL as if it were a UDF.
   - Same for `db.create_procedure` (supports `OUT`/`INOUT` via multi-return).
   - Equivalent SQL syntax: `CREATE FUNCTION f(...) RETURNS … LANGUAGE LUA AS '…'`.
   - Functions registered this way are catalog objects: persisted, dropped
     with `DROP FUNCTION`, visible in `db.functions()`.
4. **Call SQL functions and procedures from Lua.**
   - `db.call(name, args...)` invokes any SQL function, PL/SQL function, or
     procedure by name with full type round-trip.
   - `db.prepare(sql)` returns a prepared-statement userdata with
     `:execute(args…)` and `:query(args…)` methods.
5. **Lua-bodied table-valued functions.** Coordinates with Track 3 item 2:
   a Lua function that yields rows via a `yield_row(...)` helper, returnable
   as a table from `FROM`.
6. **Lua triggers.** `CREATE TRIGGER … EXECUTE FUNCTION lua_fn` where
   `lua_fn` receives `NEW` and `OLD` as COMPOSITE userdata.
7. **Transaction control from Lua.**
   - `db.begin()`, `db.commit()`, `db.rollback()`.
   - `db.savepoint(name)`, `db.release(name)`, `db.rollback_to(name)`.
8. **Documentation.** Rewrite `docs/api-reference.md` Lua section to cover the
   full surface, with a parity matrix against the JS bridge.

### Success criteria

- Round-trip test: a Lua script that pulls one of every `sql::Value::Type`
  from SQL, mutates it where applicable, writes it back, and asserts equality
  passes for all 22 types.
- The JS bridge surface from `CLAUDE.md` (`db.tables`, `db.views`, etc.) has
  Lua parity. A parity table in `docs/api-reference.md` shows no gaps.
- A Lua-defined UDF, UDA, TVF, and trigger each ship as an example under
  `examples/lua/`.
- A v1.x Lua script continues to run unchanged (existing 5-function surface
  is a strict subset).

---

## 7. Opportunistic — SQL completeness

Not headline features, but fix them while the surrounding code is open:

- **FULL OUTER JOIN and CROSS JOIN aliasing.** Listed in `CLAUDE.md` as known
  broken; resolve as part of the `exec_query.cpp` extraction.
- **Encrypted tables in JOINs.** Currently disallowed. Once the executor is
  decoupled from raw row access, route encrypted-table reads through a single
  decrypt boundary.
- **PL/SQL `BEGIN…END` parsing in `-f` files.** The CLI's block tracking is
  fragile; rework it once `plsql/` is its own module.
- **GRANT enforcement when `current_user_` is empty.** Today empty = superuser
  bypass. Add an explicit superuser concept so the bypass is intentional.
- **MERGE statement.** Standard SQL, frequently requested for ETL; lands
  cleanly once DML is in its own file.

None of these block the release; they ship as they finish.

---

## 8. Out of scope (tracked for v3.0)

- **Distribution & HA.** Replication, Raft, sharded execution, distributed
  transactions. This is a release of its own and breaks the embedded
  constraint; v3.0 will likely introduce an optional server mode rather than
  retrofit the embedded library.
- **Cloud / remote storage backends.**
- **A second wire protocol** (Postgres/MySQL compatibility).

---

## 9. Milestones

Two parallel swim lanes after M3: the perf lane (Track 2) and the
expressiveness lane (Tracks 3 + 4). The refactor (Track 1) must finish first
to unblock both.

| Month | Milestone |
|-------|-----------|
| M1 | `tests/` exists with smoke coverage of every existing module. CI green. |
| M2 | `database.h` split complete; no header > 1,500 lines. Examples still pass. |
| M3 | `QueryExecutor` extracted. Perf suite baselined against v1.x tag. Full Lua type bridge landed (Track 4 item 1). |
| M4 | Perf: vectorized expression path + columnar pushdown. Expressiveness: computed columns, MERGE, richer aggregates. Lua: catalog introspection + transaction control. |
| M5 | Perf: planner-driven join reordering + parallel scan/aggregate. Expressiveness: TVFs + LATERAL. Lua: define/call SQL funcs/procs + Lua triggers. |
| M6 | Perf: optional LLVM expr JIT behind flag. Expressiveness: PL/SQL extensions (%TYPE/%ROWTYPE, named exceptions, OUT params, overloads). Lua: Lua-bodied TVFs. |
| M7 | Scalar function expansion (regex, JSON path mutators, XML query, datetime tz). SQL completeness cleanup. |
| M8 | On-disk migration path verified, docs refresh (incl. Lua/JS parity matrix), v2.00 cut. |

Slip budget: two months (raised from one to reflect the wider scope).

**Cut order if slipping:**

1. Optional LLVM expr JIT (M6 perf).
2. Packages (Track 3 item 8).
3. Parallel scan/aggregate (M5 perf).
4. Lua-bodied TVFs (M6) — Lua can still define scalar UDFs.
5. `CUBE`/`ROLLUP` expansion (degrade to `GROUPING SETS` only).

Non-negotiable core: the refactor (M1–M3), the vectorized path (M4 perf),
computed columns and MERGE (M4 expressiveness), the full Lua type bridge
(M3 Lua), and Lua's ability to define and call SQL functions (M5 Lua).

---

## 10. Risks

- **Refactor without perf payoff.** Mitigation: M3 baselines the perf suite
  *before* M4–M5, so the value of the refactor is measurable from the moment
  the vectorized path lands.
- **On-disk format drift during refactor.** Mitigation: add a v1-format
  regression test in M1 that opens every shipped `.tdb` example; gate every PR
  on it.
- **Scope creep into distribution.** Mitigation: §8 is normative. Anything
  matching it gets filed against v3.0, not v2.x.
- **Empty `tests/` today.** The biggest hidden risk. M1 is non-negotiable for
  this reason.
- **Lua type bridge ABI churn.** Switching from raw values to userdata for
  DECIMAL/JSON/XML/etc. is a breaking change for any v1.x Lua script that
  relied on stringified output. Mitigation: ship a `db.compat = "v1"` session
  flag that restores stringification, document the upgrade path, and remove
  the flag in v3.0.
- **TVF + LATERAL semantics across the executor refactor.** TVFs change what
  a `FROM` clause can produce mid-query. Mitigation: land TVFs only after the
  `exec_query.cpp` split is stable (M3), and write the TVF test suite before
  the implementation.
- **Wider scope vs. focused-release promise.** The original ask was 1–2
  themes; this draft is four. Mitigation: §2 surfaces the tradeoff explicitly
  and §9 lists a cut order that can collapse v2.00 back toward the original
  focused scope if needed.
