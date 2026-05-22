# SQL Reference

## Data Types

### Numeric Types

| Type | Aliases | Description |
|------|---------|-------------|
| `INTEGER` | `INT`, `INT32` | 32-bit signed integer |
| `BIGINT` | `INT64` | 64-bit signed integer |
| `SMALLINT` | `INT16` | 16-bit signed integer |
| `TINYINT` | `INT8` | 8-bit signed integer |
| `FLOAT` | `REAL`, `FLOAT32` | 32-bit floating point |
| `DOUBLE` | `DOUBLE PRECISION`, `FLOAT64` | 64-bit floating point |
| `DECIMAL(p,s)` | `NUMERIC(p,s)` | Arbitrary precision decimal |
| `SERIAL` | `AUTO_INCREMENT` | Auto-incrementing integer |
| `BIGSERIAL` | | Auto-incrementing 64-bit integer |
| `MONEY` | | Currency type (stored as decimal) |
| `BOOLEAN` | `BOOL` | TRUE / FALSE |

### String Types

| Type | Description |
|------|-------------|
| `VARCHAR(n)` | Variable-length string up to n characters |
| `TEXT` | Unlimited-length string |
| `CHAR(n)` | Fixed-length string |
| `NCHAR(n)` | National character (Unicode-aware) |
| `NVARCHAR(n)` | National varying character |

### Binary Types

| Type | Description |
|------|-------------|
| `BLOB` | Binary large object |
| `BINARY(n)` | Fixed-length binary |
| `VARBINARY(n)` | Variable-length binary |
| `BIT(n)` | Bit string |

### Date/Time Types

| Type | Description |
|------|-------------|
| `DATE` | Date only (YYYY-MM-DD) |
| `TIME` | Time only (HH:MM:SS) |
| `TIMESTAMP` | Date and time |
| `TIMESTAMP WITH TIME ZONE` | Date, time, and timezone offset |
| `INTERVAL` | Duration (months + microseconds) |

### Extended Types

| Type | Description |
|------|-------------|
| `UUID` | Universally unique identifier |
| `JSON` | JSON document (text storage) |
| `JSONB` | Binary JSON |
| `XML` | XML document |
| `GEOMETRY` | OGC/ISO spatial geometry (WKT format) |
| `GEOGRAPHY` | Geographic type (spheroid) |
| `ENUM` | User-defined enumeration |
| `ARRAY` | Ordered collection |
| `MULTISET` | Unordered collection with multiplicity |
| Composite | User-defined record type (CREATE TYPE) |

---

## DDL Statements

### CREATE TABLE

```sql
CREATE [TEMPORARY] TABLE [IF NOT EXISTS] [schema.]name (
    column_name datatype [constraints],
    ...
    [table_constraints]
) [ENCRYPTED]
  [COLUMNAR]
  [PARTITION BY {RANGE|LIST|HASH} (column) (...)]
```

**Column Constraints:**
- `PRIMARY KEY` -- unique non-null identifier
- `NOT NULL` -- non-null constraint
- `UNIQUE` -- uniqueness constraint
- `DEFAULT expr` -- default value
- `CHECK (expr)` -- check constraint
- `AUTO_INCREMENT` / `SERIAL` -- auto-incrementing
- `ENCRYPTED` -- column-level AES-256-GCM encryption
- `GENERATED ALWAYS AS (expr) [STORED]` -- computed column
- `REFERENCES table(col) [ON DELETE {CASCADE|RESTRICT|SET NULL|SET DEFAULT}]`

**Table Constraints:**
- `PRIMARY KEY (col1, col2, ...)` -- composite primary key
- `UNIQUE (col1, col2, ...)` -- composite uniqueness
- `FOREIGN KEY (cols) REFERENCES table(cols) [ON DELETE action] [ON UPDATE action]`
- `CHECK (expr)` -- table-level check

### CREATE INDEX

```sql
CREATE [UNIQUE] INDEX [IF NOT EXISTS] name
ON table USING {BTREE|BPTREE|HASH|RTREE|RPTREE|GIST}
(column [ASC|DESC], ...)
[WHERE predicate]
```

