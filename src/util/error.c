#include "tdb/error.h"

const char *tdb_status_str(tdb_status_t status) {
    switch (status) {
        case TDB_OK:               return "OK";
        case TDB_ERR_NOMEM:        return "Out of memory";
        case TDB_ERR_IO:           return "I/O error";
        case TDB_ERR_CORRUPT:      return "Data corruption detected";
        case TDB_ERR_FULL:         return "Storage full";
        case TDB_ERR_NOT_FOUND:    return "Not found";
        case TDB_ERR_EXISTS:       return "Already exists";
        case TDB_ERR_TYPE_MISMATCH:return "Type mismatch";
        case TDB_ERR_CONSTRAINT:   return "Constraint violation";
        case TDB_ERR_SYNTAX:       return "Syntax error";
        case TDB_ERR_TXN_ABORT:    return "Transaction aborted";
        case TDB_ERR_TXN_CONFLICT: return "Transaction conflict";
        case TDB_ERR_LOCK_TIMEOUT: return "Lock timeout";
        case TDB_ERR_DEADLOCK:     return "Deadlock detected";
        case TDB_ERR_CRYPTO:       return "Encryption/decryption error";
        case TDB_ERR_INVALID_ARG:  return "Invalid argument";
        case TDB_ERR_UNSUPPORTED:  return "Unsupported operation";
        case TDB_ERR_INTERNAL:     return "Internal error";
    }
    return "Unknown error";
}
