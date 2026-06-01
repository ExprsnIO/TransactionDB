// ===========================================================================
// TDB Manager — full mock UI
// ===========================================================================

// ---------- Mock data ----------
const CONNECTIONS = [
  { id: "local",   name: "tdb_local",   path: "/Users/rick/Library/TDB/local.tdb", status: "on",  meta: "TDB v1.0 · RW",   env: "dev",     group: "local" },
  { id: "staging", name: "tdb_staging", path: "tdb://staging.internal:5443/app",   status: "on",  meta: "TDB v0.9.4 · RO", env: "staging", group: "internal" },
  { id: "prod",    name: "tdb_prod",    path: "tdb://prod.internal:5443/app",      status: "off", meta: "Not connected",   env: "prod",    group: "internal" },
  { id: "mem",     name: "in-memory",   path: ":memory:",                          status: "on",  meta: "Ephemeral",        env: "dev",     group: "local" },
];

const SCHEMA = {
  customers: {
    icon: "▦", flags: [], comment: "Customer accounts.",
    columns: [
      { name: "id",             type: "INTEGER",       nullable: false, default: "AUTOINCREMENT", key: "PK", comment: "Internal id" },
      { name: "email",          type: "VARCHAR(255)",  nullable: false, default: null,             key: "UQ", comment: "Login" },
      { name: "first_name",     type: "VARCHAR(64)",   nullable: false, default: null,             key: "",   comment: "" },
      { name: "last_name",      type: "VARCHAR(64)",   nullable: false, default: null,             key: "",   comment: "" },
      { name: "tier",           type: "ENUM",          nullable: false, default: "'standard'",     key: "",   comment: "standard|gold|platinum" },
      { name: "lifetime_value", type: "DECIMAL(12,2)", nullable: true,  default: "0.00",            key: "",   comment: "" },
      { name: "country",        type: "CHAR(2)",       nullable: true,  default: null,             key: "FK", comment: "ISO 3166-1" },
      { name: "signup_date",    type: "DATE",          nullable: false, default: "CURRENT_DATE",   key: "",   comment: "" },
      { name: "last_seen",      type: "TIMESTAMP",     nullable: true,  default: null,             key: "",   comment: "" },
      { name: "metadata",       type: "JSON",          nullable: true,  default: "'{}'",           key: "",   comment: "" },
    ],
    rows: makeCustomers(),
    constraints: [
      { kind: "PRIMARY KEY",  name: "customers_pk",         def: "PRIMARY KEY (id)" },
      { kind: "UNIQUE",       name: "customers_email_uq",   def: "UNIQUE (email)" },
      { kind: "CHECK",        name: "customers_tier_chk",   def: "CHECK (tier IN ('standard','gold','platinum'))" },
      { kind: "FOREIGN KEY",  name: "customers_country_fk", def: "FOREIGN KEY (country) REFERENCES countries(code)", refs: { col: "country", table: "countries", refCol: "code" } },
    ],
    fks: [{ col: "country", table: "countries", refCol: "code" }],
    partitions: null,
    storage: { kind: "row", encrypted: false, tablespace: "main" },
  },
  orders: {
    icon: "▦", flags: ["PARTITIONED"], comment: "Order header.",
    columns: [
      { name: "id",          type: "BIGINT",        nullable: false, default: "AUTOINCREMENT",     key: "PK", comment: "" },
      { name: "customer_id", type: "INTEGER",       nullable: false, default: null,                 key: "FK", comment: "" },
      { name: "total",       type: "DECIMAL(10,2)", nullable: false, default: "0.00",               key: "",   comment: "" },
      { name: "currency",    type: "CHAR(3)",       nullable: false, default: "'USD'",              key: "",   comment: "" },
      { name: "status",      type: "ENUM",          nullable: false, default: "'pending'",          key: "",   comment: "pending|paid|shipped|refunded" },
      { name: "placed_at",   type: "TIMESTAMP",     nullable: false, default: "CURRENT_TIMESTAMP", key: "",   comment: "Partition key" },
    ],
    rows: [
      [50012, 1001, 249.00, "USD", "shipped",   "2026-05-21 09:14:21"],
      [50013, 1003, 1280.50,"USD", "paid",      "2026-05-22 11:00:00"],
      [50014, 1001,  42.00, "USD", "refunded",  "2026-05-23 02:11:00"],
      [50015, 1004, 880.00, "GBP", "shipped",   "2026-05-23 07:45:00"],
      [50016, 1011, 4200.00,"GBP", "paid",      "2026-05-24 08:00:00"],
      [50017, 1008, 199.00, "USD", "pending",   "2026-05-28 02:11:30"],
    ],
    constraints: [
      { kind: "PRIMARY KEY", name: "orders_pk",          def: "PRIMARY KEY (id)" },
      { kind: "FOREIGN KEY", name: "orders_customer_fk", def: "FOREIGN KEY (customer_id) REFERENCES customers(id)", refs: { col: "customer_id", table: "customers", refCol: "id" } },
    ],
    fks: [{ col: "customer_id", table: "customers", refCol: "id" }],
    partitions: {
      by: "RANGE (placed_at)",
      list: [
        { name: "orders_2026q1", range: "FROM ('2026-01-01') TO ('2026-04-01')", rows: 1242, size: "18 MB" },
        { name: "orders_2026q2", range: "FROM ('2026-04-01') TO ('2026-07-01')", rows: 880,  size: "12 MB" },
        { name: "orders_2026q3", range: "FROM ('2026-07-01') TO ('2026-10-01')", rows: 0,    size: "8 KB" },
      ],
    },
    storage: { kind: "row", encrypted: false, tablespace: "main" },
  },
  products: {
    icon: "▦", flags: ["COLUMNAR"], comment: "Catalog. Stored columnar with zstd.",
    columns: [
      { name: "sku",   type: "VARCHAR(32)",   nullable: false, default: null, key: "PK", comment: "" },
      { name: "name",  type: "VARCHAR(255)",  nullable: false, default: null, key: "",   comment: "" },
      { name: "price", type: "DECIMAL(10,2)", nullable: false, default: null, key: "",   comment: "" },
      { name: "stock", type: "INTEGER",       nullable: false, default: "0",  key: "",   comment: "" },
      { name: "tags",  type: "JSON",          nullable: true,  default: "'[]'", key: "", comment: "" },
    ],
    rows: [
      ["TDB-PRO-1Y", "TDB Pro · 1 Year",   249.00, 9999, '["pro","subscription"]'],
      ["TDB-TEAM-1Y","TDB Team · 1 Year",  890.00, 9999, '["team","subscription"]'],
      ["TDB-ENT-1Y", "TDB Enterprise · 1Y",4200.00, 99,  '["enterprise"]'],
      ["BOOK-SQL",   "The SQL Handbook",    39.00, 142,  '["book"]'],
    ],
    constraints: [
      { kind: "PRIMARY KEY", name: "products_pk",        def: "PRIMARY KEY (sku)" },
      { kind: "CHECK",       name: "products_price_chk", def: "CHECK (price >= 0)" },
    ],
    fks: [],
    partitions: null,
    storage: { kind: "columnar", encrypted: false, tablespace: "main", compression: "zstd", ratio: 4.2 },
  },
  countries: {
    icon: "▦", flags: [], comment: "ISO country codes.",
    columns: [
      { name: "code", type: "CHAR(2)",     nullable: false, default: null, key: "PK", comment: "" },
      { name: "name", type: "VARCHAR(64)", nullable: false, default: null, key: "",   comment: "" },
    ],
    rows: [["GB","United Kingdom"],["US","United States"],["FI","Finland"],["NL","Netherlands"],["DE","Germany"],["JP","Japan"]],
    constraints: [{ kind: "PRIMARY KEY", name: "countries_pk", def: "PRIMARY KEY (code)" }],
    fks: [],
    partitions: null,
    storage: { kind: "row", encrypted: false, tablespace: "main" },
  },
  sessions: {
    icon: "▦", flags: ["ENCRYPTED"], comment: "Active user sessions. Encrypted at rest.",
    columns: [
      { name: "id",          type: "UUID",        nullable: false, default: "gen_random_uuid()",  key: "PK", comment: "" },
      { name: "customer_id", type: "INTEGER",     nullable: false, default: null,                  key: "FK", comment: "" },
      { name: "started_at",  type: "TIMESTAMP",   nullable: false, default: "CURRENT_TIMESTAMP",  key: "",   comment: "" },
      { name: "ip",          type: "VARCHAR(45)", nullable: true,  default: null,                  key: "",   comment: "" },
    ],
    rows: [
      ["9b1c-aa42", 1001, "2026-05-28 09:14:21", "10.0.0.4"],
      ["7f0a-bb91", 1003, "2026-05-28 08:30:11", "10.0.0.9"],
    ],
    constraints: [{ kind: "FOREIGN KEY", name: "sessions_customer_fk", def: "FOREIGN KEY (customer_id) REFERENCES customers(id)", refs: { col: "customer_id", table: "customers", refCol: "id" } }],
    fks: [{ col: "customer_id", table: "customers", refCol: "id" }],
    partitions: null,
    storage: { kind: "row", encrypted: true, tablespace: "secure" },
  },
  audit_log: {
    icon: "▦", flags: [], comment: "Audit trail.",
    columns: [
      { name: "id",      type: "BIGINT",      nullable: false, default: "AUTOINCREMENT",     key: "PK", comment: "" },
      { name: "actor",   type: "VARCHAR(64)", nullable: false, default: null,                 key: "",   comment: "" },
      { name: "action",  type: "VARCHAR(32)", nullable: false, default: null,                 key: "",   comment: "" },
      { name: "payload", type: "JSON",        nullable: true,  default: null,                 key: "",   comment: "" },
      { name: "at",      type: "TIMESTAMP",   nullable: false, default: "CURRENT_TIMESTAMP", key: "",   comment: "" },
    ],
    rows: [
      [1, "rick", "LOGIN", '{"ip":"10.0.0.1"}', "2026-05-28 09:00:00"],
      [2, "rick", "QUERY", '{"sql":"SELECT 1"}', "2026-05-28 09:00:02"],
    ],
    constraints: [],
    fks: [],
    partitions: null,
    storage: { kind: "row", encrypted: false, tablespace: "main" },
  },
  locations: {
    icon: "▦", flags: [], comment: "Spatial points of interest.",
    columns: [
      { name: "id",   type: "INTEGER",   nullable: false, default: "AUTOINCREMENT", key: "PK", comment: "" },
      { name: "name", type: "VARCHAR(128)", nullable: false, default: null, key: "", comment: "" },
      { name: "geom", type: "GEOMETRY",  nullable: false, default: null,             key: "",   comment: "WGS-84" },
    ],
    rows: [
      [1, "HQ",        "POINT(-122.4194 37.7749)"],
      [2, "Datacenter","POINT(-121.8863 37.3382)"],
      [3, "London",    "POINT(-0.1276 51.5074)"],
    ],
    constraints: [{ kind: "PRIMARY KEY", name: "locations_pk", def: "PRIMARY KEY (id)" }],
    fks: [],
    partitions: null,
    storage: { kind: "row", encrypted: false, tablespace: "main" },
  },
};

const VIEWS = [
  { name: "v_top_customers", kind: "view", def: "SELECT id, email, lifetime_value FROM customers ORDER BY lifetime_value DESC", deps: ["customers"] },
];
const MVIEWS = [
  { name: "mv_orders_daily", kind: "mview", def: "SELECT date(placed_at) d, count(*) n, sum(total) tot FROM orders GROUP BY 1", deps: ["orders"], refreshed: "2026-05-28 04:00:01", rows: 148 },
];
const INDEXES = [
  { name: "customers_email_uq",   table: "customers", type: "B+Tree", cols: ["email"], unique: true,  size: "240 KB" },
  { name: "orders_placed_idx",    table: "orders",    type: "B+Tree", cols: ["placed_at"], unique: false, size: "180 KB" },
  { name: "orders_customer_idx",  table: "orders",    type: "Hash",   cols: ["customer_id"], unique: false, size: "96 KB" },
  { name: "audit_payload_idx",    table: "audit_log", type: "B-Tree", cols: ["payload->>'action'"], unique: false, size: "12 KB" },
  { name: "locations_geom_rtree", table: "locations", type: "R-Tree", cols: ["geom"], unique: false, size: "32 KB" },
  { name: "locations_geom_rplus", table: "locations", type: "R+Tree", cols: ["geom"], unique: false, size: "44 KB" },
];
const SEQUENCES = [
  { name: "customers_id_seq", current: 1012, increment: 1, min: 1, max: "2^63-1", cycle: false },
  { name: "orders_id_seq",    current: 50017, increment: 1, min: 1, max: "2^63-1", cycle: false },
];
const SCRIPTS = [
  { name: "daily_refresh", lang: "lua", calls: 142, jit: "JIT (LLVM)", body:
`-- Refresh materialized views and trim audit log
function refresh()
  db.execute("REFRESH MATERIALIZED VIEW mv_orders_daily")
  local cutoff = db.now() - 86400 * 30
  db.execute(string.format("DELETE FROM audit_log WHERE at < to_timestamp(%d)", cutoff))
  db.log("refresh complete")
end
refresh()` },
  { name: "tier_upgrades",  lang: "lua", calls: 11,  jit: "Interpreter", body:
`-- Promote customers crossing thresholds
local r = db.query("SELECT id, lifetime_value FROM customers")
for _, row in ipairs(r) do
  local tier = "standard"
  if row.lifetime_value > 10000 then tier = "platinum"
  elseif row.lifetime_value > 5000 then tier = "gold" end
  db.execute(string.format("UPDATE customers SET tier='%s' WHERE id=%d", tier, row.id))
end` },
  { name: "ingest_webhook", lang: "js",  calls: 4,   jit: "JSC",         body:
`// Webhook ingestion (JavaScriptCore)
function handle(payload) {
  const j = JSON.parse(payload);
  db.execute(\`INSERT INTO audit_log(actor,action,payload) VALUES('webhook','EVENT','\${JSON.stringify(j)}')\`);
  return { ok: true };
}` },
];
const TRIGGERS = [
  { name: "orders_audit_trg", table: "orders",    timing: "AFTER",  event: "INSERT OR UPDATE", forEach: "ROW", enabled: true,
    body: "BEGIN INSERT INTO audit_log(actor, action, payload) VALUES (current_user(), 'ORDER_'||TG_OP, row_to_json(NEW)); END" },
  { name: "customers_lower_email", table: "customers", timing: "BEFORE", event: "INSERT OR UPDATE OF email", forEach: "ROW", enabled: true,
    body: "BEGIN NEW.email := lower(NEW.email); RETURN NEW; END" },
  { name: "products_check_stock",  table: "products",  timing: "BEFORE", event: "UPDATE OF stock", forEach: "ROW", enabled: false,
    body: "BEGIN IF NEW.stock < 0 THEN RAISE EXCEPTION 'negative stock'; END IF; END" },
];
const TABLESPACES = [
  { name: "main",   path: "/Users/rick/Library/TDB/data",       size: "284 MB", pages: 72000,  encrypted: false },
  { name: "secure", path: "/Users/rick/Library/TDB/secure",     size: " 18 MB", pages:  4600,  encrypted: true  },
  { name: "warm",   path: "/Users/rick/Library/TDB/warm",       size: "  4 GB", pages: 1024000, encrypted: false },
];
const WAL = [
  { lsn: "0/3F2A18", time: "2026-05-28 09:14:21.103", xid: 4811, type: "INSERT", obj: "orders",    bytes: 240,  detail: "id=50017" },
  { lsn: "0/3F2AF0", time: "2026-05-28 09:14:21.221", xid: 4811, type: "COMMIT", obj: "",          bytes: 32,   detail: "" },
  { lsn: "0/3F2B10", time: "2026-05-28 09:14:30.502", xid: 4812, type: "UPDATE", obj: "customers", bytes: 312,  detail: "id=1004 tier" },
  { lsn: "0/3F2C40", time: "2026-05-28 09:14:31.013", xid: 4812, type: "ROLLBACK", obj: "",        bytes: 32,   detail: "user abort" },
  { lsn: "0/3F2C60", time: "2026-05-28 09:15:00.001", xid: 0,    type: "CHECKPOINT", obj: "",      bytes: 64,   detail: "auto" },
];
const TXNS = [
  { xid: 4815, state: "active",     iso: "READ COMMITTED",   user: "rick",   started: "2026-05-28 09:16:02", locks: 2, sql: "SELECT * FROM orders WHERE customer_id = 1001" },
  { xid: 4816, state: "idle in tx", iso: "REPEATABLE READ",  user: "rick",   started: "2026-05-28 09:14:18", locks: 1, sql: "BEGIN; -- waiting" },
  { xid: 4817, state: "blocked",    iso: "SERIALIZABLE",     user: "ingest", started: "2026-05-28 09:15:48", locks: 3, sql: "UPDATE customers SET tier='gold' WHERE id=1009" },
];
const LOCKS = [
  { xid: 4815, obj: "orders",    mode: "AccessShare",   granted: true },
  { xid: 4816, obj: "customers", mode: "RowExclusive",  granted: true },
  { xid: 4817, obj: "customers", mode: "RowExclusive",  granted: false },
];
const USERS = [
  { name: "rick",   kind: "user", roles: ["superuser"] },
  { name: "ingest", kind: "user", roles: ["writer"]    },
  { name: "report", kind: "user", roles: ["reader"]    },
  { name: "reader",     kind: "role" },
  { name: "writer",     kind: "role" },
  { name: "superuser",  kind: "role" },
];
const PRIVS = {
  "rick":   { customers: ["S","I","U","D","X"], orders: ["S","I","U","D","X"], products: ["S","I","U","D","X"], scripts: ["X"] },
  "ingest": { customers: ["S","I","U"],         orders: ["S","I","U"],        audit_log: ["I"] },
  "report": { customers: ["S"], orders: ["S"], products: ["S"], audit_log: ["S"] },
  "reader": { customers: ["S"], orders: ["S"], products: ["S"], audit_log: ["S"] },
  "writer": { customers: ["S","I","U"], orders: ["S","I","U"] },
  "superuser": { "*": ["S","I","U","D","X"] },
};

