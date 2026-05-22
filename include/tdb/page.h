#ifndef TDB_PAGE_H
#define TDB_PAGE_H

#include "tdb/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Page flags */
#define TDB_PAGE_FLAG_LEAF      0x0001
#define TDB_PAGE_FLAG_INTERNAL  0x0002
#define TDB_PAGE_FLAG_OVERFLOW  0x0004
#define TDB_PAGE_FLAG_ENCRYPTED 0x0008

/* Page header — sits at the start of every page */
typedef struct {
    tdb_page_id_t page_id;
    tdb_lsn_t     lsn;
    uint16_t      flags;
    uint16_t      free_offset;   /* start of free space */
    uint16_t      free_end;      /* end of free space (start of slot array) */
    uint16_t      slot_count;
    uint32_t      checksum;
} tdb_page_header_t;

/* Slot directory entry */
typedef struct {
    uint16_t offset;
    uint16_t length;      /* 0 = deleted */
} tdb_slot_entry_t;

/* Initialize a page with the given page size and id */
tdb_status_t tdb_page_init(void *page, size_t page_size, tdb_page_id_t page_id);

/* Get available free space in a page */
size_t tdb_page_free_space(const void *page);

/* Insert a tuple into the page, returning the slot number */
tdb_status_t tdb_page_insert_tuple(void *page, const void *data, uint16_t len, tdb_slot_t *slot_out);

/* Get a tuple from the page by slot number */
tdb_status_t tdb_page_get_tuple(const void *page, tdb_slot_t slot,
                                 const void **data_out, uint16_t *len_out);

/* Delete a tuple by marking its slot length to 0 */
tdb_status_t tdb_page_delete_tuple(void *page, tdb_slot_t slot);

/* Update a tuple in place (must fit in existing slot) */
tdb_status_t tdb_page_update_tuple(void *page, tdb_slot_t slot,
                                    const void *data, uint16_t len);

/* Get the page header (read-only) */
static inline const tdb_page_header_t *tdb_page_get_header(const void *page) {
    return (const tdb_page_header_t *)page;
}

/* Get the page header (mutable) */
static inline tdb_page_header_t *tdb_page_get_header_mut(void *page) {
    return (tdb_page_header_t *)page;
}

#ifdef __cplusplus
}
#endif

#endif /* TDB_PAGE_H */
