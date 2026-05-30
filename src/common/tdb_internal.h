/*
** tdb_internal.h — umbrella internal header.
**
** Pulls in the public status codes and declares cross-cutting opaque types and
** forward declarations shared across engine layers. Internal source files
** include this first.
*/
#ifndef TDB_INTERNAL_H
#define TDB_INTERNAL_H

#include "transactiondb.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* On-disk integers are unsigned; page numbers start at 1 (0 == "no page"). */
typedef uint32_t tdb_pgno;
typedef uint64_t tdb_rowid;
typedef uint64_t tdb_txnid;

/* Forward declarations of major engine objects. */
typedef struct tdb_vfs      tdb_vfs;
typedef struct tdb_file     tdb_file;
typedef struct tdb_pager    tdb_pager;
typedef struct tdb_page     tdb_page;
typedef struct tdb_wal      tdb_wal;
typedef struct tdb_btree    tdb_btree;
typedef struct tdb_cursor   tdb_cursor;
typedef struct tdb_storage  tdb_storage;
typedef struct tdb_txn      tdb_txn;
typedef struct tdb_lockmgr  tdb_lockmgr;
typedef struct tdb_catalog  tdb_catalog;
typedef struct tdb_lua      tdb_lua;

/* Convenience: silence "unused" while keeping intent explicit. */
#define TDB_UNUSED(x) ((void)(x))

/* Min/max helpers (no double evaluation guard needed at our call sites). */
#define TDB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TDB_MAX(a, b) ((a) > (b) ? (a) : (b))

#endif /* TDB_INTERNAL_H */
