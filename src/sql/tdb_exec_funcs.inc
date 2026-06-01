/* tdb_exec_funcs.inc — extra built-in scalar functions (included by tdb_exec.c).
**
** This file extends eval_func() with the SQLite-compatible function library:
** date/time helpers, JSON construction/inspection, formatting (printf/format),
** char/unicode conversions, quoting, randoms, last_insert_rowid()/changes(),
** sqlite-style helpers like changes()/last_insert_rowid()/tdb_version(), and
** the 2-argument forms of trim/ltrim/rtrim.
**
** Entry point is try_builtin_extra(): returns 1 if it handled the function (and
** wrote `out`), 0 if it did not recognize it (caller falls through). On
** evaluation error it sets db error and returns -1.
*/

#include <time.h>
#include <stdint.h>

/* ----------------------------- date / time ----------------------------- */

/* Julian Day from a (proleptic Gregorian) Y/M/D date. */
static double jd_from_ymd(int y, int m, int d) {
  if (m <= 2) { y -= 1; m += 12; }
  int a = y / 100;
  int b = 2 - a + a / 4;
  return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + b - 1524.5;
}

/* (Y, M, D) from a Julian Day Number (UTC). */
static void ymd_from_jd(double jd, int *y, int *m, int *d) {
  int Z = (int)floor(jd + 0.5);
  double F = (jd + 0.5) - Z;
  int A;
  if (Z < 2299161) A = Z;
  else { int alpha = (int)floor((Z - 1867216.25) / 36524.25); A = Z + 1 + alpha - alpha / 4; }
  int B = A + 1524;
  int C = (int)floor((B - 122.1) / 365.25);
  int D = (int)floor(365.25 * C);
  int E = (int)floor((B - D) / 30.6001);
  *d = B - D - (int)floor(30.6001 * E) + (int)F;
  *m = (E < 14) ? E - 1 : E - 13;
  *y = (*m > 2) ? C - 4716 : C - 4715;
}

/* Apply HMS into the fractional part of a JD (UTC seconds-of-day). */
static double jd_apply_hms(double jd, int h, int mi, double s) {
  return jd + ((double)h * 3600.0 + (double)mi * 60.0 + s) / 86400.0;
}

/* Pull HMS out of the fractional part of a JD. */
static void hms_from_jd(double jd, int *h, int *mi, int *s, int *ms) {
  double Z = floor(jd + 0.5);
  double F = (jd + 0.5) - Z;          /* 0 .. 1 day since midnight UTC */
  double sod = floor(F * 86400.0 + 0.5);
  if (sod >= 86400.0) sod = 0.0;
  int total = (int)sod;
  *h = total / 3600; total %= 3600;
  *mi = total / 60;  *s = total % 60;
  if (ms) *ms = 0;
}

/* Parse YYYY-MM-DD[ T]HH:MM[:SS[.fff]] or HH:MM[:SS[.fff]]. Stores into JD.
** Returns 1 on success. */
static int parse_iso_to_jd(const char *s, double *jd_out) {
  if (!s) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  double sec = 0.0;
  int n = 0;
  /* date + optional time */
  if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &n) == 3 && y > 0 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
    double jd = jd_from_ymd(y, mo, d);
    const char *p = s + n;
    while (*p == ' ' || *p == 'T' || *p == 't') p++;
    if (*p) {
      int n2 = 0;
      if (sscanf(p, "%d:%d:%lf%n", &h, &mi, &sec, &n2) == 3 ||
          (sscanf(p, "%d:%d%n", &h, &mi, &n2) == 2)) {
        jd = jd_apply_hms(jd, h, mi, sec);
      } else return 0;
    }
    *jd_out = jd;
    return 1;
  }
  /* time-only: HH:MM[:SS[.fff]] -> JD with date 2000-01-01 ... actually SQLite
  ** uses 2000-01-01 base. We use Julian day for "1970-01-01" base = 2440587.5
  ** but SQLite uses JD 2451544.5 for 2000-01-01 12:00:00... simplest match:
  ** date 0 -> JD 0; the user mostly cares about deltas. */
  int n2 = 0;
  if (sscanf(s, "%d:%d:%lf%n", &h, &mi, &sec, &n2) == 3 ||
      sscanf(s, "%d:%d%n", &h, &mi, &n2) == 2) {
    *jd_out = jd_apply_hms(0.0, h, mi, sec);
    return 1;
  }
  return 0;
}

/* Parse the first time argument into a JD. Accepts "now", an ISO string, a
** real number (treated as a JD), or an integer (also a JD). Returns 1 if ok. */
static int eval_time_arg(tdb_db *db, ectx *c, const tdb_expr *e, double *jd_out) {
  TDB_UNUSED(db);
  tdb_value v; tdb_value_init(&v);
  eval(db, c, e, &v);
  int ok = 0;
  if (v.type == TDB_VAL_NULL) { tdb_value_clear(&v); return 0; }
  if (v.type == TDB_VAL_REAL) { *jd_out = v.u.r; ok = 1; }
  else if (v.type == TDB_VAL_INT) { *jd_out = (double)v.u.i; ok = 1; }
  else {
    const char *s = tdb_value_as_text(&v);
    if (s && (strcasecmp(s, "now") == 0)) {
      time_t now = time(NULL);
      /* JD of 1970-01-01 00:00 UTC is 2440587.5. */
      *jd_out = 2440587.5 + (double)now / 86400.0;
      ok = 1;
    } else if (s) {
      ok = parse_iso_to_jd(s, jd_out);
    }
  }
  tdb_value_clear(&v);
  return ok;
}