const SAVED_DEFAULT = [
  { id: "s1", name: "Top customers by LTV", sql: "SELECT id, email, lifetime_value\n  FROM customers\n  ORDER BY lifetime_value DESC\n  LIMIT 25;" },
  { id: "s2", name: "Orders this week",     sql: "SELECT id, customer_id, total, currency, status\n  FROM orders\n  WHERE placed_at >= CURRENT_DATE - INTERVAL '7 days';" },
  { id: "s3", name: "Sessions by country",  sql: "SELECT c.country, COUNT(*) AS sessions\n  FROM sessions s\n  JOIN customers c ON c.id = s.customer_id\n  GROUP BY c.country\n  ORDER BY sessions DESC;" },
];
const HISTORY_DEFAULT = [
  { sql: "SELECT * FROM customers WHERE country = 'GB';",         rows: 2,  ms: 3,  when: "today, 09:14"  },
  { sql: "SELECT COUNT(*) FROM orders WHERE status = 'shipped';", rows: 1,  ms: 1,  when: "today, 09:12"  },
  { sql: "UPDATE customers SET tier = 'gold' WHERE id = 1005;",   rows: 1,  ms: 4,  when: "today, 08:58"  },
  { sql: "EXPLAIN SELECT * FROM orders JOIN customers ON ...",    rows: 6,  ms: 2,  when: "yesterday"     },
  { sql: "CREATE INDEX idx_orders_status ON orders(status);",     rows: 0,  ms: 11, when: "yesterday"     },
];

function makeCustomers() {
  const base = [
    [1001, "ada@example.com",   "Ada",    "Lovelace",  "platinum", 12480.50, "GB", "2023-01-04", "2026-05-27 09:14:21", '{"plan":"team"}'],
    [1002, "linus@example.com", "Linus",  "Torvalds",  "gold",      8120.00, "FI", "2023-02-19", "2026-05-28 04:02:11", '{"plan":"solo"}'],
    [1003, "grace@example.com", "Grace",  "Hopper",    "platinum", 19200.00, "US", "2023-03-22", "2026-05-26 22:30:00", '{"plan":"team"}'],
    [1004, "alan@example.com",  "Alan",   "Turing",    "platinum", 22011.20, "GB", "2023-04-01", "2026-05-28 07:45:01", '{"plan":"team"}'],
    [1005, "ken@example.com",   "Ken",    "Thompson",  "gold",      6400.00, "US", "2023-04-12", "2026-05-25 14:18:55", null],
    [1006, "dennis@example.com","Dennis", "Ritchie",   "gold",      6420.00, "US", "2023-04-12", "2026-05-19 11:00:00", null],
    [1007, "barbara@example.com","Barbara","Liskov",   "standard",   420.10, "US", "2023-05-30", "2026-04-29 16:21:09", '{"plan":"solo"}'],
    [1008, "donald@example.com","Donald", "Knuth",     "platinum", 15800.00, "US", "2023-06-07", "2026-05-28 02:11:30", '{"plan":"team"}'],
    [1009, "margaret@example.com","Margaret","Hamilton","gold",     7980.00, "US", "2023-07-14", "2026-05-27 19:05:00", '{"plan":"team"}'],
    [1010, "edsger@example.com","Edsger", "Dijkstra",  "standard",   180.00, "NL", "2023-08-02", null,                  null],
    [1011, "tim@example.com",   "Tim",    "Berners-Lee","platinum",30440.00, "GB", "2023-09-19", "2026-05-28 08:00:00", '{"plan":"enterprise"}'],
    [1012, "guido@example.com", "Guido",  "van Rossum","gold",      9120.00, "NL", "2023-10-04", "2026-05-26 10:42:00", '{"plan":"team"}'],
  ];
  // Pad to ~120 rows so paging is visible
  const fillers = [];
  const cs = ["US","GB","FI","NL","DE","JP"];
  const tiers = ["standard","gold","platinum"];
  for (let i = 0; i < 110; i++) {
    const id = 1013 + i;
    fillers.push([id, `user${id}@example.com`, "User", `#${id}`, tiers[i % 3], +(Math.random()*15000).toFixed(2), cs[i % cs.length], "2024-03-01", "2026-05-20 12:00:00", '{"plan":"solo"}']);
  }
  return base.concat(fillers);
}

const INFO_TABLES = {
  tables: { cols: ["table_schema","table_name","table_type","is_columnar","is_encrypted"],
    rows: Object.entries(SCHEMA).map(([n,t])=>["public",n,"BASE TABLE", t.storage.kind==="columnar"?"YES":"NO", t.storage.encrypted?"YES":"NO"]) },
  columns: { cols: ["table_name","column_name","ordinal","data_type","is_nullable","column_default"],
    rows: Object.entries(SCHEMA).flatMap(([n,t]) => t.columns.map((c,i)=>[n, c.name, i+1, c.type, c.nullable?"YES":"NO", c.default ?? ""])) },
  constraints: { cols: ["table_name","constraint_name","constraint_type","definition"],
    rows: Object.entries(SCHEMA).flatMap(([n,t]) => t.constraints.map(k=>[n, k.name, k.kind, k.def])) },
  indexes: { cols: ["index_name","table_name","type","columns","unique","size"],
    rows: INDEXES.map(i=>[i.name, i.table, i.type, i.cols.join(", "), i.unique?"YES":"NO", i.size]) },
  triggers: { cols: ["trigger_name","table_name","timing","event","for_each","enabled"],
    rows: TRIGGERS.map(t=>[t.name, t.table, t.timing, t.event, t.forEach, t.enabled?"YES":"NO"]) },
  sequences: { cols: ["sequence_name","current_value","increment","min","max","cycle"],
    rows: SEQUENCES.map(s=>[s.name, s.current, s.increment, s.min, s.max, s.cycle?"YES":"NO"]) },
  tablespaces: { cols: ["name","path","size","pages","encrypted"],
    rows: TABLESPACES.map(t=>[t.name, t.path, t.size, t.pages, t.encrypted?"YES":"NO"]) },
};

// ---------- Persistent state ----------
const LS_KEY = "tdb-manager-v2";
const loaded = (()=>{ try { return JSON.parse(localStorage.getItem(LS_KEY) || "null"); } catch { return null; } })() || {};

const state = {
  activeConn: loaded.activeConn || "local",
  activeTable: loaded.activeTable || "customers",
  view: "browse",
  pane: "rows",
  rtab: "result",
  adminPane: "scripts",
  selectedRow: -1,
  queries: loaded.queries || { q1: { name: "Query 1", sql: "SELECT id, email, tier, lifetime_value\n  FROM customers\n  WHERE lifetime_value > 5000\n  ORDER BY lifetime_value DESC\n  LIMIT 50;" } },
  activeQ: loaded.activeQ || "q1",
  history: loaded.history || [...HISTORY_DEFAULT],
  saved: loaded.saved || [...SAVED_DEFAULT],
  filters: { table: null, conds: [] },
  sort: { col: null, dir: "asc" },
  page: 1,
  dirty: new Map(),
  hiddenCols: new Set(),
  lastResult: null,
  prevResult: null,
  settings: Object.assign({
    theme: "auto", accent: "#0a84ff", font: 12, timeout: 30, pageSize: 50, confirmDDL: true, persist: true,
  }, loaded.settings || {}),
  debugger: { running: false, line: 0, bps: new Set([3]), vars: [], stack: [] },
  scriptCurrent: SCRIPTS[0].name,
  walPos: WAL[WAL.length - 1].lsn,
};

function persist() {
  if (!state.settings.persist) return;
  const out = {
    activeConn: state.activeConn, activeTable: state.activeTable,
    queries: state.queries, activeQ: state.activeQ,
    history: state.history.slice(0, 100), saved: state.saved,
    settings: state.settings,
  };
  try { localStorage.setItem(LS_KEY, JSON.stringify(out)); } catch {}
}

// ---------- DOM helpers ----------
const $  = (s, r=document) => r.querySelector(s);
const $$ = (s, r=document) => Array.from(r.querySelectorAll(s));
const cls = (...xs) => xs.filter(Boolean).join(" ");
function el(tag, attrs={}, ...kids) {
  const e = document.createElement(tag);
  for (const [k,v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, "");
    else if (v !== false && v !== null && v !== undefined) e.setAttribute(k, v);
  }
  for (const k of kids.flat()) {
    if (k == null) continue;
    e.append(k.nodeType ? k : document.createTextNode(String(k)));
  }
  return e;
}
function svgEl(tag, attrs={}, ...kids) {
  const e = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k,v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    e.setAttribute(k, String(v));
  }
  for (const k of kids.flat()) {
    if (k == null) continue;
    e.append(k.nodeType ? k : document.createTextNode(String(k)));
  }
  return e;
}
function fmtNum(n) {
  if (n === null || n === undefined) return null;
  if (typeof n === "number" && Number.isFinite(n)) return n.toLocaleString(undefined, { maximumFractionDigits: 2, minimumFractionDigits: Number.isInteger(n) ? 0 : 2 });
  return String(n);
}
function escHtml(s) { return String(s).replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c])); }

// ---------- Toasts ----------
function toast(msg, kind="info", ms=2400) {
  const t = el("div", { class: `toast ${kind}` }, msg);
  $("#toastLayer").append(t);
  setTimeout(() => { t.style.opacity = "0"; t.style.transition = "opacity .2s"; setTimeout(()=>t.remove(), 220); }, ms);
}

// ---------- Confirm ----------
function confirmSheet(title, body) {
  return new Promise((resolve) => {
    $("#cfTitle").textContent = title;
    $("#cfBody").textContent = body;
    $("#confirmScrim").hidden = false;
    const ok = () => { $("#confirmScrim").hidden = true; cleanup(); resolve(true); };
    const no = () => { $("#confirmScrim").hidden = true; cleanup(); resolve(false); };
    function cleanup() { $("#cfOk").removeEventListener("click", ok); $("#cfCancel").removeEventListener("click", no); }
    $("#cfOk").addEventListener("click", ok); $("#cfCancel").addEventListener("click", no);
  });
}

// ---------- SQL keywords & functions ----------
const SQL_KW = new Set("SELECT FROM WHERE GROUP BY HAVING ORDER LIMIT OFFSET INSERT INTO VALUES UPDATE SET DELETE CREATE TABLE INDEX VIEW MATERIALIZED SEQUENCE DROP ALTER ADD COLUMN PRIMARY KEY FOREIGN UNIQUE CHECK NOT NULL DEFAULT REFERENCES ON CONFLICT DO NOTHING RETURNING JOIN INNER LEFT RIGHT FULL OUTER CROSS USING AS WITH RECURSIVE UNION ALL EXCEPT INTERSECT CASE WHEN THEN ELSE END EXISTS IN BETWEEN LIKE ILIKE IS DISTINCT TRUE FALSE BEGIN COMMIT ROLLBACK SAVEPOINT EXPLAIN ANALYZE CASCADE GRANT REVOKE TO ROLE USER COLUMNAR PARTITION BY RANGE LIST HASH ENCRYPTED TABLESPACE TRIGGER BEFORE AFTER INSTEAD OF EACH ROW STATEMENT REFRESH MATERIALIZED VIEW".split(/\s+/));
const SQL_FN = new Set("COUNT SUM AVG MIN MAX COALESCE CAST EXTRACT NOW CURRENT_DATE CURRENT_TIMESTAMP LOWER UPPER LENGTH SUBSTRING TO_TIMESTAMP TO_CHAR JSON_EXTRACT XPATH GEN_RANDOM_UUID".split(/\s+/));