### CREATE VIEW

```sql
CREATE [OR REPLACE] VIEW [IF NOT EXISTS] name [(columns)]
AS SELECT ...
```

### CREATE MATERIALIZED VIEW

```sql
CREATE MATERIALIZED VIEW [IF NOT EXISTS] name
[WRITABLE] [WITH [NO] DATA]
AS SELECT ...
[TABLESPACE name]
```

```sql
REFRESH MATERIALIZED VIEW [CONCURRENTLY] name [WITH [NO] DATA]
```

### CREATE SEQUENCE

```sql
CREATE SEQUENCE [IF NOT EXISTS] name
[START WITH n] [INCREMENT BY n]
[MINVALUE n] [MAXVALUE n]
[CYCLE | NO CYCLE]
```

### CREATE TYPE

```sql
-- Composite type
CREATE TYPE [IF NOT EXISTS] name AS (
    field1 datatype,
    field2 datatype, ...
)

-- Enumeration type
CREATE TYPE [IF NOT EXISTS] name AS ENUM ('label1', 'label2', ...)

-- Domain type
CREATE DOMAIN name AS base_type [CHECK (condition)]
```

### ALTER TABLE

```sql
ALTER TABLE name
    ADD COLUMN col datatype [constraints]
  | DROP COLUMN col [CASCADE]
  | ALTER COLUMN col SET DEFAULT expr
  | ALTER COLUMN col DROP DEFAULT
  | ADD CONSTRAINT name constraint_def
  | DROP CONSTRAINT name
  | RENAME TO new_name
  | RENAME COLUMN old TO new
  | ADD PARTITION partition_def
  | DROP PARTITION name
```

### DROP

```sql
DROP {TABLE|VIEW|INDEX|SEQUENCE|TRIGGER|PROCEDURE|FUNCTION|TYPE|DOMAIN|MATERIALIZED VIEW|TABLESPACE}
[IF EXISTS] name [CASCADE]
```

### TRUNCATE

```sql
TRUNCATE TABLE name [CASCADE]
```

---

## DML Statements

### SELECT

```sql
[WITH [RECURSIVE] cte_name AS (SELECT ...)] [, ...]
SELECT [DISTINCT | ALL]
    expression [[AS] alias], ...
FROM table_ref [[AS] alias]
    [{INNER|LEFT|RIGHT|FULL} JOIN table_ref ON condition]
[WHERE condition]
[GROUP BY {expr | GROUPING SETS (...) | CUBE (...) | ROLLUP (...)}]
[HAVING condition]
[WINDOW name AS (window_spec)]
[{UNION [ALL] | INTERSECT [ALL] | EXCEPT [ALL]} SELECT ...]
[ORDER BY expr [ASC|DESC] [NULLS {FIRST|LAST}], ...]
[LIMIT count]
[OFFSET skip]
```

### INSERT

```sql
INSERT INTO table [(columns)]
VALUES (expr, ...) [, (expr, ...)]
[ON CONFLICT DO NOTHING]
[ON CONFLICT DO UPDATE SET col = expr [, ...]]
[RETURNING expr [, ...]]
```

```sql
INSERT INTO table [(columns)] SELECT ...
[RETURNING expr [, ...]]
```

### UPDATE

```sql
UPDATE table [[AS] alias]
SET column = expr [, ...]
[FROM other_tables]
[WHERE condition]
[RETURNING expr [, ...]]
```

### DELETE

```sql
DELETE FROM table [[AS] alias]
[USING other_tables]
[WHERE condition]
[RETURNING expr [, ...]]
```

### MERGE

```sql
MERGE INTO target [alias]
USING source [alias] ON condition
WHEN MATCHED THEN UPDATE SET col = expr [, ...]
WHEN NOT MATCHED THEN INSERT (cols) VALUES (vals)
[WHEN MATCHED THEN DELETE]
```

---

## Partitioning

### RANGE Partitioning

