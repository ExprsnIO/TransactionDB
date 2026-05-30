/*
** tdb_sqltype.h — strict SQL type system.
**
** Where tdb_type.h provides loose SQLite-style "affinity", this layer provides
** a precise, strictly-enforced type model spanning the column types found
** across SQLite, MySQL, PostgreSQL, Oracle and DB2/HANA. A column declares a
** `tdb_typespec` (type id + optional length / precision / scale); on
** insert/update the engine checks a value against it and either coerces it
** losslessly or rejects it with TDB_MISMATCH.
*/
#ifndef TDB_SQLTYPE_H
#define TDB_SQLTYPE_H

#include "tdb_value.h"

typedef enum tdb_typeid {
  TDB_T_UNKNOWN = 0,
  /* boolean */
  TDB_T_BOOLEAN,
  /* exact numeric */
  TDB_T_TINYINT,
  TDB_T_SMALLINT,
  TDB_T_INTEGER,
  TDB_T_BIGINT,
  TDB_T_DECIMAL,     /* DECIMAL/NUMERIC(p,s) */
  /* approximate numeric */
  TDB_T_REAL,
  TDB_T_DOUBLE,
  /* character data */
  TDB_T_CHAR,        /* fixed length */
  TDB_T_VARCHAR,     /* bounded length */
  TDB_T_TEXT,        /* unbounded (TEXT/CLOB) */
  /* binary data */
  TDB_T_BINARY,
  TDB_T_VARBINARY,
  TDB_T_BLOB,
  /* date/time */
  TDB_T_DATE,
  TDB_T_TIME,
  TDB_T_TIMESTAMP,
  /* extended common types */
  TDB_T_JSON,
  TDB_T_UUID
} tdb_typeid;

typedef struct tdb_typespec {
  tdb_typeid id;
  int        length;     /* CHAR/VARCHAR/BINARY length, 0 = unspecified */
  int        precision;  /* DECIMAL total digits, 0 = unspecified */
  int        scale;      /* DECIMAL fractional digits */
} tdb_typespec;

/* Parse a declared SQL type, e.g. "DECIMAL(10,2)", "VARCHAR(80)", "BIGINT".
** Unknown names resolve to TDB_T_TEXT (permissive declaration, strict storage).*/
tdb_typespec tdb_typespec_parse(const char *decl);

/* Canonical name for a type id (e.g. "INTEGER"). */
const char *tdb_typeid_name(tdb_typeid id);

/* The storage class a value of this type is kept as on disk. */
tdb_valtype tdb_typespec_storage(const tdb_typespec *ts);

/*
** Strictly validate and (losslessly) coerce `v` to conform to `ts`, in place.
** Returns:
**   TDB_OK         value now conforms (possibly converted)
**   TDB_MISMATCH   value cannot be represented in this type
**   TDB_CONSTRAINT value violates a length/precision/scale bound
** NULL values always pass here (nullability is enforced by the column, not
** the type). On error, *why (if non-NULL) is set to a static description.
*/
int tdb_typespec_coerce(tdb_value *v, const tdb_typespec *ts, const char **why);

#endif /* TDB_SQLTYPE_H */