function tokenize(sql) {
  const out = [];
  let i = 0;
  while (i < sql.length) {
    const c = sql[i];
    if (/\s/.test(c)) { let j=i; while (j<sql.length && /\s/.test(sql[j])) j++; out.push({t:"ws", v:sql.slice(i,j)}); i=j; continue; }
    if (c === "-" && sql[i+1] === "-") { let j = sql.indexOf("\n", i); if (j<0) j=sql.length; out.push({t:"com", v:sql.slice(i,j)}); i=j; continue; }
    if (c === "/" && sql[i+1] === "*") { let j = sql.indexOf("*/", i+2); if (j<0) j=sql.length; else j+=2; out.push({t:"com", v:sql.slice(i,j)}); i=j; continue; }
    if (c === "'" || c === '"') {
      let j = i+1; while (j < sql.length && sql[j] !== c) { if (sql[j] === "\\") j++; j++; }
      j++; out.push({t:"str", v:sql.slice(i,j)}); i=j; continue;
    }
    if (/[0-9]/.test(c) || (c === "." && /[0-9]/.test(sql[i+1]))) {
      let j=i; while (j<sql.length && /[0-9.]/.test(sql[j])) j++;
      out.push({t:"num", v:sql.slice(i,j)}); i=j; continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j=i; while (j<sql.length && /[A-Za-z0-9_]/.test(sql[j])) j++;
      const v = sql.slice(i,j); const up = v.toUpperCase();
      const t = SQL_KW.has(up) ? "kw" : SQL_FN.has(up) ? "fn" : "id";
      out.push({ t, v }); i=j; continue;
    }
    out.push({ t: "op", v: c }); i++;
  }
  return out;
}
function highlightSql(sql) {
  return tokenize(sql).map(tok => {
    if (tok.t === "ws") return escHtml(tok.v);
    return `<span class="${tok.t}">${escHtml(tok.v)}</span>`;
  }).join("");
}

// ---------- SQL formatter (very simple) ----------
function formatSql(sql) {
  const keywords = ["SELECT","FROM","WHERE","GROUP BY","HAVING","ORDER BY","LIMIT","OFFSET","JOIN","LEFT JOIN","RIGHT JOIN","INNER JOIN","UNION","UNION ALL","RETURNING"];
  let s = sql.replace(/\s+/g, " ").trim();
  for (const k of keywords) {
    const re = new RegExp("\\b" + k.replace(" ", "\\s+") + "\\b", "gi");
    s = s.replace(re, "\n" + k);
  }
  return s.trim();
}

// ---------- Statement splitter ----------
function splitStatements(sql) {
  const out = []; let buf = ""; let inStr = null;
  for (let i = 0; i < sql.length; i++) {
    const c = sql[i];
    if (inStr) { buf += c; if (c === inStr && sql[i-1] !== "\\") inStr = null; continue; }
    if (c === "'" || c === '"') { inStr = c; buf += c; continue; }
    if (c === "-" && sql[i+1] === "-") { while (i < sql.length && sql[i] !== "\n") { buf += sql[i++]; } continue; }
    if (c === ";") { if (buf.trim()) out.push(buf.trim()); buf = ""; continue; }
    buf += c;
  }
  if (buf.trim()) out.push(buf.trim());
  return out;
}

// ---------- Sidebar render ----------
function renderSidebar() {
  renderConnections();
  renderTables();
  renderViews();
  renderMViews();
  renderIndexes();
  renderSequences();
  renderScripts();
  renderTriggersList();
  renderTablespaces();
  renderSaved();
}

function renderConnections() {
  const list = $("#connList"); list.replaceChildren();
  for (const c of CONNECTIONS) {
    list.append(el("li", {
      class: cls("conn", c.status === "off" && "off", "env"+c.env, state.activeConn === c.id && "active"),
      title: c.path,
      onclick: () => { state.activeConn = c.id; renderConnections(); updateTitlebar(); updateStatus(); updateEnvStrip(); persist(); },
    },
      el("span", { class: "dot" }),
      el("span", { class: "name" }, c.name),
      el("span", { class: "envtag" }, c.env),
    ));
  }
  // Also fill query connection select
  const sel = $("#qtConn");
  if (sel) {
    sel.replaceChildren();
    for (const c of CONNECTIONS.filter(x => x.status === "on"))
      sel.append(el("option", { value: c.id, selected: c.id === state.activeConn }, c.name));
  }
  // Compare selects
  for (const id of ["cmpA","cmpB"]) {
    const s = $("#"+id); if (!s) continue;
    s.replaceChildren();
    for (const c of CONNECTIONS) s.append(el("option", { value: c.id }, c.name));
  }
  if ($("#cmpA")) $("#cmpA").value = "local";
  if ($("#cmpB")) $("#cmpB").value = "staging";
  // Set selects for users / scripts / info / import
  const sa = $("#actAsUser"); if (sa) { sa.replaceChildren(); for (const u of USERS.filter(u=>u.kind==="user")) sa.append(el("option", {}, u.name)); }
  const ss = $("#scrSelect"); if (ss) { ss.replaceChildren(); for (const s of SCRIPTS) ss.append(el("option", {}, s.name)); }
  const is = $("#infoSelect"); if (is) { is.replaceChildren(); for (const k of Object.keys(INFO_TABLES)) is.append(el("option", { value: k }, "information_schema."+k)); }
  const it = $("#impTarget"); if (it) { it.replaceChildren(); for (const t of Object.keys(SCHEMA)) it.append(el("option", {}, t)); }
  const wts = $("#wizTs"); if (wts) { wts.replaceChildren(); for (const t of TABLESPACES) wts.append(el("option", {}, t.name)); }
  const dbgs = $("#dbgProc"); if (dbgs && !dbgs.children.length) { dbgs.append(el("option", {}, "promote_customer(p_id INT)")); dbgs.append(el("option", {}, "monthly_report()")); }
}

function renderTables() {
  const tree = $("#tableTree"); tree.replaceChildren();
  const f = $("#tableFilter").value || "";
  const names = Object.keys(SCHEMA).filter(n => n.toLowerCase().includes(f.toLowerCase()));
  for (const name of names) {
    const t = SCHEMA[name];
    tree.append(el("li", {
      class: cls(state.activeTable === name && "active"),
      onclick: () => { state.activeTable = name; setView("browse"); setPane("rows"); state.selectedRow = -1; state.page = 1; state.filters = { table: name, conds: [] }; state.sort = { col: null, dir: "asc" }; state.dirty.clear(); renderTables(); renderBrowse(); updateStatus(); persist(); },
    },
      el("span", { class: "ico" }, t.icon),
      el("span", { class: "name" }, name),
      ...(t.flags.length ? [el("span", { class: "flag" }, t.flags[0].slice(0,4))] : []),
      el("span", { class: "badge" }, String(t.rows.length)),
    ));
  }
  $("#cntTables").textContent = Object.keys(SCHEMA).length;
}
function renderViews() {
  const tree = $("#viewTree"); tree.replaceChildren();
  for (const v of VIEWS) {
    tree.append(el("li", { onclick: () => { loadIntoEditor(v.def); setView("query"); }, title: v.def },
      el("span", { class: "ico" }, "◫"),
      el("span", { class: "name" }, v.name),
    ));
  }
  $("#cntViews").textContent = VIEWS.length;
}
function renderMViews() {
  const tree = $("#mviewTree"); tree.replaceChildren();
  for (const v of MVIEWS) {
    tree.append(el("li", { onclick: () => { loadIntoEditor("REFRESH MATERIALIZED VIEW " + v.name + ";"); setView("query"); }, title: v.def },
      el("span", { class: "ico" }, "◬"),
      el("span", { class: "name" }, v.name),
      el("span", { class: "badge" }, v.rows),
    ));
  }
  $("#cntMViews").textContent = MVIEWS.length;
}
function renderIndexes() {
  const tree = $("#idxTree"); tree.replaceChildren();
  for (const i of INDEXES) {
    tree.append(el("li", { title: `${i.type} on ${i.table}(${i.cols.join(", ")})` },
      el("span", { class: "ico" }, "⌗"),
      el("span", { class: "name" }, i.name),
      el("span", { class: "flag" }, i.type.replace("+","").slice(0,4)),
    ));
  }
  $("#cntIdx").textContent = INDEXES.length;
}
function renderSequences() {
  const tree = $("#seqTree"); tree.replaceChildren();
  for (const s of SEQUENCES) {
    tree.append(el("li", { title: `current ${s.current}` },
      el("span", { class: "ico" }, "↻"),
      el("span", { class: "name" }, s.name),
      el("span", { class: "badge" }, s.current),
    ));
  }
  $("#cntSeq").textContent = SEQUENCES.length;
}
function renderScripts() {
  const tree = $("#scrTree"); tree.replaceChildren();
  for (const s of SCRIPTS) {
    tree.append(el("li", {
      title: `${s.lang} · ${s.jit} · ${s.calls} calls`,
      onclick: () => { setView("admin"); setAdminPane("scripts"); state.scriptCurrent = s.name; renderScriptEditor(); },
    },
      el("span", { class: "ico" }, s.lang === "js" ? "🛈" : "λ"),
      el("span", { class: "name" }, s.name),
      el("span", { class: "flag" }, s.lang),
    ));
  }
  $("#cntScr").textContent = SCRIPTS.length;
}
function renderTriggersList() {
  const tree = $("#trgTree"); tree.replaceChildren();
  for (const t of TRIGGERS) {
    tree.append(el("li", {
      title: `${t.timing} ${t.event} on ${t.table}`,
      onclick: () => { setView("admin"); setAdminPane("triggers"); renderAdminTriggers(); },
    },
      el("span", { class: "ico" }, t.enabled ? "▶" : "■"),
      el("span", { class: "name" }, t.name),
    ));
  }
  $("#cntTrg").textContent = TRIGGERS.length;
}
function renderTablespaces() {
  const tree = $("#tsTree"); tree.replaceChildren();
  for (const t of TABLESPACES) {
    tree.append(el("li", { title: t.path },
      el("span", { class: "ico" }, t.encrypted ? "🔒" : "▤"),
      el("span", { class: "name" }, t.name),
      el("span", { class: "flag" }, t.size.trim()),
    ));
  }
  $("#cntTs").textContent = TABLESPACES.length;
}
function renderSaved() {
  const list = $("#savedList"); list.replaceChildren();
  for (const s of state.saved) {
    list.append(el("li", {
      title: s.sql,
      onclick: () => { loadIntoEditor(s.sql); setView("query"); },
    }, el("span", { class: "name" }, "★ " + s.name)));
  }
}

// ---------- Title / status / env ----------
function updateTitlebar() {
  const c = CONNECTIONS.find(x => x.id === state.activeConn);
  $("#tbTitle").textContent = c.name;
  const objs = Object.keys(SCHEMA).length + VIEWS.length + MVIEWS.length + INDEXES.length + SEQUENCES.length;
  $("#tbSubtitle").textContent = `${c.meta} · ${objs} objects`;
}
function updateStatus() {
  const c = CONNECTIONS.find(x => x.id === state.activeConn);
  $("#sbConn").textContent = `${c.name} · ${c.path}`;
  $("#sbTable").textContent = state.activeTable;
  const t = SCHEMA[state.activeTable];
  $("#sbSel").textContent = `${t.rows.length.toLocaleString()} rows · ${t.columns.length} columns`;
  const live = TXNS.filter(x => x.state !== "idle").length;
  $("#sbTxn").textContent = `txn: ${live} active`;
}
function updateEnvStrip() {
  const c = CONNECTIONS.find(x => x.id === state.activeConn);
  $("#envStrip").dataset.env = c.env;
}

// ---------- Browse view ----------
function visibleRows(name) {
  const t = SCHEMA[name];
  let rows = t.rows.map((r, i) => ({ r, i }));
  // Filter
  if (state.filters.table === name) {
    for (const c of state.filters.conds) {
      const idx = t.columns.findIndex(x => x.name === c.col);
      if (idx < 0) continue;
      rows = rows.filter(({r}) => matchCond(r[idx], c.op, c.val));
    }
  }
  // Sort
  if (state.sort.col) {
    const idx = t.columns.findIndex(x => x.name === state.sort.col);
    if (idx >= 0) {
      const dir = state.sort.dir === "asc" ? 1 : -1;
      rows.sort(({r:a},{r:b}) => {
        const av = a[idx], bv = b[idx];
        if (av === bv) return 0;
        if (av === null) return 1;
        if (bv === null) return -1;
        return av > bv ? dir : -dir;
      });
    }
  }
  return rows;
}
function matchCond(v, op, val) {
  if (v === null) return op === "is null";
  switch (op) {
    case "=": return String(v) === val;
    case "!=": return String(v) !== val;
    case ">": return Number(v) > Number(val);
    case "<": return Number(v) < Number(val);
    case ">=": return Number(v) >= Number(val);
    case "<=": return Number(v) <= Number(val);
    case "like": return String(v).toLowerCase().includes(val.toLowerCase());
    case "is null": return v === null;
    case "is not null": return v !== null;
  }
  return true;
}