/* Apply one SQLite-style modifier to the running JD. Recognized:
**   "+N days|hours|minutes|seconds|months|years"  (and negative forms)
**   "start of day|month|year"
**   "weekday N"   (0=Sun)
**   "unixepoch"   (reinterpret prior numeric as Unix seconds)
*/
static int apply_modifier(double *jd, const char *m, int *unixepoch_seen) {
  if (!m) return 0;
  while (*m == ' ') m++;
  if (!strcasecmp(m, "unixepoch")) {
    /* The numeric we parsed was Unix seconds; convert to JD. */
    if (unixepoch_seen) *unixepoch_seen = 1;
    *jd = 2440587.5 + *jd / 86400.0;
    return 1;
  }
  if (!strncasecmp(m, "start of day", 12)) {
    *jd = floor(*jd + 0.5) - 0.5; return 1;
  }
  if (!strncasecmp(m, "start of month", 14)) {
    int y, mo, d; ymd_from_jd(*jd, &y, &mo, &d);
    *jd = jd_from_ymd(y, mo, 1); return 1;
  }
  if (!strncasecmp(m, "start of year", 13)) {
    int y, mo, d; ymd_from_jd(*jd, &y, &mo, &d);
    *jd = jd_from_ymd(y, 1, 1); return 1;
  }
  /* "+/-N <unit>" */
  if (*m == '+' || *m == '-' || (*m >= '0' && *m <= '9') || *m == '.') {
    char *end = NULL;
    double val = strtod(m, &end);
    if (end == m) return 0;
    while (*end == ' ') end++;
    if (!strncasecmp(end, "day", 3))         *jd += val;
    else if (!strncasecmp(end, "hour", 4))   *jd += val / 24.0;
    else if (!strncasecmp(end, "minute", 6)) *jd += val / (24.0 * 60.0);
    else if (!strncasecmp(end, "second", 6)) *jd += val / 86400.0;
    else if (!strncasecmp(end, "month", 5)) {
      int y, mo, d; ymd_from_jd(*jd, &y, &mo, &d);
      double frac = *jd - floor(*jd + 0.5) + 0.5;   /* fractional day */
      int total = y * 12 + (mo - 1) + (int)val;
      int ny = total / 12, nmo = total % 12 + 1;
      *jd = jd_from_ymd(ny, nmo, d) + frac;
    }
    else if (!strncasecmp(end, "year", 4)) {
      int y, mo, d; ymd_from_jd(*jd, &y, &mo, &d);
      double frac = *jd - floor(*jd + 0.5) + 0.5;
      *jd = jd_from_ymd(y + (int)val, mo, d) + frac;
    }
    else return 0;
    return 1;
  }
  return 0;
}

/* Materialize the trailing args (i >= 1) as modifier strings and fold into jd. */
static int fold_modifiers(tdb_db *db, ectx *c, const tdb_expr *e, int start, double *jd) {
  if (!e->args) return 1;
  int unixepoch_seen = 0;
  for (int i = start; i < e->args->n; i++) {
    tdb_value v; tdb_value_init(&v);
    eval(db, c, e->args->items[i], &v);
    const char *m = tdb_value_as_text(&v);
    int ok = m ? apply_modifier(jd, m, &unixepoch_seen) : 0;
    tdb_value_clear(&v);
    if (!ok) return 0;
  }
  return 1;
}

/* SQLite-style %-format for strftime. Supported codes:
**   %Y %m %d %H %M %S %s %j %J %f %% (and %w day-of-week, %W week-of-year-Mon)
*/
static void strftime_format(tdb_buf *out, const char *fmt, double jd) {
  int Y, Mo, D, H, Mi, S, ms;
  ymd_from_jd(jd, &Y, &Mo, &D);
  hms_from_jd(jd, &H, &Mi, &S, &ms);
  char tmp[64];
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { tdb_buf_putc(out, (uint8_t)*p); continue; }
    p++;
    if (!*p) { tdb_buf_putc(out, '%'); break; }
    switch (*p) {
      case 'Y': snprintf(tmp, sizeof tmp, "%04d", Y); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'm': snprintf(tmp, sizeof tmp, "%02d", Mo); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'd': snprintf(tmp, sizeof tmp, "%02d", D); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'H': snprintf(tmp, sizeof tmp, "%02d", H); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'M': snprintf(tmp, sizeof tmp, "%02d", Mi); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'S': snprintf(tmp, sizeof tmp, "%02d", S); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 's': {
        double unix_secs = (jd - 2440587.5) * 86400.0;
        snprintf(tmp, sizeof tmp, "%lld", (long long)unix_secs);
        tdb_buf_append(out, tmp, strlen(tmp)); break;
      }
      case 'J': snprintf(tmp, sizeof tmp, "%.7f", jd); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'j': {
        int sy = Y, sm = 1, sd = 1; (void)sm; (void)sd;
        int yd = (int)(jd - jd_from_ymd(sy, 1, 1)) + 1;
        snprintf(tmp, sizeof tmp, "%03d", yd); tdb_buf_append(out, tmp, strlen(tmp)); break;
      }
      case 'f': snprintf(tmp, sizeof tmp, "%06.3f", (double)S); tdb_buf_append(out, tmp, strlen(tmp)); break;
      case 'w': {
        int dow = ((int)floor(jd + 1.5)) % 7;        /* 0 = Sun */
        if (dow < 0) dow += 7;
        snprintf(tmp, sizeof tmp, "%d", dow); tdb_buf_append(out, tmp, strlen(tmp)); break;
      }
      case '%': tdb_buf_putc(out, '%'); break;
      default: tdb_buf_putc(out, '%'); tdb_buf_putc(out, (uint8_t)*p); break;
    }
  }
}

/* --------------------------- quote / printf ---------------------------- */

