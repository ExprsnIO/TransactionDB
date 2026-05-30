/* tdb_status.c — status-code to string mapping and version helpers. */
#include "transactiondb.h"

const char *tdb_status_str(int code) {
  switch (code) {
    case TDB_OK:          return "ok";
    case TDB_ERROR:       return "generic error";
    case TDB_BUSY:        return "database is busy";
    case TDB_LOCKED:      return "database table is locked";
    case TDB_NOMEM:       return "out of memory";
    case TDB_READONLY:    return "attempt to write a readonly database";
    case TDB_IOERR:       return "disk I/O error";
    case TDB_CORRUPT:     return "database disk image is malformed";
    case TDB_NOTFOUND:    return "not found";
    case TDB_FULL:        return "database or disk is full";
    case TDB_CONSTRAINT:  return "constraint failed";
    case TDB_MISMATCH:    return "datatype mismatch";
    case TDB_MISUSE:      return "library routine called out of sequence";
    case TDB_RANGE:       return "column/bind index out of range";
    case TDB_ABORT:       return "operation aborted";
    case TDB_UNSUPPORTED: return "feature not supported";
    case TDB_DONE:        return "done";
    case TDB_ROW:         return "row available";
    default:              return "unknown status";
  }
}

const char *tdb_libversion(void) { return TDB_VERSION_STRING; }