function renderBrowse() {
  const t = SCHEMA[state.activeTable];
  $("#browseTableName").textContent = state.activeTable;
  $("#browseRowCount").textContent = `${t.rows.length.toLocaleString()} rows`;
  $("#browseIcon").textContent = t.icon;
  $("#browseFlags").textContent = t.flags.join(" · ");

  // Filter builder
  renderFilterBuilder();

  // Visible rows
  const vis = visibleRows(state.activeTable);
  const pageSize = state.settings.pageSize;
  const totalPages = Math.max(1, Math.ceil(vis.length / pageSize));
  if (state.page > totalPages) state.page = totalPages;
  const start = (state.page - 1) * pageSize;
  const pageRows = vis.slice(start, start + pageSize);
  $("#pgInfo").textContent = `Page ${state.page} of ${totalPages} · ${vis.length.toLocaleString()} rows`;
  $("#pgPrev").disabled = state.page <= 1;
  $("#pgNext").disabled = state.page >= totalPages;

  // Data table
  const dt = $("#dataTable");
  const thead = dt.querySelector("thead"); thead.replaceChildren();
  const tbody = dt.querySelector("tbody"); tbody.replaceChildren();
  const headRow = el("tr");
  t.columns.forEach(c => {
    if (state.hiddenCols.has(c.name)) return;
    const arrow = state.sort.col === c.name ? (state.sort.dir === "asc" ? "↑" : "↓") : "";
    headRow.append(el("th", { onclick: () => { state.sort = { col: c.name, dir: state.sort.col === c.name && state.sort.dir === "asc" ? "desc" : "asc" }; renderBrowse(); } }, c.name, arrow ? el("span", { class: "sort" }, arrow) : null));
  });
  thead.append(headRow);

  const fkByCol = Object.fromEntries((t.fks || []).map(f => [f.col, f]));

  pageRows.forEach(({ r: row, i }) => {
    const tr = el("tr", {
      class: cls(state.selectedRow === i && "sel"),
      onclick: () => { state.selectedRow = i; renderBrowse(); },
    });
    t.columns.forEach((c, ci) => {
      if (state.hiddenCols.has(c.name)) return;
      const cell = row[ci];
      const isNum = /INT|DECIMAL|FLOAT|BIGINT|NUMERIC/.test(c.type);
      const isCode = /JSON|UUID|GEOMETRY/.test(c.type);
      const fk = fkByCol[c.name];
      const dirtyKey = `${state.activeTable}#${i}#${c.name}`;
      const isDirty = state.dirty.has(dirtyKey);
      const td = el("td", {
        class: cls(c.key === "PK" && "pk", isNum && "num", isCode && "code", cell === null && "null", isDirty && "dirty", fk && "fk-link"),
        contenteditable: c.key === "PK" ? null : "true",
        oninput: (e) => editCell(i, c.name, e.target.textContent),
        ondblclick: (e) => { if (isCode || String(cell).length > 40) { e.stopPropagation(); openCellModal(c.name, cell); } },
        onclick: (e) => { if (fk) { e.stopPropagation(); navigateFk(fk, cell); } },
      }, cell === null ? "NULL" : (isNum ? fmtNum(cell) : String(cell)));
      tr.append(td);
    });
    tbody.append(tr);
  });

  // Dirty / commit
  $("#dirtyInfo").classList.toggle("hidden", state.dirty.size === 0);
  $("#dirtyInfo").textContent = `${state.dirty.size} unsaved change${state.dirty.size===1?"":"s"}`;
  $("#commitBtn").disabled = state.dirty.size === 0;
  $("#revertBtn").disabled = state.dirty.size === 0;

  // Structure
  const sb = $("#schemaTable").querySelector("tbody"); sb.replaceChildren();
  for (const c of t.columns) {
    const keyTag = c.key === "PK" ? el("span", { class: "tag" }, "PK")
                : c.key === "FK" ? el("span", { class: "tag fk" }, "FK")
                : c.key === "UQ" ? el("span", { class: "tag uq" }, "UQ")
                : null;
    sb.append(el("tr", {},
      el("td", { class: "pk" }, c.name),
      el("td", {}, el("span", { class: "type" }, c.type)),
      el("td", {}, c.nullable ? "YES" : "NO"),
      el("td", { class: "code" }, c.default ?? "—"),
      el("td", {}, keyTag ?? "—"),
      el("td", {}, c.comment || ""),
    ));
  }

  // Indexes
  const ib = $("#tableIndexBody"); ib.replaceChildren();
  const idxs = INDEXES.filter(i => i.table === state.activeTable);
  if (!idxs.length) ib.append(el("tr", {}, el("td", { colspan: 5, class: "null" }, "No custom indexes.")));
  else for (const i of idxs) ib.append(el("tr", {},
    el("td", { class: "pk" }, i.name),
    el("td", {}, i.type),
    el("td", { class: "code" }, i.cols.join(", ")),
    el("td", {}, i.unique ? "YES" : "NO"),
    el("td", { class: "num" }, i.size),
  ));
  renderRTreeViz(state.activeTable);

  // Constraints
  const cp = $("#constraintsPane"); cp.replaceChildren();
  if (!t.constraints.length) {
    cp.append(el("div", { class: "empty-pane" }, el("div", { class: "empty-glyph" }, "⌗"), el("h3", {}, "No constraints"), el("p", {}, "This table has no declared constraints.")));
  } else {
    for (const k of t.constraints) {
      cp.append(el("div", { class: "constraint-card" },
        el("div", { class: "kind" }, k.kind),
        el("h4", {}, k.name),
        el("pre", {}, k.def),
      ));
    }
  }

  // Table triggers
  const tb = $("#tableTrigBody"); tb.replaceChildren();
  const trgs = TRIGGERS.filter(x => x.table === state.activeTable);
  if (!trgs.length) tb.append(el("tr", {}, el("td", { colspan: 6, class: "null" }, "No triggers on this table.")));
  else for (const x of trgs) tb.append(el("tr", {},
    el("td", {}, el("label", { class: "switch" }, el("input", { type: "checkbox", checked: x.enabled, onchange: e => { x.enabled = e.target.checked; toast(`Trigger ${x.name} ${x.enabled?"enabled":"disabled"}.`, "info"); } }), el("span"))),
    el("td", { class: "pk" }, x.name),
    el("td", {}, x.timing),
    el("td", {}, x.event),
    el("td", {}, x.forEach),
    el("td", { class: "code" }, x.body.slice(0, 80) + (x.body.length > 80 ? "…" : "")),
  ));

  // Partitions
  const pp = $("#partitionsPane"); pp.replaceChildren();
  if (!t.partitions) {
    pp.append(el("div", { class: "empty-pane" }, el("div", { class: "empty-glyph" }, "▦"), el("h3", {}, "Not partitioned"), el("p", {}, "Add a partition clause via the New Table wizard.")));
  } else {
    pp.append(el("div", { class: "chip chip-soft" }, t.partitions.by));
    for (const p of t.partitions.list) {
      pp.append(el("div", { class: "part-row" },
        el("div", {}, el("div", { class: "name" }, p.name), el("div", { class: "meta" }, p.range)),
        el("div", {}, `${p.rows.toLocaleString()} rows · ${p.size}`),
        el("div", {},
          el("button", { class: "btn btn-soft", onclick: () => toast(`DETACH PARTITION ${p.name}`, "warn") }, "Detach"),
          " ",
          el("button", { class: "btn btn-soft", onclick: () => toast(`Pruned analysis for ${p.name}`, "info") }, "Prune?"),
        ),
      ));
    }
  }

  // Stats
  const sg = $("#statsGrid"); sg.replaceChildren();
  const stats = [
    ["Rows", t.rows.length.toLocaleString(), ""],
    ["Columns", t.columns.length, ""],
    ["Storage", t.storage.kind.toUpperCase(), t.storage.kind === "columnar" ? `zstd ${t.storage.ratio}×` : ""],
    ["Encrypted", t.storage.encrypted ? "YES" : "NO", t.storage.encrypted ? "AES-256-GCM" : ""],
    ["Tablespace", t.storage.tablespace, ""],
    ["Indexes", INDEXES.filter(i=>i.table===state.activeTable).length, ""],
    ["Triggers", TRIGGERS.filter(x=>x.table===state.activeTable).length, ""],
    ["Partitions", t.partitions ? t.partitions.list.length : 0, t.partitions ? t.partitions.by : ""],
    ["Avg row size", Math.round(160 + Math.random()*40) + " B", ""],
    ["Last analyzed", "today, 04:00", ""],
  ];
  for (const [k,v,s] of stats) sg.append(el("div", { class: "stat-card" },
    el("div", { class: "k" }, k), el("div", { class: "v" }, v), s && el("div", { class: "s" }, s)
  ));
}

function renderFilterBuilder() {
  if (state.filters.table !== state.activeTable) state.filters = { table: state.activeTable, conds: [] };
  const list = $("#fbList"); list.replaceChildren();
  const t = SCHEMA[state.activeTable];
  state.filters.conds.forEach((c, ix) => {
    const colSel = el("select", { onchange: e => { c.col = e.target.value; renderFilterBuilder(); } });
    t.columns.forEach(x => colSel.append(el("option", { value: x.name, selected: x.name === c.col }, x.name)));
    const opSel = el("select", { onchange: e => { c.op = e.target.value; renderFilterBuilder(); } });
    ["=","!=",">","<",">=","<=","like","is null","is not null"].forEach(o => opSel.append(el("option", { selected: o === c.op }, o)));
    const valIn = el("input", { type: "text", value: c.val, oninput: e => { c.val = e.target.value; } });
    list.append(el("div", { class: "fb-row" }, colSel, opSel, valIn,
      el("button", { class: "x", onclick: () => { state.filters.conds.splice(ix, 1); renderFilterBuilder(); } }, "×")));
  });
  if (!state.filters.conds.length) list.append(el("div", { class: "chip chip-soft" }, "No conditions"));
}

// ---------- Editing ----------
function editCell(rowIdx, colName, value) {
  const key = `${state.activeTable}#${rowIdx}#${colName}`;
  const t = SCHEMA[state.activeTable];
  const cIdx = t.columns.findIndex(c => c.name === colName);
  const original = t.rows[rowIdx][cIdx];
  if (String(original) === value) state.dirty.delete(key);
  else state.dirty.set(key, { table: state.activeTable, rowIdx, colName, original, value });
  $("#dirtyInfo").classList.toggle("hidden", state.dirty.size === 0);
  $("#dirtyInfo").textContent = `${state.dirty.size} unsaved change${state.dirty.size===1?"":"s"}`;
  $("#commitBtn").disabled = state.dirty.size === 0;
  $("#revertBtn").disabled = state.dirty.size === 0;
}
async function commitEdits() {
  if (state.settings.confirmDDL) {
    const ok = await confirmSheet("Commit changes?", `${state.dirty.size} row edit${state.dirty.size===1?"":"s"} will be written.`);
    if (!ok) return;
  }
  for (const v of state.dirty.values()) {
    const t = SCHEMA[v.table];
    const ci = t.columns.findIndex(c => c.name === v.colName);
    t.rows[v.rowIdx][ci] = v.value;
  }
  toast(`Committed ${state.dirty.size} change${state.dirty.size===1?"":"s"}.`, "ok");
  state.dirty.clear();
  renderBrowse();
}
function revertEdits() {
  state.dirty.clear();
  renderBrowse();
  toast("Reverted changes.", "info");
}

// ---------- FK navigation ----------
function navigateFk(fk, value) {
  if (!SCHEMA[fk.table]) return;
  state.activeTable = fk.table;
  state.filters = { table: fk.table, conds: [{ col: fk.refCol, op: "=", val: String(value) }] };
  state.page = 1;
  setView("browse"); setPane("rows");
  renderTables(); renderBrowse(); updateStatus();
  toast(`Followed FK to ${fk.table}.${fk.refCol} = ${value}`, "info");
}

// ---------- Cell modal ----------
function openCellModal(col, val) {
  $("#cellTitle").textContent = col;
  let body = val == null ? "NULL" : String(val);
  try { body = JSON.stringify(JSON.parse(body), null, 2); } catch {}
  $("#cellBody").textContent = body;
  $("#cellScrim").hidden = false;
  $("#cellCopy").onclick = () => { navigator.clipboard?.writeText(body); toast("Copied.", "ok"); };
}

// ---------- R-Tree viz ----------
function renderRTreeViz(table) {
  const wrap = $("#rtreeViz"); wrap.replaceChildren();
  const rtree = INDEXES.find(i => i.table === table && i.type.startsWith("R"));
  if (!rtree) { wrap.append(el("div", { class: "empty-pane" }, el("div", { class: "empty-glyph" }, "◰"), el("h3", {}, "No spatial index"), el("p", {}, "R-Tree visualization appears here when a spatial index exists."))); return; }
  const svg = svgEl("svg", { viewBox: "0 0 600 240" });
  // root MBR
  svg.append(svgEl("rect", { x: 16, y: 16, width: 568, height: 208, fill: "rgba(10,132,255,0.06)", stroke: "rgba(10,132,255,0.6)", "stroke-dasharray": "4 3", rx: 6 }));
  // child MBRs
  const childs = [
    { x: 40, y: 40, w: 230, h: 100 },
    { x: 290, y: 50, w: 240, h: 120 },
    { x: 60, y: 160, w: 180, h: 60 },
    { x: 320, y: 180, w: 220, h: 40 },
  ];
  childs.forEach((c, i) => {
    svg.append(svgEl("rect", { x: c.x, y: c.y, width: c.w, height: c.h, fill: "rgba(255,149,0,0.08)", stroke: "rgba(255,149,0,0.7)", "stroke-width": 1, rx: 4 }));
    svg.append(svgEl("text", { x: c.x + 6, y: c.y + 14, "font-size": 10, fill: "currentColor" }, `MBR ${i+1}`));
  });
  // points (locations rows)
  const pts = SCHEMA.locations.rows.map(r => {
    const m = /POINT\(([-\d.]+)\s+([-\d.]+)\)/.exec(r[2]); if (!m) return null;
    const lon = parseFloat(m[1]), lat = parseFloat(m[2]);
    // map roughly to viewport
    return { x: 40 + ((lon + 130) / 130) * 520, y: 200 - ((lat - 30) / 30) * 180, label: r[1] };
  }).filter(Boolean);
  for (const p of pts) {
    svg.append(svgEl("circle", { cx: p.x, cy: p.y, r: 3.5, fill: "var(--accent)" }));
    svg.append(svgEl("text", { x: p.x + 6, y: p.y + 3, "font-size": 10, fill: "currentColor" }, p.label));
  }
  wrap.append(svg);
}

// ---------- View / pane switching ----------
function setView(v) {
  state.view = v;
  $$(".seg").forEach(b => b.classList.toggle("active", b.dataset.view === v));
  $$(".view").forEach(p => p.classList.toggle("active", p.dataset.view === v));
  if (v === "er") renderER();
  if (v === "admin") renderAdmin();
}
function setPane(p) {
  state.pane = p;
  $$(".bh-tabs .pill").forEach(b => b.classList.toggle("active", b.dataset.pane === p));
  $$(".view-browse .pane").forEach(x => x.classList.toggle("active", x.dataset.pane === p));
}
function setRTab(r) {
  state.rtab = r;
  $$(".rh-tabs .pill").forEach(b => b.classList.toggle("active", b.dataset.rtab === r));
  $$(".rtab").forEach(x => x.classList.toggle("active", x.dataset.rtab === r));
}
function setAdminPane(name) {
  state.adminPane = name;
  $$(".anv").forEach(b => b.classList.toggle("active", b.dataset.anv === name));
  $$(".apane").forEach(p => p.classList.toggle("active", p.dataset.apane === name));
}

// ---------- Editor binding ----------
function loadIntoEditor(sql) {
  $("#editor").value = sql;
  state.queries[state.activeQ].sql = sql;
  syncEditor();
}
function syncEditor() {
  const text = $("#editor").value;
  const lines = text.split("\n").length || 1;
  $("#gutter").textContent = Array.from({ length: lines }, (_, i) => i + 1).join("\n");
  $("#hl").innerHTML = highlightSql(text) + "\n";
}
function setEditorFont(n) {
  document.documentElement.style.setProperty("--editor-font", n + "px");
  for (const e of [$("#editor"), $("#hl"), $("#gutter"), $("#scrEditor"), $("#docSrc"), $("#docExpr"), $("#impData")]) {
    if (e) e.style.fontSize = n + "px";
  }
}

