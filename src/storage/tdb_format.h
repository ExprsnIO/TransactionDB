/*
** tdb_format.h — on-disk file format constants and byte-offset layout.
**
** A TransactionDB database is a single file divided into fixed-size pages
** (default 4096 bytes). All multi-byte integers on disk are big-endian.
**
**   Page 1 begins with a 100-byte database header, after which the page
**   carries the root of the catalog B-tree like any other page.
**
** Durability is provided by a separate write-ahead log (WAL) file: committed
** page images are appended there first and later checkpointed back into the
** main database file.
**
** This header contains only constants and offset macros — no code — so every
** layer agrees on the exact byte layout.
*/
#ifndef TDB_FORMAT_H
#define TDB_FORMAT_H

#include "../common/tdb_internal.h"

/* ------------------------------------------------------------------ */
/* Global constants                                                    */
/* ------------------------------------------------------------------ */
#define TDB_MAGIC          "TransactionDB"   /* 13 chars + NUL padding */
#define TDB_MAGIC_LEN      16
#define TDB_FORMAT_VERSION 1u

#define TDB_DEFAULT_PAGE_SIZE 4096u
#define TDB_MIN_PAGE_SIZE      512u
#define TDB_MAX_PAGE_SIZE    65536u

#define TDB_HEADER_SIZE    100u    /* database header at start of page 1 */
#define TDB_CATALOG_PGNO   1u      /* page 1 holds the catalog B-tree root */

/* ------------------------------------------------------------------ */
/* Database header field offsets (within the first 100 bytes)          */
/* ------------------------------------------------------------------ */
#define TDB_HDR_MAGIC          0   /* 16 bytes  */
#define TDB_HDR_PAGE_SIZE      16   /* u16       */
#define TDB_HDR_WRITE_VERSION  18   /* u8        */
#define TDB_HDR_READ_VERSION   19   /* u8        */
#define TDB_HDR_FORMAT_VERSION 20   /* u32       */
#define TDB_HDR_CHANGE_COUNTER 24   /* u32, bumped each commit */
#define TDB_HDR_DB_SIZE        28   /* u32, size of db in pages */
#define TDB_HDR_FREELIST_HEAD  32   /* u32, first freelist trunk page (0=none) */
#define TDB_HDR_FREELIST_COUNT 36   /* u32, total free pages */
#define TDB_HDR_CATALOG_ROOT   40   /* u32, root page of catalog (==1) */
#define TDB_HDR_SCHEMA_COOKIE  44   /* u32, bumped on any DDL */
#define TDB_HDR_TEXT_ENCODING  48   /* u32, 1=UTF-8 */
#define TDB_HDR_MAX_TXNID      52   /* u64, largest committed txn id */
#define TDB_HDR_RESERVED_PER_PG 60  /* u32, reserved bytes at end of each page */
#define TDB_HDR_RESERVED       64   /* 32 bytes reserved for future use */
#define TDB_HDR_CRC32C         96   /* u32, CRC of bytes [0,96) */

#define TDB_TEXT_ENC_UTF8      1u

/* ------------------------------------------------------------------ */
/* Page types (first byte of a page's btree header)                    */
/* ------------------------------------------------------------------ */
#define TDB_PAGE_INTERIOR_TABLE 0x05
#define TDB_PAGE_LEAF_TABLE     0x0D
#define TDB_PAGE_INTERIOR_INDEX 0x02
#define TDB_PAGE_LEAF_INDEX     0x0A
#define TDB_PAGE_OVERFLOW       0xFF
#define TDB_PAGE_FREELIST_TRUNK 0xFE

/* ------------------------------------------------------------------ */
/* B-tree page header (relative to the start of the btree area)        */
/* ------------------------------------------------------------------ */
#define TDB_BT_TYPE          0   /* u8  page type */
#define TDB_BT_FIRST_FREE    1   /* u16 first freeblock offset (0=none) */
#define TDB_BT_NCELL         3   /* u16 number of cells */
#define TDB_BT_CELL_CONTENT  5   /* u16 start of cell content area */
#define TDB_BT_FRAG_BYTES    7   /* u8  fragmented free bytes */
#define TDB_BT_RIGHT_CHILD   8   /* u32 right-most child (interior only) */

#define TDB_BT_LEAF_HDR_SIZE     8
#define TDB_BT_INTERIOR_HDR_SIZE 12

/* ------------------------------------------------------------------ */
/* Freelist trunk page layout                                          */
/* ------------------------------------------------------------------ */
#define TDB_FL_TYPE        0   /* u8  == TDB_PAGE_FREELIST_TRUNK */
#define TDB_FL_NEXT_TRUNK  1   /* u32 next trunk page (0=none) */
#define TDB_FL_LEAF_COUNT  5   /* u32 number of leaf entries */
#define TDB_FL_LEAVES      9   /* u32[] leaf page numbers */

/* ------------------------------------------------------------------ */
/* WAL file layout                                                     */
/* ------------------------------------------------------------------ */
#define TDB_WAL_MAGIC      0x7764424Bu /* "wdBK"-ish magic */
#define TDB_WAL_HDR_SIZE   32
#define TDB_WAL_HDR_MAGIC      0   /* u32 */
#define TDB_WAL_HDR_VERSION    4   /* u32 */
#define TDB_WAL_HDR_PAGE_SIZE  8   /* u32 */
#define TDB_WAL_HDR_CKPT_SEQ   12  /* u32 checkpoint sequence */
#define TDB_WAL_HDR_SALT1      16  /* u32 */
#define TDB_WAL_HDR_SALT2      20  /* u32 */
#define TDB_WAL_HDR_CRC1       24  /* u32 header checksum-1 */
#define TDB_WAL_HDR_CRC2       28  /* u32 header checksum-2 */

#define TDB_WAL_FRAME_HDR_SIZE 24
#define TDB_WAL_FR_PGNO        0   /* u32 page number */
#define TDB_WAL_FR_DBSIZE      4   /* u32 db size in pages after commit (0=not commit) */
#define TDB_WAL_FR_SALT1       8   /* u32 (must match header salt) */
#define TDB_WAL_FR_SALT2       12  /* u32 */
#define TDB_WAL_FR_CRC1        16  /* u32 rolling checksum-1 */
#define TDB_WAL_FR_CRC2        20  /* u32 rolling checksum-2 */

#endif /* TDB_FORMAT_H */