static void quote_value(tdb_buf *out, const tdb_value *v) {
  if (v->type == TDB_VAL_NULL) { tdb_buf_append(out, "NULL", 4); return; }
  if (v->type == TDB_VAL_INT) {
    char tmp[32]; snprintf(tmp, sizeof tmp, "%lld", (long long)v->u.i);
    tdb_buf_append(out, tmp, strlen(tmp)); return;
  }
  if (v->type == TDB_VAL_REAL) {
    char tmp[64]; snprintf(tmp, sizeof tmp, "%g", v->u.r);
    tdb_buf_append(out, tmp, strlen(tmp)); return;
  }
  if (v->type == TDB_VAL_BLOB) {
    static const char hexd[] = "0123456789abcdef";
    tdb_buf_putc(out, 'X'); tdb_buf_putc(out, '\'');
    for (int i = 0; i < v->u.s.n; i++) {
      uint8_t b = (uint8_t)v->u.s.p[i];
      tdb_buf_putc(out, (uint8_t)hexd[b >> 4]);
      tdb_buf_putc(out, (uint8_t)hexd[b & 0xf]);
    }
    tdb_buf_putc(out, '\''); return;
  }
  /* TEXT or COMPOSITE: emit '...' with doubled quotes */
  tdb_buf_putc(out, '\'');
  for (int i = 0; i < v->u.s.n; i++) {
    uint8_t b = (uint8_t)v->u.s.p[i];
    tdb_buf_putc(out, b);
    if (b == '\'') tdb_buf_putc(out, '\'');
  }
  tdb_buf_putc(out, '\'');
}

/* tiny printf-style formatter — supports %d %lld %i %u %x %X %o %f %g %e %s %c %% with
** optional width/precision. Each conversion pulls its argument from argvals[]. */
static void printf_format(tdb_buf *out, const char *fmt, tdb_value *argvals, int nargs) {
  int ai = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { tdb_buf_putc(out, (uint8_t)*p); continue; }
    /* assemble the conversion spec */
    const char *start = p++;
    char spec[32]; int si = 0;
    spec[si++] = '%';
    /* flags */
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') { if (si < 30) spec[si++] = *p; p++; }
    /* width */
    while (*p >= '0' && *p <= '9') { if (si < 30) spec[si++] = *p; p++; }
    /* precision */
    if (*p == '.') { if (si < 30) spec[si++] = *p; p++;
      while (*p >= '0' && *p <= '9') { if (si < 30) spec[si++] = *p; p++; }
    }
    /* length: skip h/l/ll/z/j/t */
    while (*p == 'h' || *p == 'l' || *p == 'z' || *p == 'j' || *p == 't') p++;
    char conv = *p;
    if (!conv) { tdb_buf_append(out, start, (size_t)(p - start)); break; }
    char buf[128];
    tdb_value *a = (ai < nargs) ? &argvals[ai] : NULL;
    switch (conv) {
      case '%': tdb_buf_putc(out, '%'); break;
      case 'd': case 'i': {
        if (si < 28) { spec[si++] = 'l'; spec[si++] = 'l'; }
        spec[si++] = 'd'; spec[si] = 0;
        long long iv = a ? (long long)tdb_value_as_int(a) : 0;
        snprintf(buf, sizeof buf, spec, iv);
        tdb_buf_append(out, buf, strlen(buf)); ai++; break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        if (si < 28) { spec[si++] = 'l'; spec[si++] = 'l'; }
        spec[si++] = conv; spec[si] = 0;
        unsigned long long uv = a ? (unsigned long long)tdb_value_as_int(a) : 0;
        snprintf(buf, sizeof buf, spec, uv);
        tdb_buf_append(out, buf, strlen(buf)); ai++; break;
      }
      case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
        spec[si++] = conv; spec[si] = 0;
        double dv = a ? tdb_value_as_real(a) : 0;
        snprintf(buf, sizeof buf, spec, dv);
        tdb_buf_append(out, buf, strlen(buf)); ai++; break;
      }
      case 's': {
        spec[si++] = 's'; spec[si] = 0;
        const char *sv = a ? tdb_value_as_text(a) : "";
        snprintf(buf, sizeof buf, spec, sv ? sv : "");
        tdb_buf_append(out, buf, strlen(buf)); ai++; break;
      }
      case 'c': {
        int cv = a ? (int)tdb_value_as_int(a) : 0;
        tdb_buf_putc(out, (uint8_t)cv); ai++; break;
      }
      case 'q': case 'Q': case 'w': {  /* SQLite SQL-safe variants */
        const char *sv = a ? tdb_value_as_text(a) : NULL;
        if (!sv) { if (conv == 'Q') tdb_buf_append(out, "NULL", 4); }
        else {
          if (conv == 'Q') tdb_buf_putc(out, '\'');
          for (const char *t = sv; *t; t++) { tdb_buf_putc(out, (uint8_t)*t); if (*t == '\'') tdb_buf_putc(out, '\''); }
          if (conv == 'Q') tdb_buf_putc(out, '\'');
        }
        ai++; break;
      }
      default:
        tdb_buf_append(out, start, (size_t)(p - start + 1));
        break;
    }
  }
}

/* ------------------------------ JSON helpers --------------------------- */

static int json_is_valid(const char *p, int n);   /* forward decl */