// ---------- Autocomplete ----------
const AC = { open: false, idx: 0, items: [], start: 0 };
function showAutocomplete() {
  const ed = $("#editor"); const ac = $("#ac");
  const caret = ed.selectionStart; const text = ed.value;
  // find word start
  let s = caret;
  while (s > 0 && /[A-Za-z0-9_.]/.test(text[s-1])) s--;
  const tok = text.slice(s, caret);
  if (!tok || tok.length < 1) { ac.hidden = true; AC.open = false; return; }
  const upper = tok.toUpperCase();
  const items = [];
  for (const k of SQL_KW) if (k.startsWith(upper)) items.push({ kind: "kw", v: k });
  for (const k of SQL_FN) if (k.startsWith(upper)) items.push({ kind: "fn", v: k });
  for (const name of Object.keys(SCHEMA)) if (name.toUpperCase().startsWith(upper)) items.push({ kind: "tbl", v: name });
  for (const name of Object.keys(SCHEMA)) for (const c of SCHEMA[name].columns) if (c.name.toUpperCase().startsWith(upper)) items.push({ kind: "col", v: c.name });
  if (!items.length) { ac.hidden = true; AC.open = false; return; }
  AC.items = items.slice(0, 12); AC.idx = 0; AC.start = s; AC.open = true;
  // Position naively: place above results
  const rect = ed.getBoundingClientRect();
  ac.style.left = "16px"; ac.style.top = "10px";
  ac.replaceChildren();
  AC.items.forEach((it, i) => ac.append(el("div", {
    class: cls("item", i === 0 && "sel"),
    onclick: () => { AC.idx = i; acceptAutocomplete(); },
  }, el("span", { class: "kind" }, it.kind), it.v)));
  ac.hidden = false;
}
function moveAc(d) {
  if (!AC.open) return;
  AC.idx = (AC.idx + d + AC.items.length) % AC.items.length;
  $$(".ac .item").forEach((n,i) => n.classList.toggle("sel", i === AC.idx));
}
function acceptAutocomplete() {
  if (!AC.open) return false;
  const ed = $("#editor");
  const text = ed.value;
  const after = ed.selectionStart;
  ed.value = text.slice(0, AC.start) + AC.items[AC.idx].v + text.slice(after);
  ed.selectionStart = ed.selectionEnd = AC.start + AC.items[AC.idx].v.length;
  $("#ac").hidden = true; AC.open = false;
  syncEditor();
  return true;
}

// ---------- Query run (multi-statement) ----------
function runQuery() {
  const sql = $("#editor").value.trim();
  if (!sql) return;
  setView("query");
  const stmts = splitStatements(sql);
  const out = $("#multiResults"); out.replaceChildren();
  let totalRows = 0, totalMs = 0; const msgs = []; const plans = [];
  state.prevResult = state.lastResult;
  let last = null;
  for (const s of stmts) {
    const block = el("div", { class: "stmt-block" });
    const t0 = performance.now();
    let res, msg, plan;
    if (state.settings.confirmDDL && /^\s*(drop|delete)\b/i.test(s) && !/where/i.test(s)) {
      // synchronous flow: just warn via toast
      toast("Confirm prompt skipped for inline run — see Settings.", "warn");
    }
    try { ({ res, msg, plan } = mockExec(s)); }
    catch (e) { res = { columns: [{name:"error",type:"TEXT"}], rows: [[String(e.message || e)]] }; msg = "ERROR: " + (e.message || e); plan = ""; }
    const ms = Math.max(1, Math.round(performance.now() - t0));
    totalRows += res.rows.length; totalMs += ms;
    last = { stmt: s, res, ms };
    msgs.push(msg + " (" + ms + " ms)"); plans.push(`-- ${s.slice(0,60)}\n${plan || "—"}`);
    const head = el("div", { class: "stmt-block-head" },
      el("span", {}, `${stmts.indexOf(s)+1}.`),
      el("span", { class: "code", style: "font-family: var(--mono); font-size:11.5px;" }, s.slice(0, 80) + (s.length>80?"…":"")),
      el("span", { class: cls(msg.startsWith("ERROR") ? "err" : "ok") }, msg),
      el("span", { class: "sb-spacer", style: "flex:1" }),
      el("span", {}, `${res.rows.length} rows · ${ms} ms`),
    );
    const wrap = el("div", { class: "table-wrap", style: "max-height:240px;" });
    const tbl = el("table", { class: "data-table" });
    const thead = el("thead"); const trh = el("tr");
    res.columns.forEach(c => trh.append(el("th", {}, c.name)));
    thead.append(trh); tbl.append(thead);
    const tb = el("tbody");
    res.rows.forEach(r => {
      const tr = el("tr");
      r.forEach((v, i) => {
        const c = res.columns[i];
        const isNum = c && /INT|DECIMAL|FLOAT|BIGINT|NUMERIC/.test(c.type);
        tr.append(el("td", {
          class: cls(isNum && "num", v === null && "null"),
          ondblclick: () => openCellModal(c.name, v),
        }, v === null ? "NULL" : (isNum ? fmtNum(v) : String(v))));
      });
      tb.append(tr);
    });
    tbl.append(tb); wrap.append(tbl);
    block.append(head, wrap);
    out.append(block);
  }
  state.lastResult = last && last.res;
  $("#messageLog").textContent = msgs.join("\n");
  $("#planLog").textContent = plans.join("\n\n");
  $("#resultMeta").textContent = `${totalRows} row${totalRows===1?"":"s"} · ${totalMs} ms · ${stmts.length} stmt${stmts.length===1?"":"s"}`;
  setRTab("result");
  state.history.unshift({ sql, rows: totalRows, ms: totalMs, when: "just now" });
  renderHistory($("#historyFilter").value);
  $("#sbLatency").textContent = `${totalMs} ms`;
  persist();
}

function mockExec(sql) {
  if (/^\s*explain/i.test(sql)) {
    return {
      res: { columns: [{name:"plan",type:"TEXT"}], rows: planFor(sql.replace(/^\s*explain\s+/i, "")).map(s => [s]) },
      msg: "OK · EXPLAIN",
      plan: "see Result tab",
    };
  }
  if (/^\s*(insert|update|delete|create|drop|alter|grant|revoke|begin|commit|rollback|savepoint|refresh|truncate)\b/i.test(sql)) {
    return { res: { columns: [{name:"result",type:"TEXT"}], rows: [["Statement executed."]] }, msg: "OK", plan: "DDL/DML — no plan." };
  }
  const m = sql.match(/from\s+([a-zA-Z_][\w]*)/i);
  if (!m || !SCHEMA[m[1]]) return { res: { columns:[{name:"result",type:"TEXT"}], rows:[["(no rows)"]] }, msg: "OK", plan: "—" };
  const tbl = SCHEMA[m[1]];
  let rows = tbl.rows.slice();
  const w = sql.match(/where\s+([a-z_][\w]*)\s*(=|>=|<=|>|<|!=)\s*('([^']*)'|([\d.]+))/i);
  if (w) {
    const cidx = tbl.columns.findIndex(c => c.name.toLowerCase() === w[1].toLowerCase());
    const op = w[2]; const v = w[4] !== undefined ? w[4] : w[5];
    if (cidx >= 0) {
      rows = rows.filter(r => {
        const cv = r[cidx];
        if (cv === null) return false;
        if (["=","!="].includes(op)) return op === "=" ? String(cv) === v : String(cv) !== v;
        return ({ ">":Number(cv)>Number(v), "<":Number(cv)<Number(v), ">=":Number(cv)>=Number(v), "<=":Number(cv)<=Number(v) })[op];
      });
    }
  }
  const ord = sql.match(/order\s+by\s+([a-z_][\w]*)\s*(asc|desc)?/i);
  if (ord) {
    const cidx = tbl.columns.findIndex(c => c.name.toLowerCase() === ord[1].toLowerCase());
    if (cidx >= 0) { const dir = (ord[2] || "asc").toLowerCase() === "asc" ? 1 : -1;
      rows.sort((a,b) => { const x=a[cidx], y=b[cidx]; if (x===y) return 0; if (x===null) return 1; if (y===null) return -1; return x > y ? dir : -dir; });
    }
  }
  const lim = sql.match(/limit\s+(\d+)/i); if (lim) rows = rows.slice(0, parseInt(lim[1], 10));
  // Projection
  let cols = tbl.columns, idx = tbl.columns.map((_,i)=>i);
  const sel = sql.match(/select\s+([\s\S]+?)\s+from/i);
  if (sel && sel[1].trim() !== "*") {
    const names = sel[1].split(",").map(s => s.trim().replace(/\s+as\s+.+$/i, "").split(".").pop());
    const filtered = names.map(n => tbl.columns.findIndex(c => c.name.toLowerCase() === n.toLowerCase())).filter(i => i >= 0);
    if (filtered.length) { idx = filtered; cols = filtered.map(i => tbl.columns[i]); }
  }
  const projected = rows.map(r => idx.map(i => r[i]));
  return {
    res: { columns: cols, rows: projected },
    msg: `OK · ${projected.length} row${projected.length===1?"":"s"}`,
    plan: planFor(sql).join("\n"),
  };
}
function planFor(sql) {
  const m = sql.match(/from\s+([a-zA-Z_][\w]*)/i);
  if (!m || !SCHEMA[m[1]]) return ["Scan"];
  const t = SCHEMA[m[1]];
  const out = [];
  out.push(`${t.storage.kind === "columnar" ? "Columnar" : "Seq"} Scan on ${m[1]} (rows=${t.rows.length})`);
  if (/where/i.test(sql)) out.push("  Filter: <predicate>");
  if (/order\s+by/i.test(sql)) out.push("Sort");
  if (/limit/i.test(sql)) out.push("Limit");
  return out;
}

// ---------- Diff ----------
function runDiff() {
  if (!state.lastResult || !state.prevResult) { toast("Run two queries first.", "warn"); return; }
  const a = state.prevResult, b = state.lastResult;
  const aKeys = new Set(a.rows.map(r => JSON.stringify(r)));
  const bKeys = new Set(b.rows.map(r => JSON.stringify(r)));
  const ad = b.rows.filter(r => !aKeys.has(JSON.stringify(r)));
  const rm = a.rows.filter(r => !bKeys.has(JSON.stringify(r)));
  const wrap = $("#diffWrap"); wrap.replaceChildren();
  wrap.append(el("div", { class: "diff-stats" },
    el("span", { class: "ad" }, `+${ad.length} added`),
    el("span", { class: "rm" }, `-${rm.length} removed`),
  ));
  const tw = el("div", { class: "table-wrap" });
  const tbl = el("table", { class: "data-table diff-tbl" });
  const thead = el("thead"); const trh = el("tr");
  (b.columns.length ? b.columns : a.columns).forEach(c => trh.append(el("th", {}, c.name)));
  thead.append(trh); tbl.append(thead);
  const tb = el("tbody");
  for (const r of ad) { const tr = el("tr"); r.forEach(v => tr.append(el("td", { class: "ad" }, v === null ? "NULL" : String(v)))); tb.append(tr); }
  for (const r of rm) { const tr = el("tr"); r.forEach(v => tr.append(el("td", { class: "rm" }, v === null ? "NULL" : String(v)))); tb.append(tr); }
  tbl.append(tb); tw.append(tbl); wrap.append(tw);
  setRTab("diff");
}

// ---------- History ----------
function renderHistory(filter="") {
  const list = $("#historyList"); list.replaceChildren();
  const items = state.history.filter(h => h.sql.toLowerCase().includes(filter.toLowerCase()));
  for (const h of items) {
    list.append(el("div", {
      class: "history-item",
      ondblclick: () => { loadIntoEditor(h.sql); setView("query"); },
    },
      el("div", { class: "sql" }, h.sql),
      el("div", { class: "meta" }, `${h.rows} row${h.rows===1?"":"s"} · ${h.ms} ms · ${h.when}`),
    ));
  }
  if (!items.length) list.append(el("div", { class: "empty-pane" },
    el("div", { class: "empty-glyph" }, "◷"), el("h3", {}, "No history"), el("p", {}, "Queries you run will show up here.")));
}

// ---------- Query tabs ----------
function renderQueryTabs() {
  const wrap = $("#qtTabs"); wrap.replaceChildren();
  for (const [id, q] of Object.entries(state.queries)) {
    wrap.append(el("div", {
      class: cls("qt-tab", state.activeQ === id && "active"),
      onclick: () => switchQuery(id),
    }, q.name,
      el("span", { class: "x", onclick: (e) => { e.stopPropagation(); closeQuery(id); } }, "×"),
    ));
  }
  wrap.append(el("button", { class: "qt-new", id: "qtNew2", onclick: newQuery }, "+"));
}
function switchQuery(id) {
  state.queries[state.activeQ].sql = $("#editor").value;
  state.activeQ = id;
  $("#editor").value = state.queries[id].sql;
  syncEditor();
  renderQueryTabs();
  persist();
}
function newQuery() {
  const n = Object.keys(state.queries).length + 1;
  const id = "q" + Date.now();
  state.queries[id] = { name: "Query " + n, sql: "" };
  switchQuery(id);
  setView("query");
  $("#editor").focus();
}
function closeQuery(id) {
  const keys = Object.keys(state.queries);
  if (keys.length === 1) return;
  delete state.queries[id];
  if (state.activeQ === id) switchQuery(Object.keys(state.queries)[0]);
  else renderQueryTabs();
  persist();
}