```sql
CREATE TABLE orders (
    id INT, order_date DATE, amount DECIMAL(10,2)
) PARTITION BY RANGE (order_date) (
    PARTITION p_q1 VALUES LESS THAN ('2025-04-01'),
    PARTITION p_q2 VALUES LESS THAN ('2025-07-01'),
    PARTITION p_q3 VALUES LESS THAN ('2025-10-01'),
    PARTITION p_future VALUES LESS THAN MAXVALUE
);
```

### LIST Partitioning

```sql
CREATE TABLE events (
    id INT, severity VARCHAR(10), message TEXT
) PARTITION BY LIST (severity) (
    PARTITION p_info VALUES IN ('INFO', 'DEBUG'),
    PARTITION p_warn VALUES IN ('WARN'),
    PARTITION p_error VALUES IN ('ERROR', 'CRITICAL')
);
```

### HASH Partitioning

```sql
CREATE TABLE sessions (
    id INT, user_id INT, token VARCHAR(100)
) PARTITION BY HASH (user_id) (
    PARTITION p0 VALUES WITH (MODULUS 4, REMAINDER 0),
    PARTITION p1 VALUES WITH (MODULUS 4, REMAINDER 1),
    PARTITION p2 VALUES WITH (MODULUS 4, REMAINDER 2),
    PARTITION p3 VALUES WITH (MODULUS 4, REMAINDER 3)
);
```

Partition pruning is automatic -- queries with WHERE clauses on the partition key only scan relevant partitions.

---

## Encryption

TDB supports AES-256-GCM transparent data encryption at the table and column level:

```sql
-- Table-level encryption
CREATE TABLE secrets ENCRYPTED (
    id INT,
    data TEXT ENCRYPTED,     -- column-level encryption
    notes TEXT               -- this column is also encrypted (table-level)
);
```

---

## Columnar Storage

```sql
CREATE TABLE metrics (
    ts DATE, region VARCHAR(10), value INT
) COLUMNAR;

-- Data stored per-column, auto-materialized to rows for queries
SELECT region, SUM(value) FROM metrics GROUP BY region;
```

---

## Transaction Control

```sql
BEGIN [TRANSACTION]
    [ISOLATION LEVEL {SERIALIZABLE|REPEATABLE READ|READ COMMITTED|READ UNCOMMITTED}]
    [READ ONLY | READ WRITE];

COMMIT;
ROLLBACK;

SAVEPOINT name;
RELEASE SAVEPOINT name;
ROLLBACK TO SAVEPOINT name;
```

---

## Access Control

```sql
GRANT {SELECT|INSERT|UPDATE|DELETE|ALL} [, ...]
ON [TABLE|VIEW|SEQUENCE] object
TO role [WITH GRANT OPTION];

REVOKE {SELECT|INSERT|UPDATE|DELETE|ALL} [, ...]
ON [TABLE|VIEW|SEQUENCE] object
FROM role;
```

Privileges are enforced on SELECT, INSERT, UPDATE, and DELETE when a user context is set.

---

## Prepared Statements

```sql
PREPARE stmt_name AS SELECT * FROM users WHERE id = $1;
EXECUTE stmt_name(42);
DEALLOCATE stmt_name;
DEALLOCATE ALL;
```

---

## Cursors

```sql
DECLARE cur [SCROLL] [INSENSITIVE] CURSOR [WITH HOLD] FOR SELECT ...;
OPEN cur;
FETCH {NEXT|PRIOR|FIRST|LAST|FORWARD n|BACKWARD n} FROM cur;
CLOSE cur;
```

---

## Graph Queries

TDB supports SQL/PGQ-inspired graph pattern matching on relational tables:

```sql
-- Tables serve as node/edge collections
CREATE TABLE people (id INT PRIMARY KEY, name VARCHAR(20));
CREATE TABLE knows (id INT, from_id INT, to_id INT,
    FOREIGN KEY (from_id) REFERENCES people(id),
    FOREIGN KEY (to_id) REFERENCES people(id));

-- MATCH pattern resolves FK relationships automatically
MATCH (a:knows)-[e:to_id]->(b:people)
RETURN a.from_id, b.name
ORDER BY b.name;
```