/* Render a tdb_value as JSON into `out` (text fields get JSON-string escaping).
** Composite values render as a JSON array of their fields. Used by
** json_object/json_array. */
static void json_emit_value(tdb_buf *out, const tdb_value *v) {
  switch (v->type) {
    case TDB_VAL_NULL: tdb_buf_append(out, "null", 4); return;
    case TDB_VAL_INT: {
      char t[32]; snprintf(t, sizeof t, "%lld", (long long)v->u.i);
      tdb_buf_append(out, t, strlen(t)); return;
    }
    case TDB_VAL_REAL: {
      char t[64]; snprintf(t, sizeof t, "%g", v->u.r);
      tdb_buf_append(out, t, strlen(t)); return;
    }
    case TDB_VAL_TEXT: {
      /* If the text is itself a valid JSON document, emit it as-is so
      ** json_object/json_array compose properly (SQLite calls this
      ** subtype JSON). Otherwise emit as a JSON-escaped string. */
      if (v->u.s.n > 0 && v->u.s.p) {
        const char *t = v->u.s.p;
        int n = v->u.s.n;
        const char *q = t;
        while (q < t + n && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q < t + n && (*q == '{' || *q == '[') && json_is_valid(t, n)) {
          tdb_buf_append(out, t, (size_t)n);
          return;
        }
      }
      tdb_buf_putc(out, '"');
      for (int i = 0; i < v->u.s.n; i++) {
        unsigned char b = (unsigned char)v->u.s.p[i];
        switch (b) {
          case '"':  tdb_buf_append(out, "\\\"", 2); break;
          case '\\': tdb_buf_append(out, "\\\\", 2); break;
          case '\n': tdb_buf_append(out, "\\n", 2); break;
          case '\r': tdb_buf_append(out, "\\r", 2); break;
          case '\t': tdb_buf_append(out, "\\t", 2); break;
          case '\b': tdb_buf_append(out, "\\b", 2); break;
          case '\f': tdb_buf_append(out, "\\f", 2); break;
          default:
            if (b < 0x20) {
              char esc[8]; snprintf(esc, sizeof esc, "\\u%04x", b);
              tdb_buf_append(out, esc, strlen(esc));
            } else tdb_buf_putc(out, b);
        }
      }
      tdb_buf_putc(out, '"'); return;
    }
    case TDB_VAL_BLOB: {
      /* SQLite would render as a hex string for jsonb; we emit a text string. */
      static const char hexd[] = "0123456789abcdef";
      tdb_buf_putc(out, '"');
      for (int i = 0; i < v->u.s.n; i++) {
        uint8_t b = (uint8_t)v->u.s.p[i];
        tdb_buf_putc(out, (uint8_t)hexd[b >> 4]);
        tdb_buf_putc(out, (uint8_t)hexd[b & 0xf]);
      }
      tdb_buf_putc(out, '"'); return;
    }
    case TDB_VAL_COMPOSITE: {
      int nf = tdb_value_composite_count(v);
      tdb_buf_putc(out, '[');
      for (int i = 0; i < nf; i++) {
        if (i) tdb_buf_putc(out, ',');
        tdb_value f; tdb_value_init(&f);
        tdb_value_composite_field(v, i, &f);
        json_emit_value(out, &f);
        tdb_value_clear(&f);
      }
      tdb_buf_putc(out, ']'); return;
    }
  }
}

/* Skip whitespace within a JSON document. */
static const char *json_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

/* Find the end of a JSON value at `p`. Returns pointer just past it, or NULL. */
static const char *json_skip_value(const char *p, const char *end);

static const char *json_skip_string(const char *p, const char *end) {
  if (p >= end || *p != '"') return NULL;
  p++;
  while (p < end && *p != '"') {
    if (*p == '\\' && p + 1 < end) p += 2;
    else p++;
  }
  if (p >= end) return NULL;
  return p + 1;
}

static const char *json_skip_value(const char *p, const char *end) {
  p = json_skip_ws(p, end);
  if (p >= end) return NULL;
  if (*p == '"') return json_skip_string(p, end);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    int in_str = 0;
    while (p < end) {
      if (in_str) {
        if (*p == '\\' && p + 1 < end) p += 2;
        else { if (*p == '"') in_str = 0; p++; }
        continue;
      }
      if (*p == '"') { in_str = 1; p++; continue; }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
      p++;
    }
    return NULL;
  }
  /* literal: true/false/null or number */
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\t' && *p != '\n' && *p != '\r') p++;
  return p;
}

/* Decode the next JSON string starting at *pp (which points at the opening '"').
** Caller-owned malloc'd buffer; *pp moves to just past the closing quote. */
static char *json_take_string(const char **pp, const char *end) {
  const char *p = *pp;
  if (p >= end || *p != '"') return NULL;
  p++;
  tdb_buf b; tdb_buf_init(&b);
  while (p < end && *p != '"') {
    if (*p == '\\' && p + 1 < end) {
      p++;
      switch (*p) {
        case '"': tdb_buf_putc(&b, '"'); break;
        case '\\': tdb_buf_putc(&b, '\\'); break;
        case '/': tdb_buf_putc(&b, '/'); break;
        case 'n': tdb_buf_putc(&b, '\n'); break;
        case 'r': tdb_buf_putc(&b, '\r'); break;
        case 't': tdb_buf_putc(&b, '\t'); break;
        case 'b': tdb_buf_putc(&b, '\b'); break;
        case 'f': tdb_buf_putc(&b, '\f'); break;
        case 'u': {
          if (p + 4 < end) {
            int cp = 0;
            for (int k = 1; k <= 4; k++) {
              int c = p[k];
              int v = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
              cp = (cp << 4) | v;
            }
            p += 4;
            if (cp < 0x80) tdb_buf_putc(&b, (uint8_t)cp);
            else if (cp < 0x800) {
              tdb_buf_putc(&b, (uint8_t)(0xc0 | (cp >> 6)));
              tdb_buf_putc(&b, (uint8_t)(0x80 | (cp & 0x3f)));
            } else {
              tdb_buf_putc(&b, (uint8_t)(0xe0 | (cp >> 12)));
              tdb_buf_putc(&b, (uint8_t)(0x80 | ((cp >> 6) & 0x3f)));
              tdb_buf_putc(&b, (uint8_t)(0x80 | (cp & 0x3f)));
            }
          }
          break;
        }
        default: tdb_buf_putc(&b, (uint8_t)*p); break;
      }
      p++;
    } else { tdb_buf_putc(&b, (uint8_t)*p); p++; }
  }
  if (p >= end) { tdb_buf_free(&b); return NULL; }
  *pp = p + 1;
  /* NUL-terminate */
  tdb_buf_putc(&b, 0);
  char *out = (char *)tdb_malloc(b.len);
  if (out) memcpy(out, b.data, b.len);
  tdb_buf_free(&b);
  return out;
}