// ---------- ER diagram ----------
function renderER() {
  const svg = $("#erSvg");
  while (svg.firstChild) svg.removeChild(svg.firstChild);
  const defs = svgEl("defs");
  defs.append(svgEl("marker", { id: "er-arrow", viewBox: "0 0 10 10", refX: 9, refY: 5, markerWidth: 8, markerHeight: 8, orient: "auto" },
    svgEl("path", { d: "M0,0 L10,5 L0,10 z", fill: "var(--orange)" })));
  svg.append(defs);

  const names = Object.keys(SCHEMA);
  const W = 220, ROW_H = 18, HEAD = 26, GAP = 40;
  const boxes = {};
  const cols = 3;
  names.forEach((n, i) => {
    const t = SCHEMA[n];
    const h = HEAD + ROW_H * t.columns.length;
    const x = 40 + (i % cols) * (W + GAP);
    const y = 40 + Math.floor(i / cols) * (h + GAP);
    boxes[n] = { x, y, w: W, h, t };
  });
  // expand canvas
  const maxY = Math.max(...Object.values(boxes).map(b => b.y + b.h)) + 40;
  const maxX = Math.max(...Object.values(boxes).map(b => b.x + b.w)) + 40;
  svg.setAttribute("viewBox", `0 0 ${maxX} ${maxY}`);
  svg.setAttribute("width",  maxX);
  svg.setAttribute("height", maxY);

  // FK lines first
  for (const [n, t] of Object.entries(SCHEMA)) {
    for (const fk of (t.fks || [])) {
      if (!boxes[fk.table]) continue;
      const a = boxes[n], b = boxes[fk.table];
      const ax = a.x + a.w, ay = a.y + 14;
      const bx = b.x, by = b.y + 14;
      const c1x = ax + 30, c2x = bx - 30;
      svg.append(svgEl("path", { class: "er-fk-line",
        d: `M ${ax} ${ay} C ${c1x} ${ay}, ${c2x} ${by}, ${bx} ${by}` }));
    }
  }
  // Tables
  for (const [n, b] of Object.entries(boxes)) {
    const g = svgEl("g");
    g.append(svgEl("rect", { class: "er-tbl", x: b.x, y: b.y, width: b.w, height: b.h, rx: 8 }));
    g.append(svgEl("rect", { class: "er-th",  x: b.x, y: b.y, width: b.w, height: HEAD, rx: 8 }));
    g.append(svgEl("rect", { class: "er-th",  x: b.x, y: b.y + 8, width: b.w, height: HEAD - 8 }));
    g.append(svgEl("text", { class: "er-th-text", x: b.x + 12, y: b.y + 17 }, n));
    b.t.columns.forEach((c, i) => {
      g.append(svgEl("text", {
        class: "er-col" + (c.key === "PK" ? " pk" : ""),
        x: b.x + 12, y: b.y + HEAD + 12 + i * ROW_H,
      }, `${c.key === "PK" ? "★ " : c.key === "FK" ? "→ " : ""}${c.name}  `,
        svgEl("tspan", { fill: "rgba(120,120,128,0.9)" }, c.type)
      ));
    });
    g.addEventListener("dblclick", () => { state.activeTable = n; setView("browse"); renderTables(); renderBrowse(); updateStatus(); });
    svg.append(g);
  }
}

// ---------- Admin: scripts ----------
function renderAdmin() {
  renderScriptEditor();
  renderAdminTriggers();
  renderWal();
  renderTxns();
  renderPrivs();
  renderInfo();
  renderDoc();
  renderDbg();
  renderSchemaCompareSelects();
}
function renderScriptEditor() {
  const s = SCRIPTS.find(x => x.name === state.scriptCurrent) || SCRIPTS[0];
  $("#scrSelect").value = s.name;
  $("#scrLang").value = s.lang;
  $("#scrEditor").value = s.body;
  $("#scrMeta").textContent = `${s.lang} · ${s.jit} · called ${s.calls}×`;
  $("#scrJit").textContent = s.jit === "JIT (LLVM)"
    ? "Tier: native (LLVM ORC LLJIT)\nIR size: 14 KB\nOpcodes lowered: 38 / 38\nFallbacks: 0"
    : s.jit === "JSC"
      ? "Engine: JavaScriptCore\nLast compile: 4 ms\nHeap: 2.4 MB"
      : "Tier: interpreter\nReason: closures or metamethods present";
}
function runCurrentScript() {
  const s = SCRIPTS.find(x => x.name === state.scriptCurrent);
  if (!s) return;
  s.calls++;
  const out = `> ${s.name} (${s.lang}, ${s.jit})\n[db.log] refresh complete\n[db.log] 2 rows affected\nreturn: { ok: true }\nelapsed: ${ (4 + Math.random()*8).toFixed(1) } ms`;
  $("#scrOut").textContent = out;
  $("#scrMeta").textContent = `${s.lang} · ${s.jit} · called ${s.calls}×`;
  toast("Script executed.", "ok");
}

// ---------- Admin: triggers ----------
function renderAdminTriggers() {
  const body = $("#adminTrigBody"); body.replaceChildren();
  const f = ($("#trgFilter").value || "").toLowerCase();
  for (const t of TRIGGERS) {
    if (f && !(t.name + t.table).toLowerCase().includes(f)) continue;
    body.append(el("tr", {},
      el("td", {}, el("label", { class: "switch" }, el("input", { type: "checkbox", checked: t.enabled, onchange: e => { t.enabled = e.target.checked; renderTriggersList(); renderAdminTriggers(); toast(`${t.name} ${t.enabled?"enabled":"disabled"}.`, "info"); } }), el("span"))),
      el("td", { class: "pk" }, t.name),
      el("td", {}, t.table),
      el("td", {}, t.timing),
      el("td", {}, t.event),
      el("td", {}, t.forEach),
      el("td", { class: "code", ondblclick: () => openCellModal(t.name, t.body) }, t.body.slice(0, 90) + (t.body.length>90?"…":"")),
    ));
  }
}

// ---------- Admin: WAL ----------
function renderWal() {
  const body = $("#walBody"); body.replaceChildren();
  for (const w of WAL) body.append(el("tr", {},
    el("td", { class: "code" }, w.lsn),
    el("td", { class: "code" }, w.time),
    el("td", { class: "num" }, w.xid || ""),
    el("td", {}, w.type),
    el("td", {}, w.obj || "—"),
    el("td", { class: "num" }, w.bytes),
    el("td", { class: "code" }, w.detail),
  ));
  $("#walCkpt").textContent = `Last checkpoint · ${WAL.filter(x => x.type === "CHECKPOINT").pop()?.lsn || "—"}`;
}

// ---------- Admin: transactions ----------
function renderTxns() {
  const body = $("#txnBody"); body.replaceChildren();
  for (const t of TXNS) body.append(el("tr", {},
    el("td", { class: "num pk" }, t.xid),
    el("td", {}, el("span", { class: cls("chip","chip-soft") }, t.state)),
    el("td", {}, t.iso),
    el("td", {}, t.user),
    el("td", { class: "code" }, t.started),
    el("td", { class: "num" }, t.locks),
    el("td", { class: "code", ondblclick: () => openCellModal("Last SQL", t.sql) }, t.sql.slice(0, 80)),
    el("td", {}, el("button", { class: "btn btn-soft danger", onclick: async () => {
      if (await confirmSheet("Cancel transaction?", `Terminate XID ${t.xid}? This will roll it back.`)) toast(`XID ${t.xid} terminated.`, "warn");
    } }, "Kill")),
  ));
  const lb = $("#lockBody"); lb.replaceChildren();
  for (const l of LOCKS) lb.append(el("tr", {},
    el("td", { class: "num pk" }, l.xid),
    el("td", {}, l.obj),
    el("td", {}, l.mode),
    el("td", {}, l.granted ? "YES" : el("span", { style: "color: var(--red); font-weight:600" }, "WAITING")),
  ));
}

// ---------- Admin: privileges ----------
function renderPrivs() {
  const list = $("#userList"); list.replaceChildren();
  for (const u of USERS) list.append(el("li", {
    class: cls(state.privSubject === u.name && "active"),
    onclick: () => { state.privSubject = u.name; renderPrivs(); },
  }, el("span", { class: "ico" }, u.kind === "role" ? "◆" : "●"), el("span", { class: "name" }, u.name), el("span", { class: "flag" }, u.kind)));
  const subj = state.privSubject || $("#actAsUser").value || "rick";
  state.privSubject = subj;
  $("#privSubj").textContent = subj;
  const body = $("#grantBody"); body.replaceChildren();
  const p = PRIVS[subj] || {};
  const objs = Object.keys(SCHEMA);
  for (const o of objs) {
    const row = p[o] || p["*"] || [];
    body.append(el("tr", {},
      el("td", { class: "pk" }, o),
      ...["S","I","U","D","X"].map(k => el("td", {}, el("input", {
        type: "checkbox", checked: row.includes(k),
        onchange: e => { (PRIVS[subj] = PRIVS[subj] || {}); (PRIVS[subj][o] = PRIVS[subj][o] || []);
          const s = new Set(PRIVS[subj][o]); e.target.checked ? s.add(k) : s.delete(k); PRIVS[subj][o] = [...s];
          toast(`${e.target.checked?"GRANT":"REVOKE"} ${ {S:"SELECT",I:"INSERT",U:"UPDATE",D:"DELETE",X:"EXECUTE"}[k] } ON ${o} ${e.target.checked?"TO":"FROM"} ${subj}`, "info");
        }
      }))),
      el("td", {}, el("input", { type: "checkbox", checked: ["S","I","U","D","X"].every(k => row.includes(k)),
        onchange: e => { PRIVS[subj] = PRIVS[subj] || {}; PRIVS[subj][o] = e.target.checked ? ["S","I","U","D","X"] : []; renderPrivs(); } })),
    ));
  }
}

// ---------- Admin: info_schema ----------
function renderInfo() {
  const k = $("#infoSelect").value || "tables";
  const t = INFO_TABLES[k];
  const head = $("#infoHead"); head.replaceChildren();
  const trh = el("tr"); t.cols.forEach(c => trh.append(el("th", {}, c))); head.append(trh);
  const body = $("#infoBody"); body.replaceChildren();
  for (const r of t.rows) {
    const tr = el("tr");
    r.forEach(v => tr.append(el("td", { class: "code" }, String(v))));
    body.append(tr);
  }
}

// ---------- Admin: doc playground ----------
function renderDoc() {
  if (!$("#docSrc").value) $("#docSrc").value = `{"customer":{"id":1003,"orders":[{"id":50013,"total":1280.5},{"id":50019,"total":42}]}}`;
  if (!$("#docExpr").value) $("#docExpr").value = `$..orders[?(@.total > 100)].id`;
}
function runDocExpr() {
  const mode = $("#docMode").value;
  const src = $("#docSrc").value;
  const expr = $("#docExpr").value;
  let out = "";
  try {
    if (mode === "jsonpath") {
      const data = JSON.parse(src);
      // very tiny JSONPath-ish for the demo
      if (expr.includes("orders") && expr.includes("total")) {
        const m = /total\s*>\s*([\d.]+)/.exec(expr);
        const th = m ? Number(m[1]) : 0;
        const ids = (data.customer?.orders || []).filter(o => o.total > th).map(o => o.id);
        out = JSON.stringify(ids, null, 2);
      } else out = JSON.stringify(data, null, 2);
    } else if (mode === "xpath" || mode === "xquery") {
      out = `Evaluated ${mode} against XML source:\n${expr}\n\n<result>[stub] would return matching nodes</result>`;
    } else if (mode === "graphql") {
      out = `query result for:\n${expr}\n\n{ "data": { "customer": { "id": 1003 } } }`;
    }
  } catch (e) { out = "Error: " + (e.message || e); }
  $("#docOut").textContent = out;
}

// ---------- Admin: debugger ----------
const DBG_SRC = [
  "CREATE OR REPLACE PROCEDURE promote_customer(p_id INT) AS",
  "DECLARE",
  "  v_ltv DECIMAL(12,2);",
  "  v_tier VARCHAR(16);",
  "BEGIN",
  "  SELECT lifetime_value INTO v_ltv FROM customers WHERE id = p_id;",
  "  IF v_ltv >= 10000 THEN v_tier := 'platinum';",
  "  ELSIF v_ltv >= 5000 THEN v_tier := 'gold';",
  "  ELSE v_tier := 'standard';",
  "  END IF;",
  "  UPDATE customers SET tier = v_tier WHERE id = p_id;",
  "  RAISE NOTICE 'promoted % to %', p_id, v_tier;",
  "END;",
];
function renderDbg() {
  const code = $("#dbgCode"); code.replaceChildren();
  DBG_SRC.forEach((line, i) => {
    const ln = el("div", { class: cls("ln", state.debugger.bps.has(i) && "bp", state.debugger.line === i && state.debugger.running && "cur") },
      el("span", { class: "num" }, i + 1),
      el("span", { class: "bp", onclick: () => { state.debugger.bps.has(i) ? state.debugger.bps.delete(i) : state.debugger.bps.add(i); renderDbg(); } }),
      el("span", {}, line),
    );
    code.append(ln);
  });
  const vars = $("#dbgVars"); vars.replaceChildren();
  for (const v of state.debugger.vars) vars.append(el("tr", {}, el("td", { class: "pk" }, v.n), el("td", {}, v.t), el("td", { class: "code" }, v.v)));
  const stk = $("#dbgStack"); stk.replaceChildren();
  for (const f of state.debugger.stack) stk.append(el("li", {}, f));
  const bps = $("#dbgBps"); bps.replaceChildren();
  for (const b of state.debugger.bps) bps.append(el("li", {}, `Line ${b + 1}: ${DBG_SRC[b]?.trim() ?? ""}`));
}
function dbgStart() {
  state.debugger.running = true; state.debugger.line = 0;
  state.debugger.vars = [{n:"p_id", t:"INT", v:"1004"}, {n:"v_ltv", t:"DECIMAL(12,2)", v:"NULL"}, {n:"v_tier", t:"VARCHAR(16)", v:"NULL"}];
  state.debugger.stack = ["promote_customer(p_id=1004)"];
  renderDbg(); toast("Debugger started.", "info");
}
function dbgStep() {
  if (!state.debugger.running) return;
  state.debugger.line = Math.min(DBG_SRC.length - 1, state.debugger.line + 1);
  if (state.debugger.line === 5) state.debugger.vars[1].v = "22011.20";
  if (state.debugger.line === 7) state.debugger.vars[2].v = "'platinum'";
  renderDbg();
}
function dbgCont() {
  while (state.debugger.running && state.debugger.line < DBG_SRC.length - 1) {
    dbgStep();
    if (state.debugger.bps.has(state.debugger.line)) break;
  }
}
function dbgStop() { state.debugger.running = false; state.debugger.line = 0; renderDbg(); toast("Debugger stopped.", "info"); }

// ---------- Schema compare ----------
function renderSchemaCompareSelects() { /* selects already populated in renderConnections */ }
function runSchemaCompare() {
  const body = $("#cmpBody"); body.replaceChildren();
  const diffs = [
    ["customers.metadata", "different",  "TEXT (staging) → JSON (local)"],
    ["orders.placed_at",    "different",  "TIMESTAMP (staging) → TIMESTAMPTZ (local)"],
    ["audit_log.bucket",    "only in B",  "missing in A"],
    ["products.price",      "match",      "DECIMAL(10,2)"],
    ["sessions",            "different",  "ENCRYPTED only in A"],
  ];
  for (const [o, s, d] of diffs) {
    body.append(el("tr", {},
      el("td", { class: "pk" }, o),
      el("td", {}, el("span", { class: cls("chip","chip-soft"), style: s==="match" ? "color:#1f8a3a" : s==="different" ? "color:var(--orange)" : "color:var(--red)" }, s)),
      el("td", { class: "code" }, d),
    ));
  }
  toast("Schema compared.", "ok");
}
function runMigration() {
  $("#cmpMig").hidden = false;
  $("#cmpMig").textContent =
`-- Migration A → B
ALTER TABLE customers ALTER COLUMN metadata TYPE JSON USING metadata::json;
ALTER TABLE orders    ALTER COLUMN placed_at TYPE TIMESTAMPTZ;
ALTER TABLE audit_log ADD COLUMN bucket VARCHAR(32);
ALTER TABLE sessions  ENCRYPT WITH (cipher='aes-256-gcm');`;
  toast("Migration generated.", "ok");
}