---

## EXPLAIN

```sql
EXPLAIN SELECT * FROM users WHERE id = 1;
```

---

## INFORMATION_SCHEMA

```sql
SELECT * FROM INFORMATION_SCHEMA.TABLES;
SELECT * FROM INFORMATION_SCHEMA.COLUMNS;
SELECT * FROM INFORMATION_SCHEMA.INDEXES;
SELECT * FROM INFORMATION_SCHEMA.SEQUENCES;
```

---

## Scalar Functions

### String Functions

| Function | Description |
|----------|-------------|
| `UPPER(s)` | Convert to uppercase |
| `LOWER(s)` | Convert to lowercase |
| `LENGTH(s)` | Character count |
| `SUBSTRING(s, start, len)` | Extract substring |
| `TRIM([LEADING\|TRAILING\|BOTH] [chars] FROM s)` | Remove whitespace/chars |
| `LTRIM(s)` / `RTRIM(s)` | Remove leading/trailing whitespace |
| `REPLACE(s, search, replace)` | String replacement |
| `CONCAT(s1, s2, ...)` | String concatenation |
| `LEFT(s, n)` / `RIGHT(s, n)` | Leftmost/rightmost n characters |
| `REVERSE(s)` | Reverse string |
| `REPEAT(s, n)` / `REPT(s, n)` | Repeat string n times |
| `LPAD(s, len, pad)` / `RPAD(s, len, pad)` | Left/right pad |
| `POSITION(search IN s)` | Find position |
| `SPLIT_PART(s, delim, field)` | Split and extract field |
| `TRANSLATE(s, from, to)` | Character translation |
| `INITCAP(s)` / `PROPER(s)` | Capitalize words |
| `ASCII(s)` | ASCII code of first character |
| `CHR(code)` / `CHAR(code)` | Character from code |
| `SPACE(n)` | String of n spaces |
| `CONTAINS(s, substr)` | Check containment |
| `STARTSWITH(s, prefix)` / `ENDSWITH(s, suffix)` | Check prefix/suffix |
| `OCTET_LENGTH(s)` | Byte count |

### Math Functions

| Function | Description |
|----------|-------------|
| `ABS(n)` | Absolute value |
| `ROUND(n [, decimals])` | Round |
| `CEIL(n)` / `CEILING(n)` | Round up |
| `FLOOR(n)` | Round down |
| `TRUNC(n [, decimals])` | Truncate decimals |
| `MOD(a, b)` | Modulo |
| `POWER(base, exp)` | Exponentiation |
| `SQRT(n)` / `CBRT(n)` | Square/cube root |
| `SIGN(n)` | Sign (-1, 0, 1) |
| `GCD(a, b)` / `LCM(a, b)` | Greatest common divisor / Least common multiple |
| `FACTORIAL(n)` | n! |
| `COMBIN(n, k)` | Combinations |
| `PERMUT(n, k)` | Permutations |
| `HYPOT(a, b)` | Hypotenuse |
| `RANDBETWEEN(a, b)` | Random integer in range |
| `EVEN(n)` / `ODD(n)` | Next even/odd number |

### Trigonometric Functions

| Function | Description |
|----------|-------------|
| `SIN(x)` / `COS(x)` / `TAN(x)` | Trigonometric functions |
| `ASIN(x)` / `ACOS(x)` / `ATAN(x)` | Inverse trigonometric |
| `ATAN2(y, x)` | Two-argument arctangent |
| `SINH(x)` / `COSH(x)` / `TANH(x)` | Hyperbolic functions |
| `DEGREES(rad)` / `RADIANS(deg)` | Angle conversion |
| `PI()` | Pi constant |

### Logarithmic / Exponential