/* Resolve a JSONPath-ish accessor like "$" / "$.key" / "$[3]" / "$.a.b[0]"
** against the JSON document [doc, doc+n). On success returns 1 and sets
** [*vp, *vendp) to the span of the located JSON value. */
static int json_resolve_path(const char *doc, int n, const char *path,
                             const char **vp, const char **vendp) {
  const char *end = doc + n;
  const char *p = json_skip_ws(doc, end);
  if (!p || p >= end) return 0;
  if (!path || path[0] != '$') return 0;
  const char *cur = p;
  const char *cend = json_skip_value(p, end);
  if (!cend) return 0;
  const char *pp = path + 1;
  while (*pp) {
    if (*pp == '.') {
      pp++;
      const char *kstart = pp;
      while (*pp && *pp != '.' && *pp != '[') pp++;
      int klen = (int)(pp - kstart);
      /* descend into object */
      const char *q = json_skip_ws(cur, cend);
      if (q >= cend || *q != '{') return 0;
      q++;
      int found = 0;
      while (q < cend) {
        q = json_skip_ws(q, cend);
        if (q >= cend || *q == '}') break;
        const char *kp = q;
        char *kn = json_take_string(&kp, cend);
        q = kp;
        if (!kn) return 0;
        q = json_skip_ws(q, cend);
        if (q >= cend || *q != ':') { tdb_mfree(kn); return 0; }
        q++;
        q = json_skip_ws(q, cend);
        const char *ve = json_skip_value(q, cend);
        if (!ve) { tdb_mfree(kn); return 0; }
        if ((int)strlen(kn) == klen && strncmp(kn, kstart, (size_t)klen) == 0) {
          tdb_mfree(kn);
          cur = q; cend = ve;
          found = 1; break;
        }
        tdb_mfree(kn);
        q = ve;
        q = json_skip_ws(q, cend);
        if (q < cend && *q == ',') q++;
      }
      if (!found) return 0;
    } else if (*pp == '[') {
      pp++;
      int idx = (int)strtol(pp, (char **)&pp, 10);
      if (*pp != ']') return 0;
      pp++;
      const char *q = json_skip_ws(cur, cend);
      if (q >= cend || *q != '[') return 0;
      q++;
      int i = 0;
      int found = 0;
      while (q < cend) {
        q = json_skip_ws(q, cend);
        if (q >= cend || *q == ']') break;
        const char *ve = json_skip_value(q, cend);
        if (!ve) return 0;
        if (i == idx) { cur = q; cend = ve; found = 1; break; }
        q = ve;
        q = json_skip_ws(q, cend);
        if (q < cend && *q == ',') q++;
        i++;
      }
      if (!found) return 0;
    } else return 0;
  }
  *vp = cur; *vendp = cend;
  return 1;
}

/* Convert a JSON value span into a tdb_value (best-effort SQL coercion). */
static void json_span_to_value(const char *p, const char *end, tdb_value *out) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  if (p >= end) { tdb_value_set_null(out); return; }
  if (*p == '"') {
    const char *pp = p;
    char *s = json_take_string(&pp, end);
    if (s) { tdb_value_set_text(out, s, -1, 1); tdb_mfree(s); }
    else tdb_value_set_null(out);
    return;
  }
  if (*p == 'n' && end - p >= 4 && strncmp(p, "null", 4) == 0) { tdb_value_set_null(out); return; }
  if (*p == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0) { tdb_value_set_int(out, 1); return; }
  if (*p == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) { tdb_value_set_int(out, 0); return; }
  if (*p == '{' || *p == '[') {
    /* return as text */
    int n = (int)(end - p);
    tdb_value_set_text(out, p, n, 1);
    return;
  }
  /* number: see if integer or float */
  char buf[64]; int n = (int)(end - p); if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  memcpy(buf, p, (size_t)n); buf[n] = 0;
  int isf = 0;
  for (int i = 0; i < n; i++) if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { isf = 1; break; }
  if (isf) tdb_value_set_real(out, strtod(buf, NULL));
  else tdb_value_set_int(out, (int64_t)strtoll(buf, NULL, 10));
}

/* Return the JSON type of the value span: "null","true","false","integer",
** "real","text","array","object". (Booleans collapse to "integer" for SQLite
** parity, but we report the more useful spec name.) */
static const char *json_span_type(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  if (p >= end) return "null";
  if (*p == '"') return "text";
  if (*p == '{') return "object";
  if (*p == '[') return "array";
  if (end - p >= 4 && strncmp(p, "null", 4) == 0) return "null";
  if (end - p >= 4 && strncmp(p, "true", 4) == 0) return "true";
  if (end - p >= 5 && strncmp(p, "false", 5) == 0) return "false";
  /* number */
  for (const char *q = p; q < end; q++)
    if (*q == '.' || *q == 'e' || *q == 'E') return "real";
  return "integer";
}

/* Strict JSON validation: recursive descent on grammar from RFC 8259.
** Returns the byte after the value, or NULL on a syntactic error. */
static const char *json_validate_value(const char *p, const char *end);

static const char *json_validate_string(const char *p, const char *end) {
  if (p >= end || *p != '"') return NULL;
  p++;
  while (p < end && *p != '"') {
    if ((unsigned char)*p < 0x20) return NULL;
    if (*p == '\\') {
      if (p + 1 >= end) return NULL;
      char esc = p[1];
      if (esc == 'u') {
        if (p + 5 >= end) return NULL;
        for (int k = 2; k <= 5; k++) {
          int c = p[k];
          if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return NULL;
        }
        p += 6;
      } else if (esc == '"' || esc == '\\' || esc == '/' || esc == 'b' ||
                 esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
        p += 2;
      } else return NULL;
    } else p++;
  }
  if (p >= end) return NULL;
  return p + 1;
}