// ---------- Wizard (CREATE TABLE) ----------
function openWiz() {
  $("#wizScrim").hidden = false;
  $("#wizCols").querySelector("tbody").replaceChildren();
  addWizCol("id", "INTEGER", false, "AUTOINCREMENT", "PK");
  addWizCol("name", "VARCHAR(255)", false, "", "");
  updateWizSql();
}
function addWizCol(name="", type="VARCHAR(64)", nullable=true, def="", key="") {
  const tb = $("#wizCols").querySelector("tbody");
  const row = el("tr", {},
    el("td", {}, el("input", { value: name, oninput: updateWizSql })),
    el("td", {}, (() => { const s = el("select", { onchange: updateWizSql }); ["INTEGER","BIGINT","DECIMAL(10,2)","VARCHAR(64)","VARCHAR(255)","TEXT","DATE","TIMESTAMP","BOOL","UUID","JSON","XML","GEOMETRY"].forEach(t => s.append(el("option", { selected: t === type }, t))); return s; })()),
    el("td", {}, el("input", { type: "checkbox", checked: nullable, onchange: updateWizSql })),
    el("td", {}, el("input", { value: def, oninput: updateWizSql, placeholder: "default" })),
    el("td", {}, (() => { const s = el("select", { onchange: updateWizSql }); ["","PK","UQ","FK"].forEach(k => s.append(el("option", { selected: k === key }, k))); return s; })()),
    el("td", { class: "x" }, el("button", { onclick: () => { row.remove(); updateWizSql(); } }, "×")),
  );
  tb.append(row);
}
function updateWizSql() {
  const name = $("#wizName").value || "new_table";
  const columnar = $("#wizColumnar").checked ? " COLUMNAR" : "";
  const enc = $("#wizEnc").checked ? " ENCRYPTED" : "";
  const ts = " TABLESPACE " + ($("#wizTs").value || "main");
  const part = $("#wizPart").value;
  const cols = $$("#wizCols tbody tr").map(tr => {
    const t = tr.querySelectorAll("input,select");
    const n = t[0].value || "col"; const ty = t[1].value;
    const nul = t[2].checked ? "" : " NOT NULL";
    const d = t[3].value ? ` DEFAULT ${t[3].value}` : "";
    const k = t[4].value === "PK" ? " PRIMARY KEY" : t[4].value === "UQ" ? " UNIQUE" : "";
    return `  ${n} ${ty}${nul}${d}${k}`;
  }).join(",\n");
  const partClause = part ? `\nPARTITION BY ${part} (placed_at)` : "";
  $("#wizSql").textContent = `CREATE TABLE ${name} (\n${cols}\n)${columnar}${enc}${ts}${partClause};`;
}
function createWizTable() {
  const name = $("#wizName").value.trim();
  if (!name) { toast("Name required.", "err"); return; }
  if (SCHEMA[name]) { toast("Table exists.", "err"); return; }
  const cols = $$("#wizCols tbody tr").map(tr => {
    const t = tr.querySelectorAll("input,select");
    return { name: t[0].value, type: t[1].value, nullable: t[2].checked, default: t[3].value || null, key: t[4].value, comment: "" };
  }).filter(c => c.name);
  SCHEMA[name] = {
    icon: "▦", flags: [$("#wizColumnar").checked && "COLUMNAR", $("#wizEnc").checked && "ENCRYPTED"].filter(Boolean),
    comment: "", columns: cols, rows: [], constraints: [], fks: [], partitions: null,
    storage: { kind: $("#wizColumnar").checked ? "columnar" : "row", encrypted: $("#wizEnc").checked, tablespace: $("#wizTs").value || "main" },
  };
  $("#wizScrim").hidden = true;
  renderTables(); state.activeTable = name; renderBrowse(); updateStatus();
  toast(`Created ${name}.`, "ok");
}