| Function | Description |
|----------|-------------|
| `LN(n)` | Natural logarithm |
| `LOG(n)` / `LOG(base, n)` | Logarithm |
| `LOG2(n)` / `LOG10(n)` | Base-2 / Base-10 logarithm |
| `EXP(n)` | Exponential (e^n) |

### Date/Time Functions

| Function | Description |
|----------|-------------|
| `NOW()` / `CURRENT_TIMESTAMP` | Current timestamp |
| `CURRENT_DATE()` / `TODAY()` | Current date |
| `YEAR(d)` / `MONTH(d)` / `DAY(d)` | Extract date component |
| `HOUR(t)` / `MINUTE(t)` / `SECOND(t)` | Extract time component |
| `DATE_TRUNC(unit, timestamp)` | Truncate to unit |
| `DATEDIFF(unit, start, end)` | Difference between dates |
| `DATEADD(unit, n, date)` | Add interval |
| `AGE(d1, d2)` | Age between dates |
| `EXTRACT(field FROM date)` | Extract date field |

### Conditional / Null Functions

| Function | Description |
|----------|-------------|
| `COALESCE(expr1, expr2, ...)` | First non-null |
| `NULLIF(a, b)` | NULL if equal |
| `IIF(cond, true_val, false_val)` / `IF(...)` | Conditional |
| `IFNULL(expr, default)` / `NVL(expr, default)` | Default if null |
| `NVL2(expr, if_not_null, if_null)` | Conditional on null |
| `GREATEST(a, b, ...)` / `LEAST(a, b, ...)` | Max/min of values |
| `CHOOSE(index, v1, v2, ...)` | Choose by index |
| `CASE WHEN ... THEN ... ELSE ... END` | Case expression |

### Type Conversion

| Function | Description |
|----------|-------------|
| `CAST(expr AS type)` | Explicit type cast |
| `expr::type` | PostgreSQL-style cast |
| `TO_INT(v)` / `TO_CHAR(v)` / `TO_DECIMAL(v)` | Type converters |
| `TO_JSON(v)` / `TO_XML(v)` / `TO_UUID(v)` | Format converters |
| `TYPEOF(v)` | Get type name |

### Cryptographic Functions

| Function | Description |
|----------|-------------|
| `MD5(s)` / `SHA1(s)` / `SHA256(s)` | Hash functions |
| `HMAC_SHA1(key, msg)` / `HMAC_SHA256(key, msg)` | HMAC |
| `PBKDF2_SHA256(password, salt, iter, len)` | Key derivation |
| `GEN_RANDOM_UUID()` | Generate UUID |
| `RANDOM_BYTES(len)` | Random bytes |
| `CRC32(s)` | CRC32 checksum |

### Spatial Functions (PostGIS-compatible)

| Function | Description |
|----------|-------------|
| `ST_GEOMFROMTEXT(wkt [, srid])` | Create geometry |
| `ST_ASTEXT(geom)` | Convert to WKT |
| `ST_MAKEPOINT(x, y [, z])` | Create point |
| `ST_DISTANCE(a, b)` / `ST_3DDISTANCE(a, b)` | Distance |
| `ST_DWITHIN(a, b, dist)` | Within distance check |
| `ST_INTERSECTS(a, b)` / `ST_CONTAINS(a, b)` | Spatial predicates |
| `ST_WITHIN(a, b)` / `ST_TOUCHES(a, b)` | Spatial predicates |
| `ST_CROSSES(a, b)` / `ST_OVERLAPS(a, b)` | Spatial predicates |
| `ST_EQUALS(a, b)` / `ST_DISJOINT(a, b)` | Equality / disjointness |
| `ST_X(g)` / `ST_Y(g)` / `ST_Z(g)` / `ST_SRID(g)` | Accessors |
| `ST_GEOMETRYTYPE(g)` / `ST_ISVALID(g)` / `ST_ISEMPTY(g)` | Properties |
| `ST_NUMPOINTS(g)` / `ST_NUMGEOMETRIES(g)` | Counts |
| `ST_STARTPOINT(g)` / `ST_ENDPOINT(g)` | Point extraction |

### Array Functions

