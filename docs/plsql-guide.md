# PL/SQL Guide

TDB includes a procedural SQL interpreter supporting stored procedures, functions, triggers, and control flow constructs.

## Stored Procedures

### CREATE PROCEDURE

```sql
CREATE [OR REPLACE] PROCEDURE [IF NOT EXISTS] name
    ([IN|OUT|INOUT] param1 type, ...)
    [LANGUAGE {plsql|lua|javascript}]
    AS 'body'
    | AS BEGIN body END;
```

**Examples:**

```sql
-- String-body syntax (recommended for complex procedures)
CREATE PROCEDURE greet(name VARCHAR(100)) AS
    'RETURN name';

-- BEGIN...END syntax (for multi-line file mode)
CREATE PROCEDURE calculate(x INT, y INT) AS BEGIN
    DECLARE result INT := x + y;
    RETURN result
END;
```

### CREATE FUNCTION

```sql
CREATE [OR REPLACE] FUNCTION [IF NOT EXISTS] name
    (param1 type, ...)
    RETURNS return_type
    [LANGUAGE {plsql|lua|javascript}]
    AS 'body';
```

```sql
CREATE FUNCTION double_it(n INT) RETURNS INT AS
    'RETURN n * 2';

-- Lua function
CREATE FUNCTION add(a INT, b INT) RETURNS INT
    LANGUAGE lua AS 'return a + b';
```

### CALL

```sql
CALL procedure_name(arg1, arg2, ...);
```

### DROP

```sql
DROP PROCEDURE [IF EXISTS] name;
DROP FUNCTION [IF EXISTS] name;
```

---

## PL/SQL Control Flow

### Variable Declaration

```sql
DECLARE variable_name type := initial_value;
```

Variables are scoped to the procedure body. Types are inferred from the initial value:

```sql
DECLARE counter INT := 0;
DECLARE name VARCHAR(50) := John;    -- unquoted = string literal
DECLARE rate FLOAT := 3.14;
DECLARE flag BOOLEAN := TRUE;
DECLARE empty_val INT := NULL;
```

### Variable Assignment

```sql
variable := expression;
```

The expression is evaluated by executing `SELECT expression` with variable substitution:

```sql
DECLARE x INT := 10;
x := x + 5;          -- x becomes 15
x := x * 2;          -- x becomes 30
```

### IF / ELSIF / ELSE

```sql
IF condition THEN
    statements;
ELSIF condition THEN
    statements;
ELSE
    statements;
END IF;
```

**Example:**

```sql
CREATE PROCEDURE classify(score INT) AS
    'DECLARE grade VARCHAR(1) := F;
     IF score >= 90 THEN grade := A;
     ELSIF score >= 80 THEN grade := B;
     ELSIF score >= 70 THEN grade := C;
     ELSE grade := F;
     END IF;
     RETURN grade';

CALL classify(85);  -- Returns 'B'
```

### FOR Loop

```sql
FOR variable IN start..end LOOP
    statements;
END LOOP;

FOR variable IN REVERSE start..end LOOP
    statements;
END LOOP;
```

**Example:**

```sql
CREATE PROCEDURE sum_range(n INT) AS
    'DECLARE total INT := 0;
     FOR i IN 1..5 LOOP
         total := total + i;
     END LOOP;
     RETURN total';

CALL sum_range(5);  -- Returns 15
```

### WHILE Loop

```sql
WHILE condition LOOP
    statements;
END LOOP;
```

**Example:**

```sql
CREATE PROCEDURE factorial(n INT) AS
    'DECLARE result INT := 1;
     DECLARE i INT := 1;
     WHILE i <= 5 LOOP
         result := result * i;
         i := i + 1;
     END LOOP;
     RETURN result';

CALL factorial(5);  -- Returns 120
```

### RETURN

```sql
RETURN expression;
```

Exits the procedure and returns the expression value. Variables in the expression are automatically substituted.

### RAISE EXCEPTION

```sql
RAISE EXCEPTION 'error message';
```

Immediately exits the procedure with an error.

**Example:**

```sql
CREATE PROCEDURE safe_div(a INT, b INT) AS
    'IF b = 0 THEN
         RAISE EXCEPTION Division by zero;
     END IF;
     RETURN a / b';
```

### Embedded SQL

Any statement that doesn't match a PL/SQL construct is executed as SQL with variable substitution:

```sql
CREATE PROCEDURE insert_user(uname VARCHAR(100)) AS
    'DECLARE uid INT := 0;
     INSERT INTO users (name) VALUES (uname);
     SELECT MAX(id) INTO uid FROM users;
     RETURN uid';
```

---

## Triggers

### CREATE TRIGGER

```sql
CREATE [OR REPLACE] TRIGGER [IF NOT EXISTS] name
    {BEFORE|AFTER} {INSERT|UPDATE|DELETE}
    ON table
    [FOR EACH ROW]
    [LANGUAGE {plsql|lua|javascript}]
    AS 'body';
```

Triggers fire automatically on DML operations:

- **BEFORE INSERT** -- fires before each row is inserted
- **AFTER INSERT** -- fires after each row is inserted
- **BEFORE UPDATE** -- fires before each row is updated
- **AFTER UPDATE** -- fires after each row is updated (old and new row available)
- **BEFORE DELETE** -- fires before each row is deleted
- **AFTER DELETE** -- fires after each row is deleted

**Example:**

```sql
CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, status VARCHAR(20));
CREATE TABLE audit_log (id INT PRIMARY KEY AUTO_INCREMENT, msg TEXT);

CREATE TRIGGER trg_order_audit
    AFTER INSERT ON orders
    LANGUAGE lua
    AS 'db.execute("INSERT INTO audit_log (msg) VALUES (''new order'')")';

INSERT INTO orders (status) VALUES ('PENDING');
-- audit_log now has a row: 'new order'
```

### Lua Trigger Context

In Lua triggers, the `trigger` global table provides:

```lua
trigger.timing    -- "BEFORE" or "AFTER"
trigger.event     -- "INSERT", "UPDATE", or "DELETE"
trigger.table     -- table name
trigger.new       -- new row (table/dict) for INSERT/UPDATE
trigger.old       -- old row (table/dict) for UPDATE/DELETE
trigger.columns   -- column names array
```

BEFORE triggers can modify `trigger.new` to alter the inserted/updated row.

---

## Prepared Statements

```sql
PREPARE my_query AS SELECT * FROM users WHERE id = $1;
EXECUTE my_query(42);
DEALLOCATE my_query;
```

---

## Cursors

```sql
DECLARE my_cursor SCROLL CURSOR FOR
    SELECT * FROM users ORDER BY id;

OPEN my_cursor;
FETCH NEXT FROM my_cursor;     -- row 1
FETCH NEXT FROM my_cursor;     -- row 2
FETCH PRIOR FROM my_cursor;    -- row 1
FETCH LAST FROM my_cursor;     -- last row
FETCH FIRST FROM my_cursor;    -- row 1
CLOSE my_cursor;
```

Supported directions: `NEXT`, `PRIOR`, `FIRST`, `LAST`, `FORWARD n`, `BACKWARD n`.