// ---------- Import / Export ----------
function openImport() {
  $("#impScrim").hidden = false;
  $("#impTarget").value = state.activeTable;
  $("#impData").value = "id,email,first_name\n9000,demo@x.com,Demo";
  buildImportMap();
}
function buildImportMap() {
  const t = SCHEMA[$("#impTarget").value]; const wrap = $("#impMap"); wrap.replaceChildren();
  const data = ($("#impData").value || "").split("\n");
  const headers = (data[0] || "").split(",").map(s => s.trim()).filter(Boolean);
  for (const h of headers) {
    const s = el("select"); s.append(el("option", { value: "" }, "— skip —"));
    t.columns.forEach(c => s.append(el("option", { value: c.name, selected: c.name.toLowerCase() === h.toLowerCase() }, c.name)));
    wrap.append(el("div", { class: "map-row" }, el("span", { class: "code" }, h), el("span", {}, "→"), s));
  }
}
function runImport() {
  const target = $("#impTarget").value;
  const t = SCHEMA[target];
  const data = $("#impData").value.split("\n").filter(Boolean);
  const headers = (data.shift() || "").split(",").map(s => s.trim());
  let n = 0;
  for (const line of data) {
    const cells = line.split(",");
    const row = new Array(t.columns.length).fill(null);
    headers.forEach((h, i) => {
      const ci = t.columns.findIndex(c => c.name.toLowerCase() === h.toLowerCase());
      if (ci >= 0) row[ci] = cells[i];
    });
    t.rows.push(row); n++;
  }
  $("#impScrim").hidden = true;
  renderBrowse(); updateStatus();
  toast(`Imported ${n} rows into ${target}.`, "ok");
}
function openExport() {
  $("#expScrim").hidden = false;
  updateExportPreview();
}
function updateExportPreview() {
  const fmt = $("#expFormat").value; const scope = $("#expScope").value; const head = $("#expHeader").checked;
  const t = SCHEMA[state.activeTable];
  let rows = scope === "All rows" ? t.rows : scope === "Filtered" ? visibleRows(state.activeTable).map(x => x.r) : visibleRows(state.activeTable).slice((state.page-1)*state.settings.pageSize, state.page*state.settings.pageSize).map(x => x.r);
  let out = "";
  if (fmt === "CSV") {
    if (head) out += t.columns.map(c => c.name).join(",") + "\n";
    out += rows.map(r => r.map(v => v === null ? "" : /[,\n"]/.test(String(v)) ? `"${String(v).replace(/"/g,'""')}"` : String(v)).join(",")).join("\n");
  } else if (fmt === "JSON") {
    out = JSON.stringify(rows.map(r => Object.fromEntries(t.columns.map((c,i) => [c.name, r[i]]))), null, 2);
  } else if (fmt === "SQL INSERT") {
    out = rows.map(r => `INSERT INTO ${state.activeTable}(${t.columns.map(c=>c.name).join(", ")}) VALUES (${r.map(v => v === null ? "NULL" : typeof v === "number" ? v : `'${String(v).replace(/'/g,"''")}'`).join(", ")});`).join("\n");
  } else {
    out = "| " + t.columns.map(c=>c.name).join(" | ") + " |\n| " + t.columns.map(()=>"---").join(" | ") + " |\n";
    out += rows.map(r => "| " + r.map(v => v === null ? "" : String(v)).join(" | ") + " |").join("\n");
  }
  $("#expPreview").textContent = out;
}

// ---------- Backup / restore ----------
function runBackup() {
  const path = $("#bkPath").value; const enc = $("#bkEncrypt").checked; const c = $("#bkCompress").checked;
  toast(`Backup → ${path} (${c?"zstd":"raw"}${enc?", AES-256-GCM":""}).`, "ok");
}
function runRestore() {
  const src = $("#rsPath").value; const name = $("#rsName").value || "tdb_restore";
  if (!src) { toast("Source path required.", "err"); return; }
  CONNECTIONS.push({ id: "rs" + Date.now(), name, path: "/Users/rick/Library/TDB/" + name + ".tdb", status: "on", meta: "Restored", env: "dev", group: "local" });
  renderConnections();
  toast(`Restored ${name} from ${src}.`, "ok");
}

// ---------- Command palette ----------
const PAL_ITEMS = () => [
  ...Object.keys(SCHEMA).map(n => ({ kind: "Table", v: n, run: () => { state.activeTable = n; setView("browse"); renderTables(); renderBrowse(); updateStatus(); } })),
  ...VIEWS.map(v => ({ kind: "View",  v: v.name, run: () => { loadIntoEditor(v.def); setView("query"); } })),
  ...MVIEWS.map(v => ({ kind: "MView", v: v.name, run: () => { loadIntoEditor("REFRESH MATERIALIZED VIEW " + v.name + ";"); setView("query"); } })),
  ...SCRIPTS.map(s => ({ kind: "Script", v: s.name, run: () => { setView("admin"); setAdminPane("scripts"); state.scriptCurrent = s.name; renderScriptEditor(); } })),
  { kind: "Action", v: "New Query",                run: newQuery },
  { kind: "Action", v: "Run Query",                run: runQuery },
  { kind: "Action", v: "Format Query",             run: () => loadIntoEditor(formatSql($("#editor").value)) },
  { kind: "Action", v: "Explain Query",            run: () => { loadIntoEditor("EXPLAIN " + $("#editor").value.replace(/^\s*explain\s*/i,"")); runQuery(); } },
  { kind: "Action", v: "New Table…",               run: openWiz },
  { kind: "Action", v: "Import Data…",             run: openImport },
  { kind: "Action", v: "Export Data…",             run: openExport },
  { kind: "Action", v: "Open Connection…",         run: openSheet },
  { kind: "Action", v: "Go to ER Diagram",         run: () => setView("er") },
  { kind: "Action", v: "Go to History",            run: () => setView("history") },
  { kind: "Action", v: "Admin · Scripts",          run: () => { setView("admin"); setAdminPane("scripts"); } },
  { kind: "Action", v: "Admin · Triggers",         run: () => { setView("admin"); setAdminPane("triggers"); } },
  { kind: "Action", v: "Admin · WAL",              run: () => { setView("admin"); setAdminPane("wal"); } },
  { kind: "Action", v: "Admin · Transactions",     run: () => { setView("admin"); setAdminPane("txns"); } },
  { kind: "Action", v: "Admin · Privileges",       run: () => { setView("admin"); setAdminPane("privs"); } },
  { kind: "Action", v: "Admin · information_schema", run: () => { setView("admin"); setAdminPane("info"); renderInfo(); } },
  { kind: "Action", v: "Admin · JSON/XML Playground", run: () => { setView("admin"); setAdminPane("doc"); renderDoc(); } },
  { kind: "Action", v: "Admin · Backup/Restore",   run: () => { setView("admin"); setAdminPane("backup"); } },
  { kind: "Action", v: "Admin · Schema Compare",   run: () => { setView("admin"); setAdminPane("diag"); } },
  { kind: "Action", v: "Admin · PL/SQL Debugger",  run: () => { setView("admin"); setAdminPane("dbg"); renderDbg(); } },
  { kind: "Action", v: "Admin · Settings",         run: () => { setView("admin"); setAdminPane("settings"); } },
  { kind: "Action", v: "Toggle Sidebar",           run: () => $(".main").classList.toggle("no-sb") },
];
let palIdx = 0; let palResults = [];
function openPalette() {
  $("#paletteScrim").hidden = false;
  $("#palInput").value = ""; palIdx = 0; renderPalette("");
  setTimeout(() => $("#palInput").focus(), 10);
}
function closePalette() { $("#paletteScrim").hidden = true; }
function renderPalette(q) {
  const list = $("#palList"); list.replaceChildren();
  const items = PAL_ITEMS().filter(x => x.v.toLowerCase().includes(q.toLowerCase()));
  palResults = items.slice(0, 50); palIdx = Math.min(palIdx, Math.max(0, palResults.length - 1));
  palResults.forEach((it, i) => list.append(el("li", { class: cls(i === palIdx && "sel"), onclick: () => { it.run(); closePalette(); } },
    el("span", { class: "kind" }, it.kind), el("span", { class: "name" }, it.v),
  )));
}
function paletteMove(d) { if (!palResults.length) return; palIdx = (palIdx + d + palResults.length) % palResults.length; renderPalette($("#palInput").value); }
function paletteAccept() { const it = palResults[palIdx]; if (it) { it.run(); closePalette(); } }

// ---------- Sheets ----------
function openSheet() { $("#sheetScrim").hidden = false; }
function closeSheet() { $("#sheetScrim").hidden = true; }

// ---------- Settings ----------
function applySettings() {
  // theme
  if (state.settings.theme === "dark") document.documentElement.setAttribute("data-theme", "dark");
  else if (state.settings.theme === "light") document.documentElement.setAttribute("data-theme", "light");
  else document.documentElement.removeAttribute("data-theme");
  // accent
  document.documentElement.style.setProperty("--accent", state.settings.accent);
  document.documentElement.style.setProperty("--accent-soft", state.settings.accent + "26");
  document.documentElement.style.setProperty("--accent-ring", state.settings.accent + "73");
  setEditorFont(state.settings.font);
  // hydrate inputs
  $("#setTheme").value = state.settings.theme;
  $("#setAccent").value = state.settings.accent;
  $("#setFont").value = state.settings.font;
  $("#setTimeout").value = state.settings.timeout;
  $("#setPageSize").value = state.settings.pageSize;
  $("#setConfirmDDL").checked = state.settings.confirmDDL;
  $("#setPersist").checked = state.settings.persist;
}

// ---------- Wire-up ----------
function wire() {
  // Segmented control + browse pills + result pills
  $$(".seg").forEach(b => b.addEventListener("click", () => setView(b.dataset.view)));
  $$(".bh-tabs .pill").forEach(b => b.addEventListener("click", () => setPane(b.dataset.pane)));
  $$(".rh-tabs .pill").forEach(b => b.addEventListener("click", () => setRTab(b.dataset.rtab)));

  // Admin nav
  $$(".anv").forEach(b => b.addEventListener("click", () => setAdminPane(b.dataset.anv)));

  // Sidebar toggle + accordions
  $("#toggleSidebar").addEventListener("click", () => $(".main").classList.toggle("no-sb"));
  $$(".acc-head").forEach(h => h.addEventListener("click", (e) => {
    if (e.target.closest(".sb-add")) return;
    const acc = h.parentElement;
    acc.dataset.open = acc.dataset.open === "1" ? "0" : "1";
    h.querySelector(".caret").textContent = acc.dataset.open === "1" ? "▾" : "▸";
  }));

  // Table filter
  $("#tableFilter").addEventListener("input", () => renderTables());

  // Editor / gutter
  const ed = $("#editor");
  ed.value = state.queries[state.activeQ].sql;
  syncEditor();
  ed.addEventListener("input", () => { syncEditor(); state.queries[state.activeQ].sql = ed.value; persist(); showAutocomplete(); });
  ed.addEventListener("scroll", () => { $("#gutter").scrollTop = ed.scrollTop; $("#hl").scrollTop = ed.scrollTop; $("#hl").scrollLeft = ed.scrollLeft; });
  ed.addEventListener("blur", () => { setTimeout(() => { $("#ac").hidden = true; AC.open = false; }, 100); });
  ed.addEventListener("keydown", (e) => {
    if (AC.open && (e.key === "ArrowDown" || e.key === "ArrowUp")) { e.preventDefault(); moveAc(e.key === "ArrowDown" ? 1 : -1); return; }
    if (AC.open && (e.key === "Enter" || e.key === "Tab")) { if (acceptAutocomplete()) { e.preventDefault(); return; } }
    if (AC.open && e.key === "Escape") { $("#ac").hidden = true; AC.open = false; e.preventDefault(); return; }
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); runQuery(); }
    if (e.key === "Tab") {
      e.preventDefault();
      const s = ed.selectionStart, en = ed.selectionEnd;
      ed.value = ed.value.slice(0, s) + "  " + ed.value.slice(en);
      ed.selectionStart = ed.selectionEnd = s + 2;
      syncEditor();
    }
    // Bracket autoclose
    if ("([{'\"`".includes(e.key) && ed.selectionStart === ed.selectionEnd) {
      const close = { "(":")", "[":"]", "{":"}", "'":"'", '"':'"', "`":"`" }[e.key];
      e.preventDefault();
      const s = ed.selectionStart;
      ed.value = ed.value.slice(0, s) + e.key + close + ed.value.slice(s);
      ed.selectionStart = ed.selectionEnd = s + 1;
      syncEditor();
    }
  });

  // Header buttons
  $("#runBtn").addEventListener("click", runQuery);
  $("#runQueryBtn").addEventListener("click", runQuery);
  $("#newQueryBtn").addEventListener("click", newQuery);
  $("#explainBtn").addEventListener("click", () => { const s = ed.value.trim(); if (!s) return; loadIntoEditor(/^\s*explain/i.test(s) ? s : "EXPLAIN " + s); runQuery(); });
  $("#formatBtn").addEventListener("click", () => loadIntoEditor(formatSql(ed.value)));
  $("#diffBtn").addEventListener("click", runDiff);
  $("#saveQueryBtn").addEventListener("click", () => {
    const sql = ed.value.trim(); if (!sql) return;
    const name = "Saved " + (state.saved.length + 1);
    state.saved.push({ id: "s" + Date.now(), name, sql });
    renderSaved(); persist(); toast("Query saved.", "ok");
  });
  $("#saveCurrent").addEventListener("click", () => $("#saveQueryBtn").click());

  // Browse actions
  $("#filterBtn").addEventListener("click", () => {
    const b = $("#filterBuilder"); b.hidden = !b.hidden;
    if (!state.filters.conds.length) {
      const t = SCHEMA[state.activeTable];
      state.filters.conds.push({ col: t.columns[0].name, op: "=", val: "" });
      renderFilterBuilder();
    }
  });
  $("#fbAdd").addEventListener("click", () => {
    const t = SCHEMA[state.activeTable];
    state.filters.conds.push({ col: t.columns[0].name, op: "=", val: "" });
    renderFilterBuilder();
  });
  $("#fbClear").addEventListener("click", () => { state.filters.conds = []; renderBrowse(); });
  $("#fbApply").addEventListener("click", () => { state.page = 1; renderBrowse(); });
  $("#colsBtn").addEventListener("click", () => {
    const t = SCHEMA[state.activeTable];
    const popup = el("div", { class: "ac", style: "position:fixed; right:24px; top:80px; padding:8px; display:grid; gap:4px; z-index:50;" });
    t.columns.forEach(c => popup.append(el("label", { style: "display:flex; gap:8px; align-items:center; font-size:12px; padding:2px 6px;" },
      el("input", { type: "checkbox", checked: !state.hiddenCols.has(c.name), onchange: e => { e.target.checked ? state.hiddenCols.delete(c.name) : state.hiddenCols.add(c.name); renderBrowse(); } }), c.name)));
    popup.append(el("button", { class: "btn btn-soft", style: "margin-top:6px", onclick: () => popup.remove() }, "Close"));
    document.body.append(popup);
  });
  $("#addRowBtn").addEventListener("click", () => {
    const t = SCHEMA[state.activeTable];
    t.rows.unshift(t.columns.map(c => c.key === "PK" ? Math.floor(Math.random()*9000+9000) : c.nullable ? null : ""));
    state.page = 1; renderBrowse(); updateStatus(); toast("New row inserted (uncommitted).", "info");
  });
  $("#pgPrev").addEventListener("click", () => { state.page = Math.max(1, state.page - 1); renderBrowse(); });
  $("#pgNext").addEventListener("click", () => { state.page = state.page + 1; renderBrowse(); });
  $("#commitBtn").addEventListener("click", commitEdits);
  $("#revertBtn").addEventListener("click", revertEdits);

  // Import / Export
  $("#importBtn").addEventListener("click", openImport);
  $("#exportBtn").addEventListener("click", openExport);
  $("#impCancel").addEventListener("click", () => $("#impScrim").hidden = true);
  $("#impClose").addEventListener("click", () => $("#impScrim").hidden = true);
  $("#impTarget").addEventListener("change", buildImportMap);
  $("#impData").addEventListener("input", buildImportMap);
  $("#impRun").addEventListener("click", runImport);
  $("#expCancel").addEventListener("click", () => $("#expScrim").hidden = true);
  $("#expClose").addEventListener("click", () => $("#expScrim").hidden = true);
  $("#expFormat").addEventListener("change", updateExportPreview);
  $("#expScope").addEventListener("change", updateExportPreview);
  $("#expHeader").addEventListener("change", updateExportPreview);
  $("#expCopy").addEventListener("click", () => { navigator.clipboard?.writeText($("#expPreview").textContent); toast("Copied to clipboard.", "ok"); });

  // History
  $("#historyFilter").addEventListener("input", e => renderHistory(e.target.value));
  $("#clearHistory").addEventListener("click", async () => { if (await confirmSheet("Clear history?", "All history items will be removed.")) { state.history = []; renderHistory(); persist(); toast("History cleared.", "info"); } });
  $("#exportHistory").addEventListener("click", () => { navigator.clipboard?.writeText(state.history.map(h => h.sql).join(";\n")); toast("History copied.", "ok"); });

  // ER
  $("#erRelayout").addEventListener("click", renderER);
  $("#erFit").addEventListener("click", () => $("#erSvg").scrollIntoView());
  $("#erExport").addEventListener("click", () => { navigator.clipboard?.writeText($("#erSvg").outerHTML); toast("SVG copied to clipboard.", "ok"); });

  // Connection sheet
  $("#addConn").addEventListener("click", openSheet);
  $("#closeSheet").addEventListener("click", closeSheet);
  $("#cancelSheet").addEventListener("click", closeSheet);
  $("#sheetScrim").addEventListener("click", e => { if (e.target.id === "sheetScrim") closeSheet(); });
  $("#createConn").addEventListener("click", () => {
    const name = $("#connName").value || "tdb_new";
    const path = $("#connPath").value || ":memory:";
    CONNECTIONS.push({
      id: "c" + Date.now(), name, path, status: "on", meta: $("#connMode").value,
      env: $("#connEnv").value, group: $("#connGroup").value || "default",
    });
    renderConnections(); closeSheet(); toast(`Connected to ${name}.`, "ok");
  });

  // Wizard
  $("#newTableBtn").addEventListener("click", openWiz);
  $("#wizClose").addEventListener("click", () => $("#wizScrim").hidden = true);
  $("#wizCancel").addEventListener("click", () => $("#wizScrim").hidden = true);
  $("#wizAddCol").addEventListener("click", () => addWizCol());
  $("#wizName").addEventListener("input", updateWizSql);
  $("#wizColumnar").addEventListener("change", updateWizSql);
  $("#wizEnc").addEventListener("change", updateWizSql);
  $("#wizPart").addEventListener("change", updateWizSql);
  $("#wizTs").addEventListener("change", updateWizSql);
  $("#wizCreate").addEventListener("click", createWizTable);

  // Cell modal / confirm
  $("#cellClose").addEventListener("click", () => $("#cellScrim").hidden = true);
  $("#cellOk").addEventListener("click", () => $("#cellScrim").hidden = true);

  // Scripts
  $("#scrSelect").addEventListener("change", e => { state.scriptCurrent = e.target.value; renderScriptEditor(); });
  $("#scrLang").addEventListener("change", e => { const s = SCRIPTS.find(x => x.name === state.scriptCurrent); if (s) s.lang = e.target.value; renderScriptEditor(); });
  $("#scrSave").addEventListener("click", () => { const s = SCRIPTS.find(x => x.name === state.scriptCurrent); if (s) s.body = $("#scrEditor").value; toast("Script saved.", "ok"); });
  $("#scrNew").addEventListener("click", () => {
    const n = "script_" + (SCRIPTS.length + 1);
    SCRIPTS.push({ name: n, lang: "lua", calls: 0, jit: "Interpreter", body: "-- new script\n" });
    state.scriptCurrent = n; renderScripts(); renderScriptEditor();
  });
  $("#scrRun").addEventListener("click", runCurrentScript);
  $$(".apane[data-apane='scripts'] .pill").forEach(b => b.addEventListener("click", () => {
    $$(".apane[data-apane='scripts'] .pill").forEach(x => x.classList.toggle("active", x === b));
    const k = b.dataset.stab;
    $("#scrOut").hidden = k !== "out"; $("#scrJit").hidden = k !== "jit";
  }));

  // Triggers
  $("#trgFilter").addEventListener("input", renderAdminTriggers);
  $("#trgNew").addEventListener("click", () => toast("Trigger creator stub — would open a CREATE TRIGGER editor.", "info"));

  // WAL
  $("#walCheckpoint").addEventListener("click", () => { WAL.push({ lsn: "0/3F2E80", time: new Date().toISOString().replace("T"," ").slice(0,19), xid: 0, type: "CHECKPOINT", obj: "", bytes: 64, detail: "forced" }); renderWal(); toast("Checkpoint forced.", "ok"); });
  $("#walExport").addEventListener("click", () => { navigator.clipboard?.writeText(WAL.map(w => Object.values(w).join("\t")).join("\n")); toast("WAL copied.", "ok"); });

  // Transactions
  $("#txnRefresh").addEventListener("click", () => { renderTxns(); toast("Refreshed.", "info"); });

  // Privileges
  $("#actAsUser").addEventListener("change", e => { state.privSubject = e.target.value; renderPrivs(); toast(`Now acting as ${e.target.value}.`, "info"); });
  $("#newRole").addEventListener("click", () => { USERS.push({ name: "role_" + (USERS.length+1), kind: "role" }); renderPrivs(); });
  $("#newUser").addEventListener("click", () => { USERS.push({ name: "user_" + (USERS.length+1), kind: "user", roles: ["reader"] }); renderPrivs(); });

  // info_schema
  $("#infoSelect").addEventListener("change", renderInfo);

  // Doc playground
  $("#docRun").addEventListener("click", runDocExpr);

  // Backup
  $("#bkRun").addEventListener("click", runBackup);
  $("#rsRun").addEventListener("click", runRestore);

  // Schema compare
  $("#cmpRun").addEventListener("click", runSchemaCompare);
  $("#cmpMigrate").addEventListener("click", runMigration);

  // Debugger
  $("#dbgStart").addEventListener("click", dbgStart);
  $("#dbgStep").addEventListener("click", dbgStep);
  $("#dbgCont").addEventListener("click", dbgCont);
  $("#dbgStop").addEventListener("click", dbgStop);

  // Settings
  $("#setTheme").addEventListener("change", e => { state.settings.theme = e.target.value; applySettings(); persist(); });
  $("#setAccent").addEventListener("input", e => { state.settings.accent = e.target.value; applySettings(); persist(); });
  $("#setFont").addEventListener("input", e => { state.settings.font = +e.target.value; applySettings(); persist(); });
  $("#setTimeout").addEventListener("change", e => { state.settings.timeout = +e.target.value; persist(); });
  $("#setPageSize").addEventListener("change", e => { state.settings.pageSize = +e.target.value; renderBrowse(); persist(); });
  $("#setConfirmDDL").addEventListener("change", e => { state.settings.confirmDDL = e.target.checked; persist(); });
  $("#setPersist").addEventListener("change", e => { state.settings.persist = e.target.checked; if (e.target.checked) persist(); else localStorage.removeItem(LS_KEY); });
  $("#setNewConn").addEventListener("click", openSheet);
  $("#setReset").addEventListener("click", async () => {
    if (await confirmSheet("Reset?", "Clear all saved tabs, history, and preferences.")) {
      localStorage.removeItem(LS_KEY);
      toast("State cleared. Reload to fully reset.", "info");
    }
  });

  // Global search routes to filter
  $("#globalSearch").addEventListener("input", (e) => { $("#tableFilter").value = e.target.value; renderTables(); });
  $("#globalSearch").addEventListener("focus", openPalette);
  $("#globalSearch").addEventListener("click", openPalette);

  // Palette
  $("#palInput").addEventListener("input", e => renderPalette(e.target.value));
  $("#palInput").addEventListener("keydown", e => {
    if (e.key === "ArrowDown") { e.preventDefault(); paletteMove(1); }
    else if (e.key === "ArrowUp") { e.preventDefault(); paletteMove(-1); }
    else if (e.key === "Enter") { e.preventDefault(); paletteAccept(); }
    else if (e.key === "Escape") closePalette();
  });
  $("#paletteScrim").addEventListener("click", e => { if (e.target.id === "paletteScrim") closePalette(); });

  // Keyboard shortcuts
  window.addEventListener("keydown", e => {
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") { e.preventDefault(); openPalette(); }
    else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "n") { e.preventDefault(); newQuery(); }
    else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "f") { e.preventDefault(); openPalette(); }
    else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "s") { e.preventDefault(); if (state.view === "query") { $("#saveQueryBtn").click(); } else commitEdits(); }
    else if (e.key === "Escape") { closePalette(); closeSheet(); $("#wizScrim").hidden = true; $("#impScrim").hidden = true; $("#expScrim").hidden = true; $("#cellScrim").hidden = true; $("#confirmScrim").hidden = true; }
  });
}

// ---------- Boot ----------
function boot() {
  renderSidebar();
  renderBrowse();
  renderHistory();
  renderQueryTabs();
  updateTitlebar();
  updateStatus();
  updateEnvStrip();
  applySettings();
  wire();
  toast("Connected to tdb_local.", "ok", 1600);
}
boot();
