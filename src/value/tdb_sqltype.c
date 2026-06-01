/* tdb_sqltype.c — strict SQL type parsing, classification, and coercion. */
#include "tdb_sqltype.h"
#include "tdb_geom.h"
#include "../common/tdb_buf.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ----------------------------- name table ----------------------------- */

typedef struct { const char *name; tdb_typeid id; } type_alias;

/* Longest/most-specific aliases first where prefixes overlap. */
static const type_alias k_aliases[] = {
  {"BOOLEAN", TDB_T_BOOLEAN}, {"BOOL", TDB_T_BOOLEAN},
  {"TINYINT", TDB_T_TINYINT},
  {"SMALLINT", TDB_T_SMALLINT}, {"INT2", TDB_T_SMALLINT},
  {"BIGINT", TDB_T_BIGINT}, {"INT8", TDB_T_BIGINT},
  {"INTEGER", TDB_T_INTEGER}, {"INT4", TDB_T_INTEGER}, {"INT", TDB_T_INTEGER},
  {"DECIMAL", TDB_T_DECIMAL}, {"NUMERIC", TDB_T_DECIMAL}, {"NUMBER", TDB_T_DECIMAL},
  {"DOUBLE", TDB_T_DOUBLE}, {"FLOAT8", TDB_T_DOUBLE},
  {"REAL", TDB_T_REAL}, {"FLOAT", TDB_T_REAL}, {"FLOAT4", TDB_T_REAL},
  {"VARCHAR", TDB_T_VARCHAR}, {"NVARCHAR", TDB_T_VARCHAR}, {"VARCHAR2", TDB_T_VARCHAR},
  {"CHARACTER VARYING", TDB_T_VARCHAR},
  {"NCHAR", TDB_T_CHAR}, {"CHAR", TDB_T_CHAR}, {"CHARACTER", TDB_T_CHAR},
  {"CLOB", TDB_T_TEXT}, {"TEXT", TDB_T_TEXT}, {"STRING", TDB_T_TEXT},
  {"VARBINARY", TDB_T_VARBINARY}, {"BYTEA", TDB_T_VARBINARY},
  {"BINARY", TDB_T_BINARY},
  {"BLOB", TDB_T_BLOB}, {"RAW", TDB_T_BLOB},
  {"TIMESTAMP", TDB_T_TIMESTAMP}, {"DATETIME", TDB_T_TIMESTAMP},
  {"DATE", TDB_T_DATE},
  {"TIME", TDB_T_TIME},
  {"JSON", TDB_T_JSON}, {"JSONB", TDB_T_JSON},
  {"UUID", TDB_T_UUID}, {"GUID", TDB_T_UUID},
  {"GEOMETRY", TDB_T_GEOMETRY}, {"GEOM", TDB_T_GEOMETRY},
  {"POINT", TDB_T_POINT},
  {"GEOGRAPHY", TDB_T_GEOGRAPHY}, {"GEOG", TDB_T_GEOGRAPHY},
  {"COMPOSITE", TDB_T_COMPOSITE}, {"STRUCT", TDB_T_COMPOSITE}, {"ROW", TDB_T_COMPOSITE},
};

const char *tdb_typeid_name(tdb_typeid id) {
  switch (id) {
    case TDB_T_BOOLEAN:   return "BOOLEAN";
    case TDB_T_TINYINT:   return "TINYINT";
    case TDB_T_SMALLINT:  return "SMALLINT";
    case TDB_T_INTEGER:   return "INTEGER";
    case TDB_T_BIGINT:    return "BIGINT";
    case TDB_T_DECIMAL:   return "DECIMAL";
    case TDB_T_REAL:      return "REAL";
    case TDB_T_DOUBLE:    return "DOUBLE";
    case TDB_T_CHAR:      return "CHAR";
    case TDB_T_VARCHAR:   return "VARCHAR";
    case TDB_T_TEXT:      return "TEXT";
    case TDB_T_BINARY:    return "BINARY";
    case TDB_T_VARBINARY: return "VARBINARY";
    case TDB_T_BLOB:      return "BLOB";
    case TDB_T_DATE:      return "DATE";
    case TDB_T_TIME:      return "TIME";
    case TDB_T_TIMESTAMP: return "TIMESTAMP";
    case TDB_T_JSON:      return "JSON";
    case TDB_T_UUID:      return "UUID";
    case TDB_T_GEOMETRY:  return "GEOMETRY";
    case TDB_T_POINT:     return "POINT";
    case TDB_T_GEOGRAPHY: return "GEOGRAPHY";
    case TDB_T_COMPOSITE: return "COMPOSITE";
    default:              return "UNKNOWN";
  }
}