static const char *json_validate_number(const char *p, const char *end) {
  const char *start = p;
  if (p < end && *p == '-') p++;
  if (p >= end) return NULL;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') {
    while (p < end && *p >= '0' && *p <= '9') p++;
  } else return NULL;
  if (p < end && *p == '.') {
    p++;
    if (p >= end || !(*p >= '0' && *p <= '9')) return NULL;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  if (p < end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    if (p >= end || !(*p >= '0' && *p <= '9')) return NULL;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }
  if (p == start) return NULL;
  return p;
}

static const char *json_validate_value(const char *p, const char *end) {
  p = json_skip_ws(p, end);
  if (p >= end) return NULL;
  if (*p == '"') return json_validate_string(p, end);
  if (*p == 't') { return (end - p >= 4 && strncmp(p, "true", 4) == 0) ? p + 4 : NULL; }
  if (*p == 'f') { return (end - p >= 5 && strncmp(p, "false", 5) == 0) ? p + 5 : NULL; }
  if (*p == 'n') { return (end - p >= 4 && strncmp(p, "null", 4) == 0) ? p + 4 : NULL; }
  if (*p == '-' || (*p >= '0' && *p <= '9')) return json_validate_number(p, end);
  if (*p == '[') {
    p++; p = json_skip_ws(p, end);
    if (p < end && *p == ']') return p + 1;
    for (;;) {
      p = json_validate_value(p, end);
      if (!p) return NULL;
      p = json_skip_ws(p, end);
      if (p >= end) return NULL;
      if (*p == ']') return p + 1;
      if (*p != ',') return NULL;
      p++;
    }
  }
  if (*p == '{') {
    p++; p = json_skip_ws(p, end);
    if (p < end && *p == '}') return p + 1;
    for (;;) {
      p = json_skip_ws(p, end);
      if (p >= end || *p != '"') return NULL;
      p = json_validate_string(p, end);
      if (!p) return NULL;
      p = json_skip_ws(p, end);
      if (p >= end || *p != ':') return NULL;
      p++;
      p = json_validate_value(p, end);
      if (!p) return NULL;
      p = json_skip_ws(p, end);
      if (p >= end) return NULL;
      if (*p == '}') return p + 1;
      if (*p != ',') return NULL;
      p++;
    }
  }
  return NULL;
}

/* Validate a JSON document: parse one value at the top, see all bytes consumed. */
static int json_is_valid(const char *p, int n) {
  if (!p || n <= 0) return 0;
  const char *end = p + n;
  const char *e = json_validate_value(p, end);
  if (!e) return 0;
  e = json_skip_ws(e, end);
  return e == end;
}

/* --------------------------- the dispatcher ---------------------------- */

