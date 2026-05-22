#ifndef TDB_TYPES_H
#define TDB_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Page and block identifiers */
typedef uint64_t tdb_page_id_t;
typedef uint64_t tdb_txn_id_t;
typedef uint64_t tdb_lsn_t;       /* Log Sequence Number */
typedef uint32_t tdb_slot_t;
typedef uint16_t tdb_col_id_t;

/* Row identifier: page + slot */
typedef struct {
    tdb_page_id_t page_id;
    tdb_slot_t    slot;
} tdb_rid_t;

/* Status codes */
typedef enum {
    TDB_OK = 0,
    TDB_ERR_NOMEM,
    TDB_ERR_IO,
    TDB_ERR_CORRUPT,
    TDB_ERR_FULL,
    TDB_ERR_NOT_FOUND,
    TDB_ERR_EXISTS,
    TDB_ERR_TYPE_MISMATCH,
    TDB_ERR_CONSTRAINT,
    TDB_ERR_SYNTAX,
    TDB_ERR_TXN_ABORT,
    TDB_ERR_TXN_CONFLICT,
    TDB_ERR_LOCK_TIMEOUT,
    TDB_ERR_DEADLOCK,
    TDB_ERR_CRYPTO,
    TDB_ERR_INVALID_ARG,
    TDB_ERR_UNSUPPORTED,
    TDB_ERR_INTERNAL,
} tdb_status_t;

/* SQL data types — complete SQL:2003+ coverage */
typedef enum {
    TDB_TYPE_NULL = 0,

    /* Boolean (SQL:1999) */
    TDB_TYPE_BOOL,

    /* Exact numeric (SQL-92, SQL:2003) */
    TDB_TYPE_INT8,        /* TINYINT (non-standard, widely supported) */
    TDB_TYPE_INT16,       /* SMALLINT */
    TDB_TYPE_INT32,       /* INTEGER / INT */
    TDB_TYPE_INT64,       /* BIGINT */
    TDB_TYPE_DECIMAL,     /* DECIMAL(p,s) / NUMERIC(p,s) */
    TDB_TYPE_MONEY,       /* MONEY (non-standard) */

    /* Approximate numeric (SQL-92) */
    TDB_TYPE_FLOAT32,     /* REAL / FLOAT(1-24) */
    TDB_TYPE_FLOAT64,     /* DOUBLE PRECISION / FLOAT(25-53) */

    /* Character string (SQL-92) */
    TDB_TYPE_CHAR,        /* CHAR(n) / CHARACTER(n) */
    TDB_TYPE_VARCHAR,     /* VARCHAR(n) / CHARACTER VARYING(n) */
    TDB_TYPE_TEXT,        /* TEXT (non-standard, widely supported) */

    /* National character (SQL-92) */
    TDB_TYPE_NCHAR,       /* NATIONAL CHAR(n) / NCHAR(n) */
    TDB_TYPE_NVARCHAR,    /* NATIONAL CHARACTER VARYING / NVARCHAR(n) */

    /* Binary string (SQL:2003) */
    TDB_TYPE_BINARY,      /* BINARY(n) */
    TDB_TYPE_VARBINARY,   /* VARBINARY(n) / BINARY VARYING(n) */
    TDB_TYPE_BLOB,        /* BLOB / BINARY LARGE OBJECT */

    /* Character LOB (SQL-92/SQL:2003) */
    TDB_TYPE_CLOB,        /* CLOB / CHARACTER LARGE OBJECT */
    TDB_TYPE_NCLOB,       /* NCLOB / NATIONAL CHARACTER LARGE OBJECT */

    /* Bit string (SQL-92, deprecated SQL:2003) */
    TDB_TYPE_BIT,         /* BIT(n) */
    TDB_TYPE_BIT_VARYING, /* BIT VARYING(n) */

    /* Date/Time (SQL-92) */
    TDB_TYPE_DATE,        /* DATE */
    TDB_TYPE_TIME,        /* TIME / TIME WITHOUT TIME ZONE */
    TDB_TYPE_TIMETZ,      /* TIME WITH TIME ZONE */
    TDB_TYPE_TIMESTAMP,   /* TIMESTAMP / TIMESTAMP WITHOUT TIME ZONE */
    TDB_TYPE_TIMESTAMPTZ, /* TIMESTAMP WITH TIME ZONE */
    TDB_TYPE_INTERVAL,    /* INTERVAL */

    /* UUID (non-standard, widely supported) */
    TDB_TYPE_UUID,

    /* Enumeration (non-standard) */
    TDB_TYPE_ENUM,

    /* Collection types (SQL:1999, SQL:2003) */
    TDB_TYPE_ARRAY,       /* ARRAY */
    TDB_TYPE_MULTISET,    /* MULTISET */
    TDB_TYPE_ROW,         /* ROW (composite) */

    /* Document types */
    TDB_TYPE_JSON,        /* JSON (SQL:2016) */
    TDB_TYPE_JSONB,       /* JSONB (non-standard binary JSON) */
    TDB_TYPE_XML,         /* XML (SQL:2003) */

    /* Spatial types (OGC/ISO SQL/MM) */
    TDB_TYPE_GEOMETRY,    /* GEOMETRY */
    TDB_TYPE_GEOGRAPHY,   /* GEOGRAPHY */
    TDB_TYPE_RASTER,      /* RASTER (PostGIS extension) */
} tdb_type_t;

/* MVCC row version info */
typedef struct {
    tdb_txn_id_t xmin;       /* creating transaction */
    tdb_txn_id_t xmax;       /* deleting transaction (0 = live) */
    tdb_lsn_t    lsn;        /* last modification LSN */
    uint32_t     version;    /* row version counter */
} tdb_row_version_t;

#ifdef __cplusplus
}
#endif

#endif /* TDB_TYPES_H */