| Function | Description |
|----------|-------------|
| `MAKE_ARRAY(expr, ...)` | Create array |
| `ARRAY_LENGTH(arr)` | Array length |
| `ARRAY_AT(arr, index)` | Element at index |
| `ARRAY_APPEND(arr, elem)` | Append element |
| `ARRAY_CONTAINS(arr, elem)` | Check containment |
| `ARRAY_REMOVE_AT(arr, index)` | Remove element |
| `ARRAY_CONCAT(arr1, arr2)` | Concatenate arrays |

### Statistical / Prediction Functions

| Function | Description |
|----------|-------------|
| `CORREL(x, y)` | Pearson correlation |
| `NORM_DIST(val, mean, stdev, cumulative)` | Normal distribution |
| `NORM_INV(prob, mean, stdev)` | Inverse normal |
| `T_DIST(val, df, cumulative)` | T distribution |
| `FORECAST(x, x_arr, y_arr)` | Linear forecast |
| `SLOPE(y_arr, x_arr)` / `INTERCEPT(y_arr, x_arr)` | Regression |
| `RSQ(y_arr, x_arr)` | R-squared |

---

## Aggregate Functions

| Function | Description |
|----------|-------------|
| `COUNT(*)` / `COUNT(expr)` / `COUNT(DISTINCT expr)` | Row count |
| `SUM(expr)` / `SUM(DISTINCT expr)` | Sum |
| `AVG(expr)` / `AVG(DISTINCT expr)` | Average |
| `MIN(expr)` / `MAX(expr)` | Minimum / Maximum |
| `STDDEV(expr)` / `STDDEV_POP(expr)` / `STDDEV_SAMP(expr)` | Standard deviation |
| `VARIANCE(expr)` / `VAR_POP(expr)` / `VAR_SAMP(expr)` | Variance |
| `STRING_AGG(expr, delimiter)` / `GROUP_CONCAT(expr, delim)` | String concatenation |
| `MEDIAN(expr)` | Median |
| `GROUPING(expr)` | Grouping indicator (for GROUPING SETS/CUBE/ROLLUP) |

### GROUPING SETS / CUBE / ROLLUP

```sql
SELECT dept, region, SUM(sales)
FROM sales_data
GROUP BY GROUPING SETS ((dept, region), (dept), (region), ());

-- CUBE generates all combinations
GROUP BY CUBE (dept, region)

-- ROLLUP generates hierarchical subtotals
GROUP BY ROLLUP (country, state, city)
```

---

## Window Functions

```sql
function_name([args]) OVER (
    [PARTITION BY expr, ...]
    [ORDER BY expr [ASC|DESC] [NULLS {FIRST|LAST}], ...]
    [frame_clause]
)
```

### Ranking Functions

| Function | Description |
|----------|-------------|
| `ROW_NUMBER()` | Sequential row number (1, 2, 3, ...) |
| `RANK()` | Rank with gaps for ties |
| `DENSE_RANK()` | Rank without gaps |
| `NTILE(n)` | Divide into n equal buckets |
| `PERCENT_RANK()` | Relative rank (0 to 1) |
| `CUME_DIST()` | Cumulative distribution (0 to 1) |

### Value Functions

| Function | Description |
|----------|-------------|
| `LAG(expr [, offset [, default]])` | Previous row value |
| `LEAD(expr [, offset [, default]])` | Next row value |
| `FIRST_VALUE(expr)` | First value in frame |
| `LAST_VALUE(expr)` | Last value in frame |
| `NTH_VALUE(expr, n)` | Nth value in frame |

### Aggregate Window Functions

`SUM()`, `COUNT()`, `AVG()`, `MIN()`, `MAX()` can be used as window functions with running/sliding frame semantics.

### Frame Clause

```sql
{ROWS | RANGE | GROUPS}
    {UNBOUNDED PRECEDING | n PRECEDING | CURRENT ROW}
    [BETWEEN ... AND {UNBOUNDED FOLLOWING | n FOLLOWING | CURRENT ROW}]
```
