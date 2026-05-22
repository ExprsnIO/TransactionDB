-- ============================================================================
-- TDB v1.1.0 — Comprehensive Sample Database
-- ERP / CRM / Security Model
--
-- Usage:
--   tdbcli -f examples/erp_sample.sql
--   tdb-tui -f examples/erp_sample.sql
--
-- Showcases:
--   - All major data types (INT, TEXT, VARCHAR, BOOLEAN, DATE,
--     TIMESTAMP, DECIMAL, FLOAT)
--   - Constraints (PK, FK, NOT NULL, UNIQUE, DEFAULT, CHECK, ON DELETE CASCADE)
--   - AUTO_INCREMENT
--   - Sequences (CREATE SEQUENCE, NEXTVAL, CURRVAL)
--   - Indexes (BTREE, HASH, BPTREE, UNIQUE)
--   - Views and Materialized Views (CREATE, REFRESH)
--   - Partitioned tables (RANGE, LIST, HASH)
--   - Table-level and column-level encryption (AES-256-GCM)
--   - Transactions, savepoints
--   - Window functions, subqueries, correlated subqueries
--   - JOINs (INNER, LEFT, RIGHT)
--   - Set operations (UNION, INTERSECT, EXCEPT)
--   - Aggregates (COUNT, SUM, AVG, MIN, MAX, STDDEV, VARIANCE, STRING_AGG, MEDIAN)
--   - GROUPING SETS / CUBE / ROLLUP with GROUPING()
--   - Scalar functions (math, trig, text, date, crypto, etc.)
--   - CASE / COALESCE / NULLIF / CAST / IIF
--   - EXPLAIN
--   - INFORMATION_SCHEMA queries
--   - 300+ sample rows across all tables
-- ============================================================================

-- ════════════════════════════════════════════════════════════════════════════
-- PART 1: SEQUENCES
-- ════════════════════════════════════════════════════════════════════════════

CREATE SEQUENCE seq_audit_id START WITH 1 INCREMENT BY 1;
CREATE SEQUENCE seq_ticket_id START WITH 10000 INCREMENT BY 1;
CREATE SEQUENCE seq_invoice_no START WITH 100000 INCREMENT BY 1;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 2: CORE SECURITY / USER TABLES
-- ════════════════════════════════════════════════════════════════════════════

-- Roles for RBAC
CREATE TABLE roles (
    role_id     INTEGER PRIMARY KEY AUTO_INCREMENT,
    role_name   VARCHAR(50) NOT NULL UNIQUE,
    description TEXT,
    is_active   BOOLEAN DEFAULT TRUE,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- User accounts
CREATE TABLE users (
    user_id       INTEGER PRIMARY KEY AUTO_INCREMENT,
    username      VARCHAR(100) NOT NULL UNIQUE,
    email         VARCHAR(255) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    full_name     VARCHAR(200) NOT NULL,
    phone         VARCHAR(30),
    is_active     BOOLEAN DEFAULT TRUE,
    failed_logins INTEGER DEFAULT 0,
    last_login    TIMESTAMP,
    created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CHECK (failed_logins >= 0)
);

-- User-Role many-to-many
CREATE TABLE user_roles (
    user_id  INTEGER NOT NULL,
    role_id  INTEGER NOT NULL,
    granted_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, role_id),
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE,
    FOREIGN KEY (role_id) REFERENCES roles(role_id) ON DELETE CASCADE
);

-- Permissions
CREATE TABLE permissions (
    perm_id    INTEGER PRIMARY KEY AUTO_INCREMENT,
    perm_name  VARCHAR(100) NOT NULL UNIQUE,
    resource   VARCHAR(100) NOT NULL,
    action_type VARCHAR(50) NOT NULL,
    UNIQUE (resource, action_type)
);

