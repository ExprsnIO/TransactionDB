#ifndef TDB_ERROR_H
#define TDB_ERROR_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *tdb_status_str(tdb_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* TDB_ERROR_H */