static int starts_with_ci(const char *s, const char *prefix) {
  return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

tdb_typespec tdb_typespec_parse(const char *decl) {
  tdb_typespec ts;
  ts.id = TDB_T_TEXT;
  ts.length = ts.precision = ts.scale = 0;
  if (!decl) return ts;

  while (*decl && isspace((unsigned char)*decl)) decl++;

  for (size_t i = 0; i < sizeof(k_aliases) / sizeof(k_aliases[0]); i++) {
    if (starts_with_ci(decl, k_aliases[i].name)) {
      ts.id = k_aliases[i].id;
      break;
    }
  }

  /* parse optional ( N ) or ( P , S ) */
  const char *lp = strchr(decl, '(');
  if (lp) {
    int a = 0, b = 0;
    int got = sscanf(lp, "(%d,%d", &a, &b);
    if (got >= 1) {
      if (ts.id == TDB_T_DECIMAL) {
        ts.precision = a;
        ts.scale = (got >= 2) ? b : 0;
      } else {
        ts.length = a;
      }
    }
  }
  return ts;
}

tdb_valtype tdb_typespec_storage(const tdb_typespec *ts) {
  switch (ts->id) {
    case TDB_T_BOOLEAN:
    case TDB_T_TINYINT:
    case TDB_T_SMALLINT:
    case TDB_T_INTEGER:
    case TDB_T_BIGINT:
      return TDB_VAL_INT;
    case TDB_T_REAL:
    case TDB_T_DOUBLE:
      return TDB_VAL_REAL;
    case TDB_T_BINARY:
    case TDB_T_VARBINARY:
    case TDB_T_BLOB:
    case TDB_T_GEOMETRY:
    case TDB_T_POINT:
    case TDB_T_GEOGRAPHY:
    case TDB_T_COMPOSITE:
      return TDB_VAL_BLOB;
    default:
      /* DECIMAL, CHAR/VARCHAR/TEXT, DATE/TIME/TIMESTAMP, JSON, UUID
      ** are stored as TEXT for exact representation. */
      return TDB_VAL_TEXT;
  }
}

/* full integer parse ("123", "-9") */
static int parse_int_exact(const char *s, int64_t *out) {
  if (!s || !*s) return 0;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  while (end && *end && isspace((unsigned char)*end)) end++;
  if (end && *end == '\0') { *out = (int64_t)v; return 1; }
  return 0;
}

static int parse_real_exact(const char *s, double *out) {
  if (!s || !*s) return 0;
  char *end = NULL;
  double v = strtod(s, &end);
  while (end && *end && isspace((unsigned char)*end)) end++;
  if (end && *end == '\0') { *out = v; return 1; }
  return 0;
}

static int int_in_range(int64_t v, int64_t lo, int64_t hi, const char **why) {
  if (v < lo || v > hi) { if (why) *why = "integer out of range for type"; return 0; }
  return 1;
}

/* Count significant digits and fractional digits of a decimal text. */
static void decimal_digits(const char *s, int *digits, int *frac) {
  *digits = 0; *frac = 0;
  int seen_dot = 0, started = 0;
  for (; *s; s++) {
    if (*s == '-' || *s == '+') continue;
    if (*s == '.') { seen_dot = 1; continue; }
    if (isdigit((unsigned char)*s)) {
      if (!started && *s == '0' && !seen_dot) continue; /* skip leading zeros */
      started = 1;
      (*digits)++;
      if (seen_dot) (*frac)++;
    }
  }
  if (*digits == 0) *digits = 1; /* the value 0 */
}

int tdb_typespec_coerce(tdb_value *v, const tdb_typespec *ts, const char **why) {
  if (why) *why = NULL;
  if (v->type == TDB_VAL_NULL) return TDB_OK; /* nullability handled elsewhere */

  switch (ts->id) {
    case TDB_T_BOOLEAN: {
      int64_t iv;
      if (v->type == TDB_VAL_INT) iv = v->u.i;
      else if (v->type == TDB_VAL_TEXT) {
        const char *s = v->u.s.p;
        if (!strcasecmp(s, "true") || !strcmp(s, "1")) iv = 1;
        else if (!strcasecmp(s, "false") || !strcmp(s, "0")) iv = 0;
        else { if (why) *why = "not a boolean"; return TDB_MISMATCH; }
      } else { if (why) *why = "not a boolean"; return TDB_MISMATCH; }
      tdb_value_set_int(v, iv ? 1 : 0);
      return TDB_OK;
    }
    case TDB_T_TINYINT:
    case TDB_T_SMALLINT:
    case TDB_T_INTEGER:
    case TDB_T_BIGINT: {
      int64_t iv = 0;
      if (v->type == TDB_VAL_INT) iv = v->u.i;
      else if (v->type == TDB_VAL_REAL) {
        if ((double)(int64_t)v->u.r != v->u.r) { if (why) *why = "real has fractional part"; return TDB_MISMATCH; }
        iv = (int64_t)v->u.r;
      } else if (v->type == TDB_VAL_TEXT) {
        if (!parse_int_exact(v->u.s.p, &iv)) { if (why) *why = "not an integer"; return TDB_MISMATCH; }
      } else { if (why) *why = "not an integer"; return TDB_MISMATCH; }

      int ok = 1;
      if (ts->id == TDB_T_TINYINT) ok = int_in_range(iv, -128, 127, why);
      else if (ts->id == TDB_T_SMALLINT) ok = int_in_range(iv, -32768, 32767, why);
      else if (ts->id == TDB_T_INTEGER) ok = int_in_range(iv, -2147483648LL, 2147483647LL, why);
      if (!ok) return TDB_CONSTRAINT;
      tdb_value_set_int(v, iv);
      return TDB_OK;
    }
    case TDB_T_REAL:
    case TDB_T_DOUBLE: {
      double rv = 0;
      if (v->type == TDB_VAL_REAL) rv = v->u.r;
      else if (v->type == TDB_VAL_INT) rv = (double)v->u.i;
      else if (v->type == TDB_VAL_TEXT) {
        if (!parse_real_exact(v->u.s.p, &rv)) { if (why) *why = "not a number"; return TDB_MISMATCH; }
      } else { if (why) *why = "not a number"; return TDB_MISMATCH; }
      tdb_value_set_real(v, rv);
      return TDB_OK;
    }
    case TDB_T_DECIMAL: {
      /* stored as canonical text; validate it is numeric and within p/s */
      double rv;
      if (v->type != TDB_VAL_INT && v->type != TDB_VAL_REAL) {
        if (v->type != TDB_VAL_TEXT || !parse_real_exact(v->u.s.p, &rv)) {
          if (why) *why = "not a numeric value";
          return TDB_MISMATCH;
        }
      }
      const char *txt = tdb_value_as_text(v); /* materialize */
      if (ts->precision > 0) {
        int digits = 0, frac = 0;
        decimal_digits(txt, &digits, &frac);
        if (frac > ts->scale) { if (why) *why = "too many fractional digits"; return TDB_CONSTRAINT; }
        if (digits > ts->precision) { if (why) *why = "exceeds numeric precision"; return TDB_CONSTRAINT; }
      }
      /* ensure stored as TEXT */
      if (v->type != TDB_VAL_TEXT) tdb_value_set_text(v, txt, -1, 1);
      return TDB_OK;
    }
    case TDB_T_CHAR:
    case TDB_T_VARCHAR:
    case TDB_T_TEXT:
    case TDB_T_JSON: {
      if (v->type != TDB_VAL_TEXT) {
        const char *t = tdb_value_as_text(v);
        tdb_value_set_text(v, t ? t : "", -1, 1);
      }
      if (ts->length > 0 && v->u.s.n > ts->length) {
        if (why) *why = "string longer than declared length";
        return TDB_CONSTRAINT;
      }
      return TDB_OK;
    }
    case TDB_T_UUID: {
      if (v->type == TDB_VAL_TEXT) {
        if (v->u.s.n != 36) { if (why) *why = "UUID must be 36 chars"; return TDB_CONSTRAINT; }
        return TDB_OK;
      }
      if (v->type == TDB_VAL_BLOB) {
        if (v->u.s.n != 16) { if (why) *why = "UUID blob must be 16 bytes"; return TDB_CONSTRAINT; }
        return TDB_OK;
      }
      if (why) *why = "not a UUID";
      return TDB_MISMATCH;
    }
    case TDB_T_COMPOSITE: {
      /* Already a composite: accept. A blob (e.g. just decoded from storage)
      ** carries the composite's record bytes, so re-tag it in place. */
      if (v->type == TDB_VAL_COMPOSITE) return TDB_OK;
      if (v->type == TDB_VAL_BLOB) { v->type = TDB_VAL_COMPOSITE; return TDB_OK; }
      if (why) *why = "not a composite value";
      return TDB_MISMATCH;
    }
    case TDB_T_BINARY:
    case TDB_T_VARBINARY:
    case TDB_T_BLOB: {
      if (v->type != TDB_VAL_BLOB) { if (why) *why = "not binary data"; return TDB_MISMATCH; }
      if (ts->length > 0 && v->u.s.n > ts->length) {
        if (why) *why = "binary longer than declared length";
        return TDB_CONSTRAINT;
      }
      return TDB_OK;
    }
    case TDB_T_DATE:
    case TDB_T_TIME:
    case TDB_T_TIMESTAMP: {
      /* Accept ISO-8601 text or an integer (epoch); store as text. */
      if (v->type == TDB_VAL_INT) return TDB_OK; /* epoch seconds */
      if (v->type == TDB_VAL_TEXT) {
        /* lightweight shape check: must contain a digit and only legal chars */
        const char *s = v->u.s.p; int has_digit = 0;
        for (; *s; s++) {
          if (isdigit((unsigned char)*s)) has_digit = 1;
          else if (!strchr("-:T.+Z /", *s)) { if (why) *why = "malformed date/time"; return TDB_MISMATCH; }
        }
        if (!has_digit) { if (why) *why = "malformed date/time"; return TDB_MISMATCH; }
        return TDB_OK;
      }
      if (why) *why = "not a date/time";
      return TDB_MISMATCH;
    }
    case TDB_T_GEOMETRY:
    case TDB_T_POINT:
    case TDB_T_GEOGRAPHY: {
      /* Accept WKT text (parse to binary) or an already-encoded geometry blob. */
      if (v->type == TDB_VAL_TEXT) {
        tdb_buf b; tdb_buf_init(&b);
        const char *gw = NULL;
        if (tdb_geom_from_wkt(v->u.s.p, &b, &gw) != TDB_OK) {
          tdb_buf_free(&b);
          if (why) *why = gw ? gw : "invalid geometry WKT";
          return TDB_MISMATCH;
        }
        if (ts->id == TDB_T_POINT && tdb_geom_type_of(b.data, (int)b.len) != TDB_GEOM_POINT) {
          tdb_buf_free(&b);
          if (why) *why = "expected a POINT geometry";
          return TDB_CONSTRAINT;
        }
        tdb_value_set_blob(v, b.data, (int)b.len, 1);
        tdb_buf_free(&b);
        return TDB_OK;
      }
      if (v->type == TDB_VAL_BLOB) {
        if (!tdb_geom_valid(v->u.s.p ? (const uint8_t *)v->u.s.p : (const uint8_t *)"", (int)v->u.s.n)) {
          if (why) *why = "invalid geometry blob";
          return TDB_MISMATCH;
        }
        return TDB_OK;
      }
      if (why) *why = "not a geometry";
      return TDB_MISMATCH;
    }
    default:
      return TDB_OK;
  }
}