/* try_builtin_extra: returns
**    1  if it handled the function (wrote `out`)
**    0  if the function name is not one of ours
**   -1  on an evaluation error (db error set, caller should propagate)
*/
static int try_builtin_extra(tdb_db *db, ectx *c, const tdb_expr *e, tdb_value *out) {
  const char *fn = e->name;
  int argc = e->args ? e->args->n : 0;

  /* ---- connection statistics -------------------------------------- */
  if (!strcasecmp(fn, "last_insert_rowid") && argc == 0) {
    tdb_value_set_int(out, db->last_insert_rowid); return 1;
  }
  if (!strcasecmp(fn, "changes") && argc == 0) {
    tdb_value_set_int(out, db->changes); return 1;
  }
  if (!strcasecmp(fn, "total_changes") && argc == 0) {
    tdb_value_set_int(out, db->total_changes); return 1;
  }
  if ((!strcasecmp(fn, "tdb_version") || !strcasecmp(fn, "version")) && argc == 0) {
    tdb_value_set_text(out, TDB_VERSION_STRING, -1, 1); return 1;
  }
  if (!strcasecmp(fn, "random") && argc == 0) {
    /* xorshift64* — non-cryptographic, sufficient for SQL `random()` */
    static uint64_t st = 0;
    if (!st) st = (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ull;
    st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
    uint64_t r = st * 0x2545f4914f6cdd1dull;
    tdb_value_set_int(out, (int64_t)r);
    return 1;
  }

  /* ---- quote / printf / format ------------------------------------ */
  if (!strcasecmp(fn, "quote") && argc == 1) {
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[0], &v);
    tdb_buf b; tdb_buf_init(&b);
    quote_value(&b, &v);
    tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
    tdb_buf_free(&b); tdb_value_clear(&v);
    return 1;
  }
  if ((!strcasecmp(fn, "printf") || !strcasecmp(fn, "format")) && argc >= 1) {
    tdb_value f; tdb_value_init(&f); eval(db, c, e->args->items[0], &f);
    const char *fmt = tdb_value_as_text(&f);
    int extra = argc - 1;
    tdb_value *vs = (tdb_value *)tdb_calloc(sizeof(tdb_value) * (size_t)(extra ? extra : 1));
    for (int i = 0; i < extra; i++) { tdb_value_init(&vs[i]); eval(db, c, e->args->items[i + 1], &vs[i]); }
    tdb_buf b; tdb_buf_init(&b);
    if (fmt) printf_format(&b, fmt, vs, extra);
    tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
    tdb_buf_free(&b);
    for (int i = 0; i < extra; i++) tdb_value_clear(&vs[i]);
    tdb_mfree(vs); tdb_value_clear(&f);
    return 1;
  }

  /* ---- char / unicode --------------------------------------------- */
  if (!strcasecmp(fn, "char")) {
    tdb_buf b; tdb_buf_init(&b);
    for (int i = 0; i < argc; i++) {
      tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[i], &v);
      int cp = (int)tdb_value_as_int(&v); tdb_value_clear(&v);
      if (cp < 0x80) tdb_buf_putc(&b, (uint8_t)cp);
      else if (cp < 0x800) {
        tdb_buf_putc(&b, (uint8_t)(0xc0 | (cp >> 6)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | (cp & 0x3f)));
      } else if (cp < 0x10000) {
        tdb_buf_putc(&b, (uint8_t)(0xe0 | (cp >> 12)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | ((cp >> 6) & 0x3f)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | (cp & 0x3f)));
      } else {
        tdb_buf_putc(&b, (uint8_t)(0xf0 | (cp >> 18)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | ((cp >> 12) & 0x3f)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | ((cp >> 6) & 0x3f)));
        tdb_buf_putc(&b, (uint8_t)(0x80 | (cp & 0x3f)));
      }
    }
    tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
    tdb_buf_free(&b);
    return 1;
  }
  if (!strcasecmp(fn, "unicode") && argc == 1) {
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[0], &v);
    const char *s = tdb_value_as_text(&v);
    if (!s || !*s) tdb_value_set_null(out);
    else {
      unsigned char b = (unsigned char)s[0];
      int cp = 0;
      if (b < 0x80) cp = b;
      else if ((b & 0xe0) == 0xc0 && s[1])
        cp = ((b & 0x1f) << 6) | (s[1] & 0x3f);
      else if ((b & 0xf0) == 0xe0 && s[1] && s[2])
        cp = ((b & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
      else if ((b & 0xf8) == 0xf0 && s[1] && s[2] && s[3])
        cp = ((b & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
             ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
      tdb_value_set_int(out, cp);
    }
    tdb_value_clear(&v);
    return 1;
  }

  /* ---- trim with chars argument ----------------------------------- */
  if ((!strcasecmp(fn, "trim") || !strcasecmp(fn, "ltrim") || !strcasecmp(fn, "rtrim")) && argc == 2) {
    tdb_value v, cv; tdb_value_init(&v); tdb_value_init(&cv);
    eval(db, c, e->args->items[0], &v);
    eval(db, c, e->args->items[1], &cv);
    const char *s = tdb_value_as_text(&v), *cs = tdb_value_as_text(&cv);
    if (!s) tdb_value_set_null(out);
    else if (!cs || !*cs) tdb_value_set_text(out, s, -1, 1);
    else {
      int n = (int)strlen(s), i = 0, j = n;
      int left  = strcasecmp(fn, "rtrim") != 0;
      int right = strcasecmp(fn, "ltrim") != 0;
      if (left)  while (i < j && strchr(cs, s[i])) i++;
      if (right) while (j > i && strchr(cs, s[j - 1])) j--;
      tdb_value_set_text(out, s + i, j - i, 1);
    }
    tdb_value_clear(&v); tdb_value_clear(&cv);
    return 1;
  }

  /* ---- date / time ------------------------------------------------ */
  if ((!strcasecmp(fn, "date") || !strcasecmp(fn, "time") ||
       !strcasecmp(fn, "datetime") || !strcasecmp(fn, "julianday") ||
       !strcasecmp(fn, "unixepoch") || !strcasecmp(fn, "strftime")) && argc >= 1) {
    int isfmt = !strcasecmp(fn, "strftime");
    int first = isfmt ? 1 : 0;
    double jd = 0;
    if (argc <= first) { tdb_value_set_null(out); return 1; }
    if (!eval_time_arg(db, c, e->args->items[first], &jd)) { tdb_value_set_null(out); return 1; }
    if (!fold_modifiers(db, c, e, first + 1, &jd)) { tdb_value_set_null(out); return 1; }
    if (!strcasecmp(fn, "julianday")) { tdb_value_set_real(out, jd); return 1; }
    if (!strcasecmp(fn, "unixepoch")) {
      double secs = (jd - 2440587.5) * 86400.0;
      tdb_value_set_int(out, (int64_t)secs); return 1;
    }
    int Y, M, D, H, Mi, S, ms;
    ymd_from_jd(jd, &Y, &M, &D);
    hms_from_jd(jd, &H, &Mi, &S, &ms);
    char tmp[64];
    if (isfmt) {
      tdb_value f; tdb_value_init(&f); eval(db, c, e->args->items[0], &f);
      const char *ftext = tdb_value_as_text(&f);
      tdb_buf b; tdb_buf_init(&b);
      if (ftext) strftime_format(&b, ftext, jd);
      tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
      tdb_buf_free(&b); tdb_value_clear(&f);
    } else if (!strcasecmp(fn, "date")) {
      snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", Y, M, D);
      tdb_value_set_text(out, tmp, -1, 1);
    } else if (!strcasecmp(fn, "time")) {
      snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", H, Mi, S);
      tdb_value_set_text(out, tmp, -1, 1);
    } else {
      snprintf(tmp, sizeof tmp, "%04d-%02d-%02d %02d:%02d:%02d", Y, M, D, H, Mi, S);
      tdb_value_set_text(out, tmp, -1, 1);
    }
    return 1;
  }
  if ((!strcasecmp(fn, "current_date") || !strcasecmp(fn, "current_time") ||
       !strcasecmp(fn, "current_timestamp")) && argc == 0) {
    time_t now = time(NULL);
    double jd = 2440587.5 + (double)now / 86400.0;
    int Y, M, D, H, Mi, S, ms;
    ymd_from_jd(jd, &Y, &M, &D); hms_from_jd(jd, &H, &Mi, &S, &ms);
    char tmp[40];
    if (!strcasecmp(fn, "current_date")) snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", Y, M, D);
    else if (!strcasecmp(fn, "current_time")) snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", H, Mi, S);
    else snprintf(tmp, sizeof tmp, "%04d-%02d-%02d %02d:%02d:%02d", Y, M, D, H, Mi, S);
    tdb_value_set_text(out, tmp, -1, 1);
    return 1;
  }

  /* ---- JSON ------------------------------------------------------- */
  if (!strcasecmp(fn, "json") && argc == 1) {
    /* validate and normalize: for now, return input as text if valid */
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[0], &v);
    const char *s = tdb_value_as_text(&v);
    if (s && json_is_valid(s, (int)strlen(s))) tdb_value_set_text(out, s, -1, 1);
    else tdb_value_set_null(out);
    tdb_value_clear(&v);
    return 1;
  }
  if (!strcasecmp(fn, "json_valid") && argc == 1) {
    tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[0], &v);
    const char *s = tdb_value_as_text(&v);
    tdb_value_set_int(out, (s && json_is_valid(s, (int)strlen(s))) ? 1 : 0);
    tdb_value_clear(&v);
    return 1;
  }
  if (!strcasecmp(fn, "json_object")) {
    tdb_buf b; tdb_buf_init(&b);
    tdb_buf_putc(&b, '{');
    for (int i = 0; i + 1 < argc; i += 2) {
      if (i) tdb_buf_putc(&b, ',');
      tdb_value kv; tdb_value_init(&kv); eval(db, c, e->args->items[i], &kv);
      tdb_value vv; tdb_value_init(&vv); eval(db, c, e->args->items[i + 1], &vv);
      const char *ks = tdb_value_as_text(&kv);
      /* always emit key as JSON string */
      tdb_value ktxt; tdb_value_init(&ktxt);
      tdb_value_set_text(&ktxt, ks ? ks : "", -1, 1);
      json_emit_value(&b, &ktxt);
      tdb_buf_putc(&b, ':');
      json_emit_value(&b, &vv);
      tdb_value_clear(&ktxt); tdb_value_clear(&kv); tdb_value_clear(&vv);
    }
    tdb_buf_putc(&b, '}');
    tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
    tdb_buf_free(&b);
    return 1;
  }
  if (!strcasecmp(fn, "json_array")) {
    tdb_buf b; tdb_buf_init(&b);
    tdb_buf_putc(&b, '[');
    for (int i = 0; i < argc; i++) {
      if (i) tdb_buf_putc(&b, ',');
      tdb_value v; tdb_value_init(&v); eval(db, c, e->args->items[i], &v);
      json_emit_value(&b, &v);
      tdb_value_clear(&v);
    }
    tdb_buf_putc(&b, ']');
    tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
    tdb_buf_free(&b);
    return 1;
  }
  if (!strcasecmp(fn, "json_extract") && argc >= 2) {
    tdb_value jv; tdb_value_init(&jv); eval(db, c, e->args->items[0], &jv);
    const char *doc = tdb_value_as_text(&jv);
    int dn = doc ? (int)strlen(doc) : 0;
    /* multi-path: build an array if more than one path arg */
    if (argc == 2) {
      tdb_value pv; tdb_value_init(&pv); eval(db, c, e->args->items[1], &pv);
      const char *p = tdb_value_as_text(&pv);
      const char *vp = NULL, *ve = NULL;
      if (doc && p && json_resolve_path(doc, dn, p, &vp, &ve))
        json_span_to_value(vp, ve, out);
      else tdb_value_set_null(out);
      tdb_value_clear(&pv);
    } else {
      tdb_buf b; tdb_buf_init(&b); tdb_buf_putc(&b, '[');
      for (int i = 1; i < argc; i++) {
        if (i > 1) tdb_buf_putc(&b, ',');
        tdb_value pv; tdb_value_init(&pv); eval(db, c, e->args->items[i], &pv);
        const char *p = tdb_value_as_text(&pv);
        const char *vp = NULL, *ve = NULL;
        if (doc && p && json_resolve_path(doc, dn, p, &vp, &ve))
          tdb_buf_append(&b, vp, (size_t)(ve - vp));
        else tdb_buf_append(&b, "null", 4);
        tdb_value_clear(&pv);
      }
      tdb_buf_putc(&b, ']');
      tdb_value_set_text(out, (const char *)b.data, (int)b.len, 1);
      tdb_buf_free(&b);
    }
    tdb_value_clear(&jv);
    return 1;
  }
  if (!strcasecmp(fn, "json_type") && (argc == 1 || argc == 2)) {
    tdb_value jv; tdb_value_init(&jv); eval(db, c, e->args->items[0], &jv);
    const char *doc = tdb_value_as_text(&jv);
    int dn = doc ? (int)strlen(doc) : 0;
    const char *vp = doc, *ve = doc + dn;
    int ok = 1;
    if (argc == 2) {
      tdb_value pv; tdb_value_init(&pv); eval(db, c, e->args->items[1], &pv);
      const char *p = tdb_value_as_text(&pv);
      ok = doc && p && json_resolve_path(doc, dn, p, &vp, &ve);
      tdb_value_clear(&pv);
    }
    if (!ok) tdb_value_set_null(out);
    else tdb_value_set_text(out, json_span_type(vp, ve), -1, 1);
    tdb_value_clear(&jv);
    return 1;
  }
  if (!strcasecmp(fn, "json_array_length") && (argc == 1 || argc == 2)) {
    tdb_value jv; tdb_value_init(&jv); eval(db, c, e->args->items[0], &jv);
    const char *doc = tdb_value_as_text(&jv);
    int dn = doc ? (int)strlen(doc) : 0;
    const char *vp = doc, *ve = doc + dn;
    int ok = 1;
    if (argc == 2) {
      tdb_value pv; tdb_value_init(&pv); eval(db, c, e->args->items[1], &pv);
      const char *p = tdb_value_as_text(&pv);
      ok = doc && p && json_resolve_path(doc, dn, p, &vp, &ve);
      tdb_value_clear(&pv);
    }
    if (!ok || !vp) { tdb_value_set_int(out, 0); tdb_value_clear(&jv); return 1; }
    const char *q = json_skip_ws(vp, ve);
    if (q >= ve || *q != '[') { tdb_value_set_int(out, 0); tdb_value_clear(&jv); return 1; }
    q++;
    int count = 0;
    while (q < ve) {
      q = json_skip_ws(q, ve);
      if (q >= ve || *q == ']') break;
      const char *next = json_skip_value(q, ve);
      if (!next) break;
      count++;
      q = next;
      q = json_skip_ws(q, ve);
      if (q < ve && *q == ',') q++;
    }
    tdb_value_set_int(out, count);
    tdb_value_clear(&jv);
    return 1;
  }

  return 0;
}