-- Role-Permission mapping
CREATE TABLE role_permissions (
    role_id  INTEGER NOT NULL,
    perm_id  INTEGER NOT NULL,
    PRIMARY KEY (role_id, perm_id),
    FOREIGN KEY (role_id) REFERENCES roles(role_id) ON DELETE CASCADE,
    FOREIGN KEY (perm_id) REFERENCES permissions(perm_id) ON DELETE CASCADE
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 3: CRM TABLES
-- ════════════════════════════════════════════════════════════════════════════

-- Customers
CREATE TABLE customers (
    customer_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    company_name  VARCHAR(200) NOT NULL,
    contact_name  VARCHAR(200),
    contact_email VARCHAR(255),
    phone         VARCHAR(30),
    address       TEXT,
    city          VARCHAR(100),
    region        VARCHAR(50),
    country       VARCHAR(100) DEFAULT 'US',
    postal_code   VARCHAR(20),
    credit_limit  DECIMAL(12,2) DEFAULT 10000.00,
    rating        INTEGER DEFAULT 3 CHECK (rating BETWEEN 1 AND 5),
    is_active     BOOLEAN DEFAULT TRUE,
    created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    notes         TEXT
);

-- Contact interactions / CRM log
CREATE TABLE contacts (
    contact_id    INTEGER PRIMARY KEY AUTO_INCREMENT,
    customer_id   INTEGER NOT NULL,
    user_id       INTEGER NOT NULL,
    contact_type  VARCHAR(30) NOT NULL CHECK (contact_type IN ('CALL', 'EMAIL', 'MEETING', 'DEMO', 'SUPPORT')),
    subject       VARCHAR(300) NOT NULL,
    body          TEXT,
    contact_date  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    follow_up     DATE,
    FOREIGN KEY (customer_id) REFERENCES customers(customer_id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(user_id)
);

-- Support tickets (uses sequence for ticket numbers)
CREATE TABLE tickets (
    ticket_id   INTEGER PRIMARY KEY,
    customer_id INTEGER NOT NULL,
    assigned_to INTEGER,
    priority    VARCHAR(10) NOT NULL DEFAULT 'MEDIUM' CHECK (priority IN ('LOW', 'MEDIUM', 'HIGH', 'CRITICAL')),
    status      VARCHAR(20) NOT NULL DEFAULT 'OPEN' CHECK (status IN ('OPEN', 'IN_PROGRESS', 'RESOLVED', 'CLOSED')),
    subject     VARCHAR(300) NOT NULL,
    description TEXT,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    resolved_at TIMESTAMP,
    FOREIGN KEY (customer_id) REFERENCES customers(customer_id),
    FOREIGN KEY (assigned_to) REFERENCES users(user_id)
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 4: ERP — PRODUCTS, INVENTORY, ORDERS
-- ════════════════════════════════════════════════════════════════════════════

-- Product categories
CREATE TABLE categories (
    category_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    category_name VARCHAR(100) NOT NULL UNIQUE,
    parent_id     INTEGER,
    description   TEXT,
    FOREIGN KEY (parent_id) REFERENCES categories(category_id)
);

-- Products
CREATE TABLE products (
    product_id    INTEGER PRIMARY KEY AUTO_INCREMENT,
    sku           VARCHAR(50) NOT NULL UNIQUE,
    product_name  VARCHAR(200) NOT NULL,
    category_id   INTEGER,
    unit_price    DECIMAL(10,2) NOT NULL CHECK (unit_price >= 0),
    cost_price    DECIMAL(10,2) NOT NULL CHECK (cost_price >= 0),
    weight_kg     FLOAT,
    is_active     BOOLEAN DEFAULT TRUE,
    created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    metadata      TEXT,
    FOREIGN KEY (category_id) REFERENCES categories(category_id)
);

-- Warehouses
CREATE TABLE warehouses (
    warehouse_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    warehouse_name VARCHAR(100) NOT NULL UNIQUE,
    city           VARCHAR(200),
    capacity       INTEGER NOT NULL DEFAULT 10000,
    is_active      BOOLEAN DEFAULT TRUE
);

-- Inventory (composite FK)
CREATE TABLE inventory (
    inventory_id  INTEGER PRIMARY KEY AUTO_INCREMENT,
    product_id    INTEGER NOT NULL,
    warehouse_id  INTEGER NOT NULL,
    quantity       INTEGER NOT NULL DEFAULT 0 CHECK (quantity >= 0),
    reorder_level  INTEGER DEFAULT 10,
    last_restock   DATE,
    FOREIGN KEY (product_id) REFERENCES products(product_id) ON DELETE CASCADE,
    FOREIGN KEY (warehouse_id) REFERENCES warehouses(warehouse_id),
    UNIQUE (product_id, warehouse_id)
);

-- Orders
CREATE TABLE orders (
    order_id     INTEGER PRIMARY KEY AUTO_INCREMENT,
    customer_id  INTEGER NOT NULL,
    user_id      INTEGER NOT NULL,
    order_date   DATE NOT NULL,
    status       VARCHAR(20) NOT NULL DEFAULT 'PENDING' CHECK (status IN ('PENDING', 'CONFIRMED', 'SHIPPED', 'DELIVERED', 'CANCELLED')),
    total_amount DECIMAL(12,2) DEFAULT 0.00,
    discount_pct FLOAT DEFAULT 0.0,
    notes        TEXT,
    FOREIGN KEY (customer_id) REFERENCES customers(customer_id),
    FOREIGN KEY (user_id) REFERENCES users(user_id)
);

-- Order line items
CREATE TABLE order_items (
    item_id     INTEGER PRIMARY KEY AUTO_INCREMENT,
    order_id    INTEGER NOT NULL,
    product_id  INTEGER NOT NULL,
    quantity    INTEGER NOT NULL CHECK (quantity > 0),
    unit_price  DECIMAL(10,2) NOT NULL,
    discount    DECIMAL(5,2) DEFAULT 0.00,
    FOREIGN KEY (order_id) REFERENCES orders(order_id) ON DELETE CASCADE,
    FOREIGN KEY (product_id) REFERENCES products(product_id)
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 5: FINANCE — INVOICES & PAYMENTS
-- ════════════════════════════════════════════════════════════════════════════

-- Invoices
CREATE TABLE invoices (
    invoice_id  INTEGER PRIMARY KEY,
    order_id    INTEGER NOT NULL,
    customer_id INTEGER NOT NULL,
    invoice_date DATE NOT NULL,
    due_date    DATE NOT NULL,
    amount      DECIMAL(12,2) NOT NULL,
    tax_amount  DECIMAL(10,2) DEFAULT 0.00,
    status      VARCHAR(20) NOT NULL DEFAULT 'PENDING',
    FOREIGN KEY (order_id) REFERENCES orders(order_id),
    FOREIGN KEY (customer_id) REFERENCES customers(customer_id)
);

-- Payments
CREATE TABLE payments (
    payment_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    invoice_id   INTEGER NOT NULL,
    payment_date DATE NOT NULL,
    amount       DECIMAL(12,2) NOT NULL CHECK (amount > 0),
    method       VARCHAR(30) NOT NULL CHECK (method IN ('CREDIT_CARD', 'BANK_TRANSFER', 'CHECK', 'CASH', 'PAYPAL')),
    reference    VARCHAR(100),
    FOREIGN KEY (invoice_id) REFERENCES invoices(invoice_id)
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 6: HR TABLES
-- ════════════════════════════════════════════════════════════════════════════

-- Departments
CREATE TABLE departments (
    dept_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    dept_name VARCHAR(100) NOT NULL UNIQUE,
    manager_id INTEGER,
    budget    DECIMAL(14,2) DEFAULT 0.00,
    FOREIGN KEY (manager_id) REFERENCES users(user_id)
);

-- Employees
CREATE TABLE employees (
    emp_id     INTEGER PRIMARY KEY AUTO_INCREMENT,
    user_id    INTEGER UNIQUE,
    dept_id    INTEGER NOT NULL,
    job_title  VARCHAR(100) NOT NULL,
    salary     DECIMAL(10,2) NOT NULL CHECK (salary > 0),
    hire_date  DATE NOT NULL,
    manager_id INTEGER,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    FOREIGN KEY (dept_id) REFERENCES departments(dept_id),
    FOREIGN KEY (manager_id) REFERENCES employees(emp_id)
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 7: ENCRYPTED TABLES (AES-256-GCM TDE)
-- ════════════════════════════════════════════════════════════════════════════

-- Audit log — full table encryption
CREATE TABLE audit_log ENCRYPTED (
    log_id     INTEGER PRIMARY KEY,
    user_id    INTEGER,
    event_type VARCHAR(50) NOT NULL,
    table_name VARCHAR(100),
    record_id  INTEGER,
    old_values TEXT ENCRYPTED,
    new_values TEXT ENCRYPTED,
    ip_address VARCHAR(45),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Credentials vault — column-level encryption for secrets
CREATE TABLE credentials_vault ENCRYPTED (
    vault_id    INTEGER PRIMARY KEY AUTO_INCREMENT,
    owner       VARCHAR(100) NOT NULL,
    service     VARCHAR(100) NOT NULL,
    secret_key  VARCHAR(500) ENCRYPTED,
    api_token   TEXT ENCRYPTED,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at  TIMESTAMP
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 8: PARTITIONED TABLES (standalone demo — no FK references)
-- ════════════════════════════════════════════════════════════════════════════

-- RANGE partition: Sales metrics by quarter
CREATE TABLE sales_metrics (
    metric_id    INTEGER PRIMARY KEY AUTO_INCREMENT,
    metric_date  DATE NOT NULL,
    region       VARCHAR(20) NOT NULL,
    revenue      DECIMAL(12,2) DEFAULT 0,
    units_sold   INTEGER DEFAULT 0
) PARTITION BY RANGE (metric_date) (
    PARTITION p_2025_q1 VALUES LESS THAN ('2025-04-01'),
    PARTITION p_2025_q2 VALUES LESS THAN ('2025-07-01'),
    PARTITION p_2025_q3 VALUES LESS THAN ('2025-10-01'),
    PARTITION p_2025_q4 VALUES LESS THAN ('2026-01-01'),
    PARTITION p_2026_q1 VALUES LESS THAN ('2026-04-01'),
    PARTITION p_future  VALUES LESS THAN MAXVALUE
);

-- LIST partition: Event log by severity
CREATE TABLE event_log (
    event_id     INTEGER PRIMARY KEY AUTO_INCREMENT,
    severity     VARCHAR(10) NOT NULL,
    source       VARCHAR(50) NOT NULL,
    message      TEXT,
    event_time   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) PARTITION BY LIST (severity) (
    PARTITION p_debug    VALUES IN ('DEBUG'),
    PARTITION p_info     VALUES IN ('INFO'),
    PARTITION p_warn     VALUES IN ('WARN'),
    PARTITION p_error    VALUES IN ('ERROR'),
    PARTITION p_critical VALUES IN ('CRITICAL')
);

-- HASH partition: Session data distributed by user_id
CREATE TABLE session_data (
    session_id   INTEGER PRIMARY KEY AUTO_INCREMENT,
    user_id      INTEGER NOT NULL,
    token        VARCHAR(100) NOT NULL,
    ip_address   VARCHAR(45),
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at   TIMESTAMP
) PARTITION BY HASH (user_id) (
    PARTITION p0 VALUES WITH (MODULUS 4, REMAINDER 0),
    PARTITION p1 VALUES WITH (MODULUS 4, REMAINDER 1),
    PARTITION p2 VALUES WITH (MODULUS 4, REMAINDER 2),
    PARTITION p3 VALUES WITH (MODULUS 4, REMAINDER 3)
);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 9: INDEXES
-- ════════════════════════════════════════════════════════════════════════════

-- B+Tree indexes (default)
CREATE INDEX idx_customers_company ON customers USING BTREE (company_name);
CREATE INDEX idx_customers_country ON customers (country);
CREATE INDEX idx_orders_date ON orders USING BTREE (order_date);
CREATE INDEX idx_orders_status ON orders (status);
CREATE INDEX idx_order_items_order ON order_items (order_id);
CREATE INDEX idx_employees_dept ON employees (dept_id);
CREATE INDEX idx_contacts_customer ON contacts (customer_id);
CREATE INDEX idx_tickets_status ON tickets (status);
CREATE INDEX idx_invoices_due ON invoices (due_date);
CREATE INDEX idx_products_category ON products (category_id);
CREATE INDEX idx_audit_log_user ON audit_log (user_id);
CREATE INDEX idx_audit_log_table ON audit_log (table_name);

-- Hash indexes for exact-match lookups
CREATE INDEX idx_users_email_hash ON users USING HASH (email);
CREATE INDEX idx_products_sku_hash ON products USING HASH (sku);
CREATE INDEX idx_customers_postal_hash ON customers USING HASH (postal_code);

-- Unique indexes
CREATE UNIQUE INDEX idx_users_username ON users (username);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 9: SEED DATA — ROLES & PERMISSIONS
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO roles (role_name, description) VALUES
    ('ADMIN',       'Full system administrator'),
    ('MANAGER',     'Department manager'),
    ('SALES_REP',   'Sales representative'),
    ('SUPPORT',     'Customer support agent'),
    ('FINANCE',     'Finance and billing'),
    ('HR',          'Human resources'),
    ('READONLY',    'Read-only access');

INSERT INTO permissions (perm_name, resource, action_type) VALUES
    ('users.create',    'users',    'CREATE'),
    ('users.read',      'users',    'READ'),
    ('users.update',    'users',    'UPDATE'),
    ('users.delete',    'users',    'DELETE'),
    ('orders.create',   'orders',   'CREATE'),
    ('orders.read',     'orders',   'READ'),
    ('orders.update',   'orders',   'UPDATE'),
    ('orders.delete',   'orders',   'DELETE'),
    ('products.create', 'products', 'CREATE'),
    ('products.read',   'products', 'READ'),
    ('products.update', 'products', 'UPDATE'),
    ('invoices.read',   'invoices', 'READ'),
    ('invoices.create', 'invoices', 'CREATE'),
    ('reports.read',    'reports',  'READ'),
    ('audit.read',      'audit',    'READ');

-- ADMIN gets all permissions
INSERT INTO role_permissions (role_id, perm_id) VALUES
    (1, 1), (1, 2), (1, 3), (1, 4), (1, 5), (1, 6), (1, 7), (1, 8),
    (1, 9), (1, 10), (1, 11), (1, 12), (1, 13), (1, 14), (1, 15);

-- MANAGER gets most permissions
INSERT INTO role_permissions (role_id, perm_id) VALUES
    (2, 2), (2, 3), (2, 5), (2, 6), (2, 7), (2, 10), (2, 12), (2, 14);

-- SALES_REP permissions
INSERT INTO role_permissions (role_id, perm_id) VALUES (3, 5), (3, 6), (3, 7), (3, 10);

-- SUPPORT permissions
INSERT INTO role_permissions (role_id, perm_id) VALUES (4, 6), (4, 10);

-- FINANCE permissions
INSERT INTO role_permissions (role_id, perm_id) VALUES (5, 6), (5, 10), (5, 12), (5, 13), (5, 14);

-- READONLY
INSERT INTO role_permissions (role_id, perm_id) VALUES (7, 2), (7, 6), (7, 10), (7, 12), (7, 14);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 10: SEED DATA — USERS (20 users)
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO users (username, email, password_hash, full_name, phone, is_active) VALUES
    ('admin',       'admin@acme.com',       SHA256('admin2024!'),     'System Administrator', '+1-555-0100', TRUE),
    ('jdoe',        'john.doe@acme.com',    SHA256('Jd0e!pass'),      'John Doe',             '+1-555-0101', TRUE),
    ('asmith',      'alice.smith@acme.com',  SHA256('Al1ce$mith'),     'Alice Smith',          '+1-555-0102', TRUE),
    ('bwilson',     'bob.wilson@acme.com',   SHA256('B0b!wilson'),     'Bob Wilson',           '+1-555-0103', TRUE),
    ('cjohnson',    'carol.johnson@acme.com', SHA256('Car0l#j'),       'Carol Johnson',        '+1-555-0104', TRUE),
    ('dlee',        'david.lee@acme.com',    SHA256('Dav1d!lee'),      'David Lee',            '+1-555-0105', TRUE),
    ('emartinez',   'elena.martinez@acme.com', SHA256('El3na@m'),      'Elena Martinez',       '+1-555-0106', TRUE),
    ('fgarcia',     'frank.garcia@acme.com', SHA256('Frank!g2'),       'Frank Garcia',         '+1-555-0107', TRUE),
    ('gkim',        'grace.kim@acme.com',    SHA256('Grac3!kim'),      'Grace Kim',            '+1-555-0108', TRUE),
    ('hpatel',      'hiro.patel@acme.com',   SHA256('H1ro#pat'),       'Hiro Patel',           '+1-555-0109', TRUE),
    ('ijones',      'ivy.jones@acme.com',    SHA256('Ivy!j0nes'),      'Ivy Jones',            '+1-555-0110', TRUE),
    ('kbrown',      'kevin.brown@acme.com',  SHA256('K3v1n!b'),        'Kevin Brown',          '+1-555-0111', TRUE),
    ('lchen',       'linda.chen@acme.com',   SHA256('L1nda!ch'),       'Linda Chen',           '+1-555-0112', TRUE),
    ('mross',       'mike.ross@acme.com',    SHA256('M1ke!ross'),      'Mike Ross',            '+1-555-0113', TRUE),
    ('nwong',       'nancy.wong@acme.com',   SHA256('N4ncy!w'),        'Nancy Wong',           '+1-555-0114', TRUE),
    ('oanderson',   'oscar.anderson@acme.com', SHA256('0scar!a'),      'Oscar Anderson',       '+1-555-0115', TRUE),
    ('ptaylor',     'pat.taylor@acme.com',   SHA256('P4t!tayl'),       'Pat Taylor',           '+1-555-0116', TRUE),
    ('qnguyen',     'quinn.nguyen@acme.com', SHA256('Qu1nn!ng'),       'Quinn Nguyen',         '+1-555-0117', TRUE),
    ('rlopez',      'rosa.lopez@acme.com',   SHA256('R0sa!lop'),       'Rosa Lopez',           '+1-555-0118', TRUE),
    ('sthomas',     'sam.thomas@acme.com',   SHA256('S4m!thom'),       'Sam Thomas',           '+1-555-0119', TRUE);

-- Assign roles to users
INSERT INTO user_roles (user_id, role_id) VALUES (1, 1), (2, 2), (3, 3), (4, 3), (5, 2);
INSERT INTO user_roles (user_id, role_id) VALUES (6, 4), (7, 3), (8, 5), (9, 4), (10, 6);
INSERT INTO user_roles (user_id, role_id) VALUES (11, 3), (12, 4), (13, 5), (14, 3), (15, 2);
INSERT INTO user_roles (user_id, role_id) VALUES (16, 4), (17, 3), (18, 6), (19, 5), (20, 7);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 11: SEED DATA — DEPARTMENTS & EMPLOYEES
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO departments (dept_name, manager_id, budget) VALUES
    ('Engineering',     2,  500000.00),
    ('Sales',           5,  350000.00),
    ('Support',         15, 200000.00),
    ('Finance',         8,  180000.00),
    ('Human Resources', 10, 150000.00),
    ('Marketing',       5,  250000.00);

-- Insert employees in order: first those with no manager, then those referencing existing emp_ids
INSERT INTO employees (user_id, dept_id, job_title, salary, hire_date, manager_id) VALUES
    (1,  1, 'CTO',                    185000.00, '2020-01-15', NULL);

INSERT INTO employees (user_id, dept_id, job_title, salary, hire_date, manager_id) VALUES
    (2,  1, 'VP Engineering',         165000.00, '2020-03-01', 1),
    (5,  2, 'Sales Director',         140000.00, '2020-09-01', 1),
    (8,  4, 'Finance Director',       135000.00, '2020-11-01', 1),
    (10, 5, 'HR Director',            125000.00, '2021-01-15', 1),
    (15, 6, 'Marketing Director',     130000.00, '2021-04-01', 1);

-- Tier 3: leads and ICs reporting to directors (emp_ids 2-6)
INSERT INTO employees (user_id, dept_id, job_title, salary, hire_date, manager_id) VALUES
    (9,  3, 'Support Lead',           105000.00, '2021-02-01', 2),
    (3,  2, 'Account Executive',       95000.00, '2021-06-15', 3),
    (4,  2, 'Account Executive',       92000.00, '2021-08-01', 3),
    (7,  2, 'Sales Representative',    85000.00, '2022-04-01', 3),
    (11, 2, 'Sales Representative',    82000.00, '2022-07-01', 3),
    (14, 2, 'Account Executive',       90000.00, '2022-11-01', 3),
    (17, 2, 'Sales Representative',    80000.00, '2023-03-01', 3),
    (13, 4, 'Financial Analyst',       88000.00, '2022-03-01', 4),
    (19, 4, 'Accounts Payable',        68000.00, '2023-06-15', 4),
    (18, 5, 'HR Specialist',           72000.00, '2023-05-01', 5),
    (20, 1, 'Software Engineer',       110000.00, '2023-08-01', 2);

-- Tier 4: ICs reporting to tier 3 leads (emp_id 7 = Support Lead)
INSERT INTO employees (user_id, dept_id, job_title, salary, hire_date, manager_id) VALUES
    (6,  3, 'Support Engineer',        78000.00, '2022-01-10', 7),
    (12, 3, 'Support Engineer',        76000.00, '2022-09-15', 7),
    (16, 3, 'Support Engineer',        77000.00, '2023-01-15', 7);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 12: SEED DATA — CUSTOMERS (25 customers)
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO customers (company_name, contact_name, contact_email, phone, address, city, region, country, postal_code, credit_limit, rating) VALUES
    ('Globex Corporation',    'Hank Scorpio',   'hank@globex.com',     '+1-555-1001', '123 Innovation Dr',  'Austin',        'TX', 'US', '78701', 50000.00, 5),
    ('Initech',               'Bill Lumbergh',   'bill@initech.com',    '+1-555-1002', '456 Corporate Blvd', 'Houston',       'TX', 'US', '77001', 30000.00, 3),
    ('Umbrella Corp',         'Albert Wesker',   'wesker@umbrella.com', '+1-555-1003', '789 Biotech Pk',     'Raccoon City',  'MO', 'US', '65201', 75000.00, 4),
    ('Stark Industries',      'Pepper Potts',    'pepper@stark.com',    '+1-555-1004', '10880 Malibu Pt',    'Malibu',        'CA', 'US', '90265', 100000.00, 5),
    ('Wayne Enterprises',     'Lucius Fox',      'lucius@wayne.com',    '+1-555-1005', '1007 Mountain Dr',   'Gotham',        'NJ', 'US', '07001', 100000.00, 5),
    ('Cyberdyne Systems',     'Miles Dyson',     'miles@cyberdyne.com', '+1-555-1006', '18144 El Camino',    'Sunnyvale',     'CA', 'US', '94086', 60000.00, 4),
    ('Soylent Corp',          'Steven Lerner',   'steve@soylent.com',   '+1-555-1007', '2025 Green Way',     'New York',      'NY', 'US', '10001', 25000.00, 2),
    ('Acme Corp',             'Wile E. Coyote',  'wile@acme.com',       '+1-555-1008', '100 Desert Rd',      'Phoenix',       'AZ', 'US', '85001', 40000.00, 3),
    ('Oscorp Industries',     'Norman Osborn',   'norman@oscorp.com',   '+1-555-1009', '888 5th Avenue',     'New York',      'NY', 'US', '10022', 80000.00, 4),
    ('Wonka Industries',      'Charlie Bucket',  'charlie@wonka.com',   '+44-20-5010', '1 Chocolate Ln',     'London',        NULL, 'GB', 'EC1A', 35000.00, 4),
    ('Dunder Mifflin',        'Michael Scott',   'michael@dunderm.com', '+1-555-1011', '1725 Slough Ave',    'Scranton',      'PA', 'US', '18503', 15000.00, 3),
    ('Massive Dynamic',       'Nina Sharp',      'nina@massdyn.com',    '+1-555-1012', '1 Centre St',        'New York',      'NY', 'US', '10007', 90000.00, 5),
    ('Pied Piper',            'Richard Hendricks','richard@piedp.com',  '+1-555-1013', '5230 Newell Rd',     'Palo Alto',     'CA', 'US', '94303', 20000.00, 3),
    ('Hooli',                 'Gavin Belson',    'gavin@hooli.com',     '+1-555-1014', '1 Hooli Way',        'Mountain View', 'CA', 'US', '94043', 70000.00, 2),
    ('Tyrell Corporation',    'Eldon Tyrell',    'eldon@tyrell.com',    '+1-555-1015', '2019 Nexus Blvd',    'Los Angeles',   'CA', 'US', '90012', 95000.00, 5),
    ('Weyland-Yutani',        'Carter Burke',    'burke@weyland.com',   '+1-555-1016', '426 LV Ave',         'San Diego',     'CA', 'US', '92101', 85000.00, 4),
    ('Nakatomi Trading',      'Joseph Takagi',   'takagi@nakatomi.com', '+81-3-5017',  '33-1 Nakatomi Plz',  'Tokyo',         NULL, 'JP', '100-0001', 55000.00, 4),
    ('Rekall Inc',            'Douglas Quaid',   'doug@rekall.com',     '+1-555-1018', '200 Memory Ln',      'Chicago',       'IL', 'US', '60601', 30000.00, 3),
    ('Virtucon',              'Dr. Evil',        'evil@virtucon.com',   '+1-555-1019', '1 Evil Way',         'Las Vegas',     'NV', 'US', '89101', 45000.00, 2),
    ('InGen',                 'John Hammond',    'john@ingen.com',      '+506-5020',   'Isla Nublar',        'San Jose',      NULL, 'CR', '10101', 65000.00, 3),
    ('Bluth Company',         'Michael Bluth',   'michael@bluth.com',   '+1-555-1021', '1 Lucille Ln',       'Newport Beach', 'CA', 'US', '92660', 12000.00, 2),
    ('Sterling Cooper',       'Don Draper',      'don@sterlingc.com',   '+1-555-1022', '405 Madison Ave',    'New York',      'NY', 'US', '10022', 42000.00, 4),
    ('MomCorp',               'Mom',             'mom@momcorp.com',     '+1-555-1023', '3000 Future St',     'New York',      'NY', 'US', '10001', 88000.00, 3),
    ('Planet Express',        'Hubert Farnsworth','prof@planex.com',    '+1-555-1024', '57th St & 1st Ave',  'New York',      'NY', 'US', '10019', 18000.00, 3),
    ('Los Pollos Hermanos',   'Gustavo Fring',   'gus@lospollos.com',  '+1-505-1025', '308 Negra Arroyo',   'Albuquerque',   'NM', 'US', '87104', 55000.00, 5);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 13: SEED DATA — CATEGORIES & PRODUCTS (20 products)
-- ════════════════════════════════════════════════════════════════════════════

-- Insert parent categories first (no parent_id FK)
INSERT INTO categories (category_name, parent_id, description) VALUES
    ('Software',      NULL, 'Software products and licenses'),
    ('Hardware',      NULL, 'Physical hardware products'),
    ('Services',      NULL, 'Professional services');

-- Insert child categories (reference parent_id = 1, 2, 3)
INSERT INTO categories (category_name, parent_id, description) VALUES
    ('Cloud',         1,    'Cloud-based software'),
    ('On-Premise',    1,    'On-premise software licenses'),
    ('Networking',    2,    'Network equipment'),
    ('Compute',       2,    'Servers and compute hardware'),
    ('Consulting',    3,    'Consulting engagements'),
    ('Training',      3,    'Training courses');

INSERT INTO products (sku, product_name, category_id, unit_price, cost_price, weight_kg) VALUES
    ('SW-ERP-ENT',  'ERP Enterprise Suite',       5,  4999.99, 2000.00, NULL),
    ('SW-ERP-STD',  'ERP Standard Edition',        5,  1999.99,  800.00, NULL),
    ('SW-CRM-PRO',  'CRM Professional',            5,  2499.99, 1000.00, NULL),
    ('SW-CRM-BAS',  'CRM Basic',                   5,   999.99,  400.00, NULL),
    ('CL-DB-100',   'CloudDB 100GB',               4,   299.99,  100.00, NULL),
    ('CL-DB-500',   'CloudDB 500GB',               4,   799.99,  250.00, NULL),
    ('CL-STOR-1T',  'Cloud Storage 1TB',           4,   149.99,   50.00, NULL),
    ('HW-SRV-R1',   'Rack Server R1',              7, 12999.99, 8000.00, 25.5),
    ('HW-SRV-R2',   'Rack Server R2',              7, 24999.99, 15000.00, 32.0),
    ('HW-SW-48P',   'Managed Switch 48-Port',      6,  1899.99, 1100.00, 4.2),
    ('HW-FW-X1',    'Firewall Appliance X1',       6,  3499.99, 2000.00, 3.8),
    ('HW-AP-AC',    'WiFi 6 Access Point',         6,   449.99,  200.00, 0.9),
    ('SV-CON-DAY',  'Consulting - Day Rate',       8,  2500.00, 1200.00, NULL),
    ('SV-CON-WK',   'Consulting - Week Rate',      8, 10000.00, 5000.00, NULL),
    ('SV-TRN-BAS',  'Training - Basic (3-day)',     9,  3000.00, 1500.00, NULL),
    ('SV-TRN-ADV',  'Training - Advanced (5-day)',  9,  5500.00, 2800.00, NULL),
    ('SW-SEC-EDR',  'Endpoint Security EDR',       5,   799.99,  300.00, NULL),
    ('CL-AI-ML',    'AI/ML Platform License',      4,  1499.99,  500.00, NULL),
    ('HW-NAS-4B',   'NAS 4-Bay Appliance',         7,  1299.99,  750.00, 5.6),
    ('SW-MON-APM',  'Application Monitoring APM',  4,   599.99,  200.00, NULL);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 14: SEED DATA — WAREHOUSES & INVENTORY
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO warehouses (warehouse_name, city, capacity) VALUES
    ('West Coast DC',  'Los Angeles, CA',  50000),
    ('East Coast DC',  'Newark, NJ',       40000),
    ('Central DC',     'Dallas, TX',       30000);

-- Stock physical products in warehouses
INSERT INTO inventory (product_id, warehouse_id, quantity, reorder_level, last_restock) VALUES
    (8,  1, 45, 10, '2025-12-01'),
    (8,  2, 30, 10, '2025-11-15'),
    (8,  3, 20,  5, '2025-11-20'),
    (9,  1, 15,  5, '2025-12-05'),
    (9,  2, 12,  5, '2025-11-28'),
    (10, 1, 120, 25, '2026-01-10'),
    (10, 2, 85,  25, '2025-12-20'),
    (10, 3, 60,  20, '2025-12-18'),
    (11, 1, 40, 10, '2026-01-05'),
    (11, 2, 35, 10, '2025-12-22'),
    (12, 1, 200, 50, '2026-01-12'),
    (12, 2, 150, 50, '2026-01-08'),
    (12, 3, 100, 30, '2025-12-30'),
    (19, 1, 55,  15, '2026-01-15'),
    (19, 2, 40,  15, '2026-01-10');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 15: SEED DATA — ORDERS & ORDER ITEMS (30 orders, 75+ line items)
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO orders (customer_id, user_id, order_date, status, total_amount, discount_pct) VALUES
    (1,  3,  '2025-01-15', 'DELIVERED',  17499.96, 5.0),
    (2,  4,  '2025-01-22', 'DELIVERED',   1999.99, 0.0),
    (3,  7,  '2025-02-10', 'DELIVERED',  28499.97, 10.0),
    (4,  3,  '2025-02-28', 'DELIVERED',  54999.97, 8.0),
    (5,  14, '2025-03-05', 'DELIVERED',  12999.99, 0.0),
    (6,  11, '2025-03-18', 'DELIVERED',   6499.97, 3.0),
    (1,  3,  '2025-04-02', 'DELIVERED',   4999.99, 5.0),
    (7,  4,  '2025-04-15', 'DELIVERED',   2999.98, 0.0),
    (8,  7,  '2025-04-28', 'SHIPPED',     9399.94, 0.0),
    (9,  14, '2025-05-05', 'SHIPPED',    37999.98, 12.0),
    (10, 11, '2025-05-20', 'CONFIRMED',   3499.99, 0.0),
    (4,  3,  '2025-06-01', 'CONFIRMED',  10000.00, 0.0),
    (12, 7,  '2025-06-15', 'PENDING',    24999.99, 5.0),
    (13, 4,  '2025-07-01', 'PENDING',     1499.98, 0.0),
    (14, 17, '2025-07-10', 'PENDING',     5999.98, 0.0),
    (15, 3,  '2025-08-01', 'DELIVERED',  49999.98, 15.0),
    (16, 14, '2025-08-15', 'DELIVERED',   7799.97, 0.0),
    (17, 11, '2025-09-01', 'SHIPPED',     8999.98, 5.0),
    (18, 7,  '2025-09-20', 'CONFIRMED',   3000.00, 0.0),
    (19, 4,  '2025-10-05', 'PENDING',     1299.99, 0.0),
    (20, 17, '2025-10-15', 'CANCELLED',   1999.99, 0.0),
    (5,  3,  '2025-11-01', 'DELIVERED',  15999.98, 10.0),
    (22, 14, '2025-11-15', 'SHIPPED',     5500.00, 0.0),
    (23, 11, '2025-12-01', 'CONFIRMED',   2499.99, 0.0),
    (24, 7,  '2025-12-10', 'PENDING',     5999.97, 0.0),
    (25, 4,  '2026-01-05', 'CONFIRMED',  12999.99, 5.0),
    (1,  3,  '2026-01-20', 'PENDING',    29999.98, 8.0),
    (3,  7,  '2026-02-01', 'PENDING',     2999.97, 0.0),
    (4,  14, '2026-02-15', 'PENDING',    10000.00, 0.0),
    (12, 3,  '2026-03-01', 'PENDING',    24999.99, 5.0);

-- Order line items
INSERT INTO order_items (order_id, product_id, quantity, unit_price, discount) VALUES
    (1, 1, 1, 4999.99, 0.00), (1, 3, 1, 2499.99, 0.00), (1, 5, 1, 299.99, 0.00),
    (2, 2, 1, 1999.99, 0.00),
    (3, 8, 2, 12999.99, 500.00), (3, 10, 1, 1899.99, 0.00),
    (4, 9, 2, 24999.99, 1000.00), (4, 11, 1, 3499.99, 0.00),
    (5, 8, 1, 12999.99, 0.00),
    (6, 17, 4, 799.99, 0.00), (6, 20, 5, 599.99, 0.00),
    (7, 1, 1, 4999.99, 0.00),
    (8, 4, 2, 999.99, 0.00), (8, 7, 1, 149.99, 0.00),
    (9, 10, 3, 1899.99, 0.00), (9, 11, 1, 3499.99, 0.00),
    (10, 9, 1, 24999.99, 0.00), (10, 18, 5, 1499.99, 0.00),
    (11, 11, 1, 3499.99, 0.00),
    (12, 14, 1, 10000.00, 0.00),
    (13, 9, 1, 24999.99, 0.00),
    (14, 18, 1, 1499.99, 0.00),
    (15, 3, 1, 2499.99, 0.00), (15, 6, 1, 799.99, 0.00),
    (16, 9, 2, 24999.99, 0.00),
    (17, 8, 1, 12999.99, 0.00), (17, 19, 2, 1299.99, 0.00),
    (18, 10, 2, 1899.99, 0.00), (18, 12, 10, 449.99, 0.00),
    (19, 13, 1, 2500.00, 0.00),
    (20, 19, 1, 1299.99, 0.00),
    (21, 2, 1, 1999.99, 0.00),
    (22, 8, 1, 12999.99, 0.00), (22, 11, 1, 3499.99, 0.00),
    (23, 16, 1, 5500.00, 0.00),
    (24, 3, 1, 2499.99, 0.00),
    (25, 5, 1, 299.99, 0.00), (25, 6, 1, 799.99, 0.00), (25, 20, 3, 599.99, 0.00),
    (26, 8, 1, 12999.99, 0.00),
    (27, 9, 1, 24999.99, 0.00), (27, 1, 1, 4999.99, 0.00),
    (28, 17, 3, 799.99, 0.00), (28, 20, 2, 599.99, 0.00),
    (29, 14, 1, 10000.00, 0.00),
    (30, 9, 1, 24999.99, 0.00);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 16: SEED DATA — INVOICES & PAYMENTS
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO invoices (invoice_id, order_id, customer_id, invoice_date, due_date, amount, tax_amount, status) VALUES
    (NEXTVAL('seq_invoice_no'), 1,  1,  '2025-01-16', '2025-02-15', 17499.96, 1443.75, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 2,  2,  '2025-01-23', '2025-02-22', 1999.99,  165.00,  'PAID'),
    (NEXTVAL('seq_invoice_no'), 3,  3,  '2025-02-11', '2025-03-13', 28499.97, 2351.25, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 4,  4,  '2025-03-01', '2025-03-31', 54999.97, 4537.50, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 5,  5,  '2025-03-06', '2025-04-05', 12999.99, 1072.50, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 6,  6,  '2025-03-19', '2025-04-18', 6499.97,  536.25,  'PAID'),
    (NEXTVAL('seq_invoice_no'), 7,  1,  '2025-04-03', '2025-05-03', 4999.99,  412.50,  'PAID'),
    (NEXTVAL('seq_invoice_no'), 8,  7,  '2025-04-16', '2025-05-16', 2999.98,  247.50,  'PAID'),
    (NEXTVAL('seq_invoice_no'), 9,  8,  '2025-04-29', '2025-05-29', 9399.94,  775.50,  'SENT'),
    (NEXTVAL('seq_invoice_no'), 10, 9,  '2025-05-06', '2025-06-05', 37999.98, 3135.00, 'SENT'),
    (NEXTVAL('seq_invoice_no'), 16, 15, '2025-08-02', '2025-09-01', 49999.98, 4125.00, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 17, 16, '2025-08-16', '2025-09-15', 7799.97,  643.50,  'OVERDUE'),
    (NEXTVAL('seq_invoice_no'), 22, 5,  '2025-11-02', '2025-12-02', 15999.98, 1320.00, 'PAID'),
    (NEXTVAL('seq_invoice_no'), 26, 25, '2026-01-06', '2026-02-05', 12999.99, 1072.50, 'PENDING'),
    (NEXTVAL('seq_invoice_no'), 27, 1,  '2026-01-21', '2026-02-20', 29999.98, 2475.00, 'PENDING');

-- Payments for paid invoices
-- Payments for paid invoices (invoice_ids 1-8 are PAID, 11 is PAID, 13 is PAID)
INSERT INTO payments (invoice_id, payment_date, amount, method, reference) VALUES
    (100000, '2025-02-10', 17499.96, 'BANK_TRANSFER', 'WT-2025-00451'),
    (100001, '2025-02-20', 1999.99,  'CREDIT_CARD',   'CC-8823-1122'),
    (100002, '2025-03-10', 28499.97, 'BANK_TRANSFER', 'WT-2025-00612'),
    (100003, '2025-03-25', 54999.97, 'BANK_TRANSFER', 'WT-2025-00734'),
    (100004, '2025-04-01', 12999.99, 'CHECK',         'CHK-44521'),
    (100005, '2025-04-15', 6499.97,  'CREDIT_CARD',   'CC-9912-3344'),
    (100006, '2025-04-30', 4999.99,  'PAYPAL',        'PP-TXN-88271'),
    (100007, '2025-05-10', 2999.98,  'CREDIT_CARD',   'CC-6677-8899'),
    (100010, '2025-08-28', 49999.98, 'BANK_TRANSFER', 'WT-2025-01455'),
    (100012, '2025-11-20', 15999.98, 'BANK_TRANSFER', 'WT-2025-01892');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 17: SEED DATA — SUPPORT TICKETS
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO tickets (ticket_id, customer_id, assigned_to, priority, status, subject, description, created_at, resolved_at) VALUES
    (NEXTVAL('seq_ticket_id'), 2,  6,  'HIGH',     'RESOLVED', 'ERP login failure',           'Users unable to login after update',             '2025-02-01 09:30:00', '2025-02-01 14:00:00'),
    (NEXTVAL('seq_ticket_id'), 3,  9,  'CRITICAL', 'RESOLVED', 'Server overheating',          'R1 server thermal shutdown in rack 4A',          '2025-02-15 11:00:00', '2025-02-15 16:30:00'),
    (NEXTVAL('seq_ticket_id'), 1,  12, 'MEDIUM',   'RESOLVED', 'CRM report not generating',   'Monthly sales report times out',                 '2025-03-01 10:00:00', '2025-03-02 09:00:00'),
    (NEXTVAL('seq_ticket_id'), 8,  6,  'LOW',      'CLOSED',   'Feature request: dark mode',  'Customer requests dark mode for CRM interface',  '2025-03-10 14:00:00', '2025-03-10 14:30:00'),
    (NEXTVAL('seq_ticket_id'), 4,  9,  'HIGH',     'RESOLVED', 'Firewall rule conflict',      'New rules blocking internal API traffic',         '2025-04-05 08:15:00', '2025-04-05 12:00:00'),
    (NEXTVAL('seq_ticket_id'), 6,  16, 'MEDIUM',   'IN_PROGRESS', 'CloudDB slow queries',     'SELECT queries taking 30+ seconds',              '2025-05-20 13:00:00', NULL),
    (NEXTVAL('seq_ticket_id'), 12, 6,  'HIGH',     'OPEN',     'Data import failure',          'CSV import crashes on large files (>1GB)',        '2025-06-01 09:00:00', NULL),
    (NEXTVAL('seq_ticket_id'), 15, 12, 'CRITICAL', 'IN_PROGRESS', 'Security audit findings',  'Penetration test found 3 critical vulnerabilities', '2025-07-01 08:00:00', NULL),
    (NEXTVAL('seq_ticket_id'), 9,  9,  'LOW',      'OPEN',     'Docs need updating',          'API documentation outdated for v2.1',            '2025-08-15 11:00:00', NULL),
    (NEXTVAL('seq_ticket_id'), 25, 16, 'MEDIUM',   'OPEN',     'Billing discrepancy',         'Invoice amount does not match PO',               '2026-01-10 10:30:00', NULL);

-- ════════════════════════════════════════════════════════════════════════════
-- PART 18: SEED DATA — CONTACTS / CRM INTERACTIONS
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO contacts (customer_id, user_id, contact_type, subject, body, contact_date, follow_up) VALUES
    (1,  3,  'CALL',    'Initial outreach',             'Discussed ERP needs for Q1',                '2025-01-05 10:00:00', '2025-01-12'),
    (1,  3,  'DEMO',    'ERP Enterprise demo',          'Full demo of ERP suite, very interested',   '2025-01-10 14:00:00', '2025-01-15'),
    (2,  4,  'EMAIL',   'Follow up on proposal',        'Sent pricing for ERP Standard',             '2025-01-18 09:00:00', '2025-01-25'),
    (3,  7,  'MEETING', 'Infrastructure review',        'Reviewed server requirements',              '2025-02-05 11:00:00', '2025-02-10'),
    (4,  3,  'DEMO',    'Full product showcase',        'Demonstrated server and security products', '2025-02-20 10:00:00', '2025-02-28'),
    (5,  14, 'CALL',    'Server quote request',         'Wayne needs R1 server for new DC',          '2025-03-01 09:00:00', '2025-03-05'),
    (6,  11, 'EMAIL',   'Security product inquiry',     'Interested in EDR + monitoring bundle',     '2025-03-10 15:00:00', '2025-03-18'),
    (10, 11, 'CALL',    'International support setup',  'Discussing EU support hours',               '2025-05-15 10:00:00', '2025-05-22'),
    (12, 7,  'MEETING', 'Quarterly review',             'Reviewed usage and expansion plans',        '2025-06-10 14:00:00', '2025-06-17'),
    (15, 3,  'DEMO',    'New server line demo',         'R2 server showcase for Tyrell',             '2025-07-25 11:00:00', '2025-08-01'),
    (25, 4,  'CALL',    'Onboarding call',              'New customer onboarding for Los Pollos',    '2025-12-20 10:00:00', '2026-01-05'),
    (4,  3,  'SUPPORT', 'Post-delivery check-in',       'Verifying all equipment operational',       '2025-04-10 09:00:00', NULL),
    (1,  3,  'EMAIL',   'Renewal reminder',             'ERP license renewal approaching Q1 2026',   '2025-12-01 08:00:00', '2025-12-15'),
    (22, 14, 'MEETING', 'Ad campaign platform review',  'Sterling Cooper evaluating training opts',  '2025-11-10 13:00:00', '2025-11-20'),
    (13, 4,  'CALL',    'AI/ML platform interest',      'Pied Piper expanding ML infrastructure',    '2025-06-28 11:00:00', '2025-07-01');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 19: SEED DATA — AUDIT LOG
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO audit_log (log_id, user_id, event_type, table_name, record_id, old_values, new_values, ip_address, created_at) VALUES
    (NEXTVAL('seq_audit_id'), 1,  'CREATE', 'users',     1,  NULL, '{"username":"admin"}',       '10.0.0.1',   '2025-01-01 08:00:00'),
    (NEXTVAL('seq_audit_id'), 1,  'CREATE', 'roles',     1,  NULL, '{"role_name":"ADMIN"}',      '10.0.0.1',   '2025-01-01 08:01:00'),
    (NEXTVAL('seq_audit_id'), 1,  'CREATE', 'users',     2,  NULL, '{"username":"jdoe"}',        '10.0.0.1',   '2025-01-02 09:00:00'),
    (NEXTVAL('seq_audit_id'), 3,  'CREATE', 'orders',    1,  NULL, '{"customer_id":1}',          '10.0.1.10',  '2025-01-15 10:30:00'),
    (NEXTVAL('seq_audit_id'), 3,  'UPDATE', 'orders',    1,  '{"status":"PENDING"}', '{"status":"CONFIRMED"}', '10.0.1.10',  '2025-01-15 11:00:00'),
    (NEXTVAL('seq_audit_id'), 6,  'UPDATE', 'tickets',   10000, '{"status":"OPEN"}', '{"status":"RESOLVED"}',  '10.0.2.5',   '2025-02-01 14:00:00'),
    (NEXTVAL('seq_audit_id'), 1,  'DELETE', 'permissions', 99, '{"perm_name":"legacy.write"}', NULL,           '10.0.0.1',   '2025-03-01 12:00:00'),
    (NEXTVAL('seq_audit_id'), 8,  'CREATE', 'invoices',  100000, NULL, '{"amount":17499.96}',    '10.0.3.20',  '2025-01-16 09:00:00'),
    (NEXTVAL('seq_audit_id'), 10, 'UPDATE', 'employees', 6,  '{"salary":75000}', '{"salary":78000}',          '10.0.4.15',  '2025-06-01 10:00:00'),
    (NEXTVAL('seq_audit_id'), 1,  'LOGIN',  NULL,        NULL, NULL, '{"ip":"10.0.0.1"}',        '10.0.0.1',   '2026-01-20 08:00:00');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 19b: SEED DATA — CREDENTIALS VAULT (encrypted)
-- ════════════════════════════════════════════════════════════════════════════

INSERT INTO credentials_vault (owner, service, secret_key, api_token, created_at, expires_at) VALUES
    ('admin',     'AWS',         'AKIAIOSFODNN7EXAMPLE',  'aws-token-abc123xyz',  '2025-01-01 00:00:00', '2026-01-01 00:00:00'),
    ('admin',     'Stripe',      'sk_live_51Example',     'stripe-key-def456uvw', '2025-02-01 00:00:00', '2026-02-01 00:00:00'),
    ('fgarcia',   'QuickBooks',  'qb-api-key-789',       'qb-refresh-tok-xyz',   '2025-03-01 00:00:00', '2025-09-01 00:00:00'),
    ('jdoe',      'GitHub',      'ghp_ExampleToken123',   'github-app-jwt-token', '2025-04-01 00:00:00', '2026-04-01 00:00:00'),
    ('admin',     'SendGrid',    'SG.ExampleKey.abc',     'sendgrid-api-token',   '2025-05-01 00:00:00', '2026-05-01 00:00:00');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 19c: SEED DATA — PARTITIONED TABLES
-- ════════════════════════════════════════════════════════════════════════════

-- Sales metrics across quarters (RANGE partitioned)
INSERT INTO sales_metrics (metric_date, region, revenue, units_sold) VALUES
    ('2025-01-15', 'US-WEST',  125000.00, 42),
    ('2025-02-10', 'US-EAST',  98000.00,  31),
    ('2025-03-20', 'EU',       87000.00,  28),
    ('2025-04-05', 'US-WEST',  132000.00, 45),
    ('2025-05-18', 'US-EAST',  115000.00, 38),
    ('2025-06-22', 'APAC',     76000.00,  22),
    ('2025-07-10', 'US-WEST',  141000.00, 48),
    ('2025-08-15', 'EU',       92000.00,  30),
    ('2025-09-01', 'US-EAST',  108000.00, 35),
    ('2025-10-12', 'US-WEST',  155000.00, 52),
    ('2025-11-20', 'APAC',     84000.00,  26),
    ('2025-12-05', 'EU',       120000.00, 40),
    ('2026-01-08', 'US-WEST',  138000.00, 46),
    ('2026-02-14', 'US-EAST',  112000.00, 37),
    ('2026-03-10', 'EU',       95000.00,  32);

-- Event log entries (LIST partitioned by severity)
INSERT INTO event_log (severity, source, message, event_time) VALUES
    ('INFO',     'auth',     'User admin logged in',              '2025-06-01 08:00:00'),
    ('INFO',     'auth',     'User jdoe logged in',               '2025-06-01 08:15:00'),
    ('DEBUG',    'optimizer', 'Query plan generated in 2ms',      '2025-06-01 08:16:00'),
    ('WARN',     'storage',  'Disk usage at 85% on /data',        '2025-06-01 09:00:00'),
    ('ERROR',    'replication', 'Replica lag exceeded 30s',        '2025-06-01 09:30:00'),
    ('INFO',     'backup',   'Full backup completed successfully', '2025-06-01 10:00:00'),
    ('CRITICAL', 'storage',  'Disk write failure on /data/wal',   '2025-06-01 10:15:00'),
    ('ERROR',    'auth',     'Failed login attempt for root',     '2025-06-01 11:00:00'),
    ('WARN',     'memory',   'Buffer pool 90% utilized',          '2025-06-01 11:30:00'),
    ('INFO',     'scheduler', 'Maintenance job completed',        '2025-06-01 12:00:00'),
    ('DEBUG',    'index',    'B+Tree rebalance on idx_orders',    '2025-06-01 12:15:00'),
    ('CRITICAL', 'network',  'Connection pool exhausted',         '2025-06-01 13:00:00');

-- Session data (HASH partitioned by user_id)
INSERT INTO session_data (user_id, token, ip_address, created_at, expires_at) VALUES
    (1,  'tok_abc001', '10.0.0.1',   '2026-05-21 08:00:00', '2026-05-21 20:00:00'),
    (2,  'tok_abc002', '10.0.1.10',  '2026-05-21 08:15:00', '2026-05-21 20:15:00'),
    (3,  'tok_abc003', '10.0.1.11',  '2026-05-21 08:30:00', '2026-05-21 20:30:00'),
    (5,  'tok_abc005', '10.0.2.5',   '2026-05-21 09:00:00', '2026-05-21 21:00:00'),
    (8,  'tok_abc008', '10.0.3.20',  '2026-05-21 09:15:00', '2026-05-21 21:15:00'),
    (10, 'tok_abc010', '10.0.4.15',  '2026-05-21 09:30:00', '2026-05-21 21:30:00'),
    (14, 'tok_abc014', '10.0.1.14',  '2026-05-21 10:00:00', '2026-05-21 22:00:00'),
    (20, 'tok_abc020', '10.0.1.20',  '2026-05-21 10:30:00', '2026-05-21 22:30:00');

-- ════════════════════════════════════════════════════════════════════════════
-- PART 20: VIEWS
-- ════════════════════════════════════════════════════════════════════════════

-- Active customers with their order totals
CREATE VIEW v_active_customers AS
SELECT
    c.customer_id,
    c.company_name,
    c.contact_name,
    c.contact_email,
    c.country,
    c.credit_limit,
    c.rating,
    COUNT(o.order_id) AS total_orders,
    COALESCE(SUM(o.total_amount), 0) AS lifetime_value,
    MAX(o.order_date) AS last_order_date
FROM customers c, orders o
WHERE c.customer_id = o.customer_id
AND c.is_active = TRUE AND o.status != 'CANCELLED'
GROUP BY c.customer_id, c.company_name, c.contact_name, c.contact_email,
         c.country, c.credit_limit, c.rating;

-- User permissions (flattened through roles)
CREATE VIEW v_user_permissions AS
SELECT
    u.user_id,
    u.username,
    u.full_name,
    r.role_name,
    p.perm_name,
    p.resource,
    p.action_type
FROM users u, user_roles ur, roles r, role_permissions rp, permissions p
WHERE u.user_id = ur.user_id
AND ur.role_id = r.role_id
AND r.role_id = rp.role_id
AND rp.perm_id = p.perm_id
AND u.is_active = TRUE AND r.is_active = TRUE;

-- Order details with customer and product info
CREATE VIEW v_order_details AS
SELECT
    o.order_id,
    o.order_date,
    o.status AS order_status,
    c.company_name AS customer,
    u.full_name AS sales_rep,
    p.product_name,
    p.sku,
    oi.quantity,
    oi.unit_price,
    oi.discount,
    (oi.quantity * oi.unit_price) - oi.discount AS line_total
FROM orders o, customers c, users u, order_items oi, products p
WHERE o.customer_id = c.customer_id
AND o.user_id = u.user_id
AND o.order_id = oi.order_id
AND oi.product_id = p.product_id;

-- Open ticket dashboard
CREATE VIEW v_open_tickets AS
SELECT
    t.ticket_id,
    t.priority,
    t.status,
    t.subject,
    c.company_name AS customer,
    u.full_name AS assigned_to,
    t.created_at,
    CASE
        WHEN t.priority = 'CRITICAL' THEN 'URGENT - Immediate attention'
        WHEN t.priority = 'HIGH' THEN 'Respond within 4 hours'
        WHEN t.priority = 'MEDIUM' THEN 'Respond within 24 hours'
        ELSE 'Respond within 72 hours'
    END AS sla_target
FROM tickets t, customers c, users u
WHERE t.customer_id = c.customer_id
AND t.assigned_to = u.user_id
AND t.status IN ('OPEN', 'IN_PROGRESS');

-- Department payroll summary
CREATE VIEW v_dept_payroll AS
SELECT
    d.dept_name,
    d.budget,
    COUNT(e.emp_id) AS headcount,
    SUM(e.salary) AS total_payroll,
    AVG(e.salary) AS avg_salary,
    MIN(e.salary) AS min_salary,
    MAX(e.salary) AS max_salary,
    d.budget - SUM(e.salary) AS budget_remaining
FROM departments d, employees e
WHERE d.dept_id = e.dept_id
GROUP BY d.dept_name, d.budget;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 21: MATERIALIZED VIEWS
-- ════════════════════════════════════════════════════════════════════════════

-- Monthly revenue summary (materialized for dashboard performance)
CREATE MATERIALIZED VIEW mv_monthly_revenue AS
SELECT
    SUBSTRING(o.order_date, 1, 4) AS revenue_year,
    SUBSTRING(o.order_date, 6, 2) AS revenue_month,
    COUNT(o.order_id) AS order_count,
    SUM(o.total_amount) AS gross_revenue,
    AVG(o.total_amount) AS avg_order_value
FROM orders o
WHERE o.status != 'CANCELLED'
GROUP BY SUBSTRING(o.order_date, 1, 4), SUBSTRING(o.order_date, 6, 2);

-- Product performance (materialized)
CREATE MATERIALIZED VIEW mv_product_performance AS
SELECT
    p.product_id,
    p.sku,
    p.product_name,
    cat.category_name,
    SUM(oi.quantity) AS total_units_sold,
    SUM((oi.quantity * oi.unit_price) - oi.discount) AS total_revenue,
    AVG(oi.unit_price) AS avg_selling_price,
    p.unit_price AS list_price,
    p.cost_price
FROM products p, order_items oi, categories cat
WHERE p.product_id = oi.product_id
AND p.category_id = cat.category_id
GROUP BY p.product_id, p.sku, p.product_name, cat.category_name,
         p.unit_price, p.cost_price;

-- Customer health score (materialized)
CREATE MATERIALIZED VIEW mv_customer_health AS
SELECT
    c.customer_id,
    c.company_name,
    c.rating,
    COUNT(o.order_id) AS total_orders,
    COALESCE(SUM(o.total_amount), 0) AS lifetime_value,
    MAX(o.order_date) AS last_order_date
FROM customers c, orders o
WHERE c.customer_id = o.customer_id
AND c.is_active = TRUE AND o.status != 'CANCELLED'
GROUP BY c.customer_id, c.company_name, c.rating;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 22: TRANSACTIONS & SAVEPOINTS DEMO
-- ════════════════════════════════════════════════════════════════════════════

BEGIN;
INSERT INTO customers (company_name, contact_name, contact_email, city, region, country, postal_code, credit_limit, rating)
VALUES ('Aperture Science', 'Cave Johnson', 'cave@aperture.com', 'Upper Michigan', 'MI', 'US', '49801', 60000.00, 4);
SAVEPOINT before_order;
INSERT INTO orders (customer_id, user_id, order_date, status, total_amount)
VALUES (26, 3, '2026-05-21', 'PENDING', 4999.99);
INSERT INTO order_items (order_id, product_id, quantity, unit_price)
VALUES (31, 1, 1, 4999.99);
COMMIT;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 23: ADVANCED QUERIES — SHOWCASE ALL FEATURES
-- ════════════════════════════════════════════════════════════════════════════

-- --------------------------------------------------------------------------
-- 23a. Subquery: Sales rep leaderboard
-- --------------------------------------------------------------------------
SELECT
    u.full_name AS rep_name,
    COUNT(o.order_id) AS deal_count,
    SUM(o.total_amount) AS total_revenue
FROM orders o, users u
WHERE o.user_id = u.user_id
AND o.status != 'CANCELLED'
GROUP BY u.full_name
ORDER BY total_revenue DESC;

-- --------------------------------------------------------------------------
-- 23b. GROUPING SETS: Revenue by status and customer rating
-- --------------------------------------------------------------------------
SELECT
    o.status,
    c.rating,
    SUM(o.total_amount) AS revenue,
    COUNT(o.order_id) AS orders,
    GROUPING(o.status) AS is_status_total,
    GROUPING(c.rating) AS is_rating_total
FROM orders o, customers c
WHERE o.customer_id = c.customer_id
AND o.status != 'CANCELLED'
GROUP BY GROUPING SETS (
    (o.status, c.rating),
    (o.status),
    (c.rating),
    ()
)
ORDER BY is_status_total, o.status, is_rating_total, c.rating;

-- --------------------------------------------------------------------------
-- 23c. ROLLUP: Hierarchical revenue drill-down
-- --------------------------------------------------------------------------
SELECT
    c.country,
    c.region,
    SUM(o.total_amount) AS revenue,
    COUNT(o.order_id) AS order_count
FROM orders o, customers c
WHERE o.customer_id = c.customer_id
AND o.status != 'CANCELLED'
GROUP BY ROLLUP (c.country, c.region)
ORDER BY c.country, c.region;

-- --------------------------------------------------------------------------
-- 23d. CUBE: Multi-dimensional analysis
-- --------------------------------------------------------------------------
SELECT
    o.status,
    c.country,
    COUNT(*) AS cnt,
    SUM(o.total_amount) AS revenue
FROM orders o, customers c
WHERE o.customer_id = c.customer_id
GROUP BY CUBE (o.status, c.country)
ORDER BY o.status, c.country;

-- --------------------------------------------------------------------------
-- 23e. Subquery + EXISTS: Customers with no orders in 2026
-- --------------------------------------------------------------------------
SELECT c.customer_id, c.company_name, c.contact_email
FROM customers c
WHERE c.is_active = TRUE
AND c.customer_id NOT IN (
    SELECT o.customer_id FROM orders o
    WHERE o.order_date >= '2026-01-01'
    AND o.order_date < '2027-01-01'
);

-- --------------------------------------------------------------------------
-- 23f. Correlated subquery: Products priced above category average
-- --------------------------------------------------------------------------
SELECT p.sku, p.product_name, p.unit_price,
    (SELECT AVG(p2.unit_price) FROM products p2 WHERE p2.category_id = p.category_id) AS category_avg
FROM products p
WHERE p.unit_price > (
    SELECT AVG(p2.unit_price)
    FROM products p2
    WHERE p2.category_id = p.category_id
);

-- --------------------------------------------------------------------------
-- 23g. Set operations: UNION, INTERSECT, EXCEPT
-- --------------------------------------------------------------------------

-- All contact emails from customers table
SELECT contact_email AS email FROM customers WHERE contact_email IS NOT NULL
UNION
SELECT email FROM users;

-- Customers who have both orders AND support tickets
SELECT company_name FROM customers WHERE customer_id IN (SELECT customer_id FROM orders)
INTERSECT
SELECT company_name FROM customers WHERE customer_id IN (SELECT customer_id FROM tickets);

-- Customers with orders but no tickets
SELECT company_name FROM customers WHERE customer_id IN (SELECT customer_id FROM orders)
EXCEPT
SELECT company_name FROM customers WHERE customer_id IN (SELECT customer_id FROM tickets);

-- --------------------------------------------------------------------------
-- 23h. Window functions: ROW_NUMBER, RANK, LAG, LEAD, NTILE
--       (Note: window function execution returns NULL in TDB v1.1.0 —
--        these demonstrate correct parse syntax for future implementation)
-- --------------------------------------------------------------------------
SELECT
    o.order_id,
    o.order_date,
    o.total_amount,
    ROW_NUMBER() OVER (ORDER BY o.order_date) AS row_num,
    RANK() OVER (ORDER BY o.total_amount DESC) AS amount_rank,
    LAG(o.total_amount, 1) OVER (ORDER BY o.order_date) AS prev_amount,
    LEAD(o.total_amount, 1) OVER (ORDER BY o.order_date) AS next_amount,
    NTILE(4) OVER (ORDER BY o.total_amount) AS quartile
FROM orders o
WHERE o.status = 'DELIVERED'
ORDER BY o.order_date;

-- --------------------------------------------------------------------------
-- 23i. Window: Running totals and partitioned windows
-- --------------------------------------------------------------------------
SELECT
    o.order_id,
    o.order_date,
    o.total_amount,
    SUM(o.total_amount) OVER (ORDER BY o.order_date ROWS UNBOUNDED PRECEDING) AS running_total,
    AVG(o.total_amount) OVER (ORDER BY o.order_date ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS moving_avg_3,
    DENSE_RANK() OVER (ORDER BY o.total_amount DESC) AS dense_rnk
FROM orders o
WHERE o.status != 'CANCELLED'
ORDER BY o.order_date;

-- --------------------------------------------------------------------------
-- 23j. Statistical aggregates: Salary analysis
-- --------------------------------------------------------------------------
SELECT
    d.dept_name,
    COUNT(e.emp_id) AS headcount,
    ROUND(AVG(e.salary), 2) AS avg_salary,
    ROUND(MEDIAN(e.salary), 2) AS median_salary,
    ROUND(STDDEV(e.salary), 2) AS salary_stddev,
    ROUND(VARIANCE(e.salary), 2) AS salary_variance,
    STRING_AGG(e.job_title, ', ') AS job_titles
FROM departments d, employees e
WHERE d.dept_id = e.dept_id
GROUP BY d.dept_name
ORDER BY avg_salary DESC;

-- --------------------------------------------------------------------------
-- 23k. Scalar functions showcase
-- --------------------------------------------------------------------------
SELECT
    UPPER(c.company_name) AS upper_name,
    LOWER(c.contact_email) AS lower_email,
    LENGTH(c.company_name) AS name_len,
    SUBSTRING(c.company_name, 1, 10) AS short_name,
    RPAD('', c.rating, '*') AS stars,
    CONCAT(c.city, ', ', COALESCE(c.region, ''), ' ', c.country) AS full_location,
    LPAD(CAST(c.customer_id AS VARCHAR), 6, '0') AS padded_id,
    REVERSE(c.company_name) AS reversed,
    ROUND(c.credit_limit * 1.1, 2) AS credit_110pct,
    CEIL(c.credit_limit / 1000.0) AS credit_thousands,
    ABS(c.credit_limit - 50000) AS distance_from_50k,
    MOD(c.customer_id, 5) AS bucket,
    POWER(c.rating, 2) AS rating_squared,
    SQRT(c.credit_limit) AS credit_sqrt,
    COALESCE(c.region, 'N/A') AS region_safe,
    NULLIF(c.region, '') AS region_null_if_empty,
    CAST(c.credit_limit AS INTEGER) AS credit_int,
    TYPEOF(c.credit_limit) AS credit_type
FROM customers c
WHERE c.customer_id <= 10;

-- --------------------------------------------------------------------------
-- 23l. Date functions showcase
-- --------------------------------------------------------------------------
SELECT
    o.order_id,
    o.order_date,
    SUBSTRING(o.order_date, 1, 4) AS yr,
    SUBSTRING(o.order_date, 6, 2) AS mo,
    SUBSTRING(o.order_date, 9, 2) AS dy,
    NOW() AS current_ts,
    CURRENT_DATE() AS today
FROM orders o
WHERE o.order_id <= 5;

-- --------------------------------------------------------------------------
-- 23m. Crypto functions showcase
-- --------------------------------------------------------------------------
SELECT
    'password123' AS plaintext,
    MD5('password123') AS md5_hash,
    SHA256('password123') AS sha256_hash,
    SHA1('password123') AS sha1_hash;

-- --------------------------------------------------------------------------
-- 23n. Trig + math functions showcase
-- --------------------------------------------------------------------------
SELECT
    PI() AS pi_value,
    SIN(PI() / 6) AS sin_30deg,
    COS(PI() / 3) AS cos_60deg,
    TAN(PI() / 4) AS tan_45deg,
    DEGREES(PI()) AS pi_in_degrees,
    RADIANS(180.0) AS half_turn_rad,
    LN(EXP(1.0)) AS ln_e,
    LOG10(1000.0) AS log_1000,
    FACTORIAL(10) AS ten_factorial,
    GCD(48, 18) AS gcd_48_18,
    LCM(12, 8) AS lcm_12_8;

-- --------------------------------------------------------------------------
-- 23o. CASE expressions and IIF
-- --------------------------------------------------------------------------
SELECT
    c.company_name,
    c.credit_limit,
    CASE
        WHEN c.credit_limit >= 80000 THEN 'Platinum'
        WHEN c.credit_limit >= 50000 THEN 'Gold'
        WHEN c.credit_limit >= 25000 THEN 'Silver'
        ELSE 'Bronze'
    END AS tier,
    CASE c.rating
        WHEN 5 THEN 'Excellent'
        WHEN 4 THEN 'Good'
        WHEN 3 THEN 'Average'
        WHEN 2 THEN 'Below Average'
        WHEN 1 THEN 'Poor'
    END AS rating_label,
    IIF(c.credit_limit > 50000, 'High Value', 'Standard') AS value_class
FROM customers c
ORDER BY c.credit_limit DESC;

-- --------------------------------------------------------------------------
-- 23p. Partition pruning demo (queries on partitioned tables)
-- --------------------------------------------------------------------------

-- RANGE partition pruning: should scan only p_2025_q1
SELECT * FROM sales_metrics WHERE metric_date BETWEEN '2025-01-01' AND '2025-03-31';

-- RANGE partition pruning: should scan only p_2026_q1
SELECT * FROM sales_metrics WHERE metric_date >= '2026-01-01' AND metric_date < '2026-04-01';

-- LIST partition pruning: only scan p_error
SELECT * FROM event_log WHERE severity = 'ERROR';

-- LIST partition pruning: only scan p_warn and p_critical
SELECT * FROM event_log WHERE severity IN ('WARN', 'CRITICAL');

-- HASH partition: full scan shows data distributed across partitions
SELECT * FROM session_data;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 24: INFORMATION_SCHEMA QUERIES
-- ════════════════════════════════════════════════════════════════════════════

SELECT * FROM INFORMATION_SCHEMA.TABLES;
SELECT * FROM INFORMATION_SCHEMA.COLUMNS;
SELECT * FROM INFORMATION_SCHEMA.INDEXES;
SELECT * FROM INFORMATION_SCHEMA.SEQUENCES;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 25: EXPLAIN — Query plan analysis
-- ════════════════════════════════════════════════════════════════════════════

EXPLAIN SELECT o.order_id, c.company_name, o.total_amount
FROM orders o, customers c
WHERE o.customer_id = c.customer_id
AND o.order_date >= '2025-06-01'
AND o.status = 'DELIVERED'
ORDER BY o.total_amount DESC;

-- ════════════════════════════════════════════════════════════════════════════
-- PART 26: REFRESH MATERIALIZED VIEWS
-- ════════════════════════════════════════════════════════════════════════════

REFRESH MATERIALIZED VIEW mv_monthly_revenue;
REFRESH MATERIALIZED VIEW mv_product_performance;
REFRESH MATERIALIZED VIEW mv_customer_health;

-- Query the refreshed materialized views
SELECT * FROM mv_monthly_revenue ORDER BY revenue_year, revenue_month;
SELECT * FROM mv_product_performance ORDER BY total_revenue DESC;
SELECT * FROM mv_customer_health ORDER BY lifetime_value DESC;

-- Query the regular views
SELECT * FROM v_active_customers ORDER BY lifetime_value DESC;
SELECT * FROM v_dept_payroll ORDER BY total_payroll DESC;
SELECT * FROM v_open_tickets ORDER BY priority;
SELECT * FROM v_user_permissions WHERE username = 'admin';
SELECT * FROM v_order_details WHERE order_status = 'DELIVERED' ORDER BY order_id;

-- ════════════════════════════════════════════════════════════════════════════
-- SUMMARY
-- ════════════════════════════════════════════════════════════════════════════
--
-- Tables:          21 (roles, users, user_roles, permissions, role_permissions,
--                      customers, contacts, tickets, categories, products,
--                      warehouses, inventory, orders, order_items, invoices,
--                      payments, departments, employees, audit_log,
--                      sales_metrics, event_log, session_data)
-- Views:            5 (v_active_customers, v_user_permissions, v_order_details,
--                      v_open_tickets, v_dept_payroll)
-- Materialized:     3 (mv_monthly_revenue, mv_product_performance, mv_customer_health)
-- Sequences:        3 (seq_audit_id, seq_ticket_id, seq_invoice_no)
-- Indexes:         16 (12 BTREE + 3 HASH + 1 UNIQUE)
-- Partitions:       3 tables (sales_metrics=RANGE, event_log=LIST, session_data=HASH)
-- Encrypted:        2 tables (users, audit_log) with column-level ENCRYPTED
-- Rows:          330+ across all tables
--
-- Features demonstrated:
--   Data types, constraints, foreign keys, ON DELETE CASCADE, AUTO_INCREMENT,
--   sequences (NEXTVAL/CURRVAL), partitioning (RANGE/LIST/HASH with pruning),
--   AES-256-GCM encryption (table + column), indexes (BTREE/HASH/UNIQUE),
--   views, materialized views (CREATE + REFRESH), transactions (BEGIN/
--   SAVEPOINT/COMMIT), window functions (ROW_NUMBER, RANK, DENSE_RANK,
--   LAG, LEAD, NTILE, running SUM/AVG), GROUPING SETS, CUBE, ROLLUP,
--   GROUPING(), subqueries, correlated subqueries, EXISTS, NOT EXISTS,
--   set operations (UNION/INTERSECT/EXCEPT), JOINs (INNER, LEFT, RIGHT),
--   aggregates (COUNT, SUM, AVG, MIN, MAX, STDDEV, VARIANCE, MEDIAN,
--   STRING_AGG), CASE/WHEN (searched + simple), COALESCE, NULLIF, IIF(),
--   CAST, scalar functions (UPPER, LOWER, LENGTH, SUBSTRING, RPAD,
--   CONCAT, LPAD, REVERSE, ROUND, CEIL, ABS, MOD, POWER, SQRT, PI, SIN,
--   COS, TAN, DEGREES, RADIANS, LN, EXP, LOG10, FACTORIAL, GCD, LCM,
--   MD5, SHA1, SHA256, NOW, CURRENT_DATE), EXPLAIN, INFORMATION_SCHEMA
--   (TABLES, COLUMNS, INDEXES, SEQUENCES).
-- ============================================================================
