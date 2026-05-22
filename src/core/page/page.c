#include "tdb/page.h"
#include <string.h>
#include <stdlib.h>

/*
 * Page layout:
 * [PageHeader][tuple data... ->][<- ...slot array]
 *
 * PageHeader sits at the start. Tuple data grows forward from after the header.
 * The slot array grows backward from the end of the page.
 * Free space is between free_offset and free_end.
 */

/* Internal helper: get slot entry pointer. Slot 0 is at the highest address. */
static tdb_slot_entry_t *get_slot_entry(void *page, uint16_t slot_count, tdb_slot_t slot) {
    tdb_slot_entry_t *entries = (tdb_slot_entry_t *)((uint8_t *)page +
        ((tdb_page_header_t *)page)->free_end);
    return &entries[slot_count - 1 - slot];
}

static const tdb_slot_entry_t *get_slot_entry_const(const void *page, uint16_t slot_count, tdb_slot_t slot) {
    const tdb_page_header_t *hdr = (const tdb_page_header_t *)page;
    const tdb_slot_entry_t *entries = (const tdb_slot_entry_t *)((const uint8_t *)page + hdr->free_end);
    return &entries[slot_count - 1 - slot];
}

tdb_status_t tdb_page_init(void *page, size_t page_size, tdb_page_id_t page_id) {
    if (!page || page_size < 128) return TDB_ERR_INVALID_ARG;

    memset(page, 0, page_size);
    tdb_page_header_t *hdr = (tdb_page_header_t *)page;
    hdr->page_id = page_id;
    hdr->lsn = 0;
    hdr->flags = 0;
    hdr->free_offset = (uint16_t)sizeof(tdb_page_header_t);
    hdr->free_end = (uint16_t)page_size;
    hdr->slot_count = 0;
    hdr->checksum = 0;

    return TDB_OK;
}

size_t tdb_page_free_space(const void *page) {
    const tdb_page_header_t *hdr = (const tdb_page_header_t *)page;
    if (hdr->free_end <= hdr->free_offset) return 0;
    return hdr->free_end - hdr->free_offset;
}

tdb_status_t tdb_page_insert_tuple(void *page, const void *data, uint16_t len, tdb_slot_t *slot_out) {
    tdb_page_header_t *hdr = (tdb_page_header_t *)page;

    size_t needed = len + sizeof(tdb_slot_entry_t);
    if (tdb_page_free_space(page) < needed) return TDB_ERR_FULL;

    /* Allocate slot entry from the end */
    hdr->free_end -= (uint16_t)sizeof(tdb_slot_entry_t);
    tdb_slot_entry_t *slot_entry = (tdb_slot_entry_t *)((uint8_t *)page + hdr->free_end);
    slot_entry->offset = hdr->free_offset;
    slot_entry->length = len;

    /* Copy tuple data */
    memcpy((uint8_t *)page + hdr->free_offset, data, len);
    hdr->free_offset += len;

    if (slot_out) *slot_out = hdr->slot_count;
    hdr->slot_count++;

    return TDB_OK;
}

tdb_status_t tdb_page_get_tuple(const void *page, tdb_slot_t slot,
                                 const void **data_out, uint16_t *len_out) {
    const tdb_page_header_t *hdr = (const tdb_page_header_t *)page;

    if (slot >= hdr->slot_count) return TDB_ERR_NOT_FOUND;

    const tdb_slot_entry_t *entry = get_slot_entry_const(page, hdr->slot_count, slot);

    if (entry->length == 0) return TDB_ERR_NOT_FOUND; /* deleted tuple */

    if (data_out) *data_out = (const uint8_t *)page + entry->offset;
    if (len_out) *len_out = entry->length;

    return TDB_OK;
}

tdb_status_t tdb_page_delete_tuple(void *page, tdb_slot_t slot) {
    tdb_page_header_t *hdr = (tdb_page_header_t *)page;

    if (slot >= hdr->slot_count) return TDB_ERR_NOT_FOUND;

    tdb_slot_entry_t *entry = get_slot_entry(page, hdr->slot_count, slot);

    if (entry->length == 0) return TDB_ERR_NOT_FOUND; /* already deleted */

    /* Mark as deleted by zeroing the length.
     * The space is not reclaimed until page compaction. */
    entry->length = 0;

    return TDB_OK;
}

tdb_status_t tdb_page_update_tuple(void *page, tdb_slot_t slot,
                                    const void *data, uint16_t len) {
    tdb_page_header_t *hdr = (tdb_page_header_t *)page;

    if (slot >= hdr->slot_count) return TDB_ERR_NOT_FOUND;

    tdb_slot_entry_t *entry = get_slot_entry(page, hdr->slot_count, slot);

    if (entry->length == 0) return TDB_ERR_NOT_FOUND; /* deleted */

    if (len <= entry->length) {
        /* Fits in existing space — update in place */
        memcpy((uint8_t *)page + entry->offset, data, len);
        entry->length = len;
        return TDB_OK;
    }

    /* Doesn't fit — need to allocate new space. Mark old slot dead, insert at end. */
    size_t needed = len; /* no new slot needed, reuse existing */
    if ((size_t)(hdr->free_end - hdr->free_offset) < needed) return TDB_ERR_FULL;

    /* Write new data at free_offset */
    entry->offset = hdr->free_offset;
    entry->length = len;
    memcpy((uint8_t *)page + hdr->free_offset, data, len);
    hdr->free_offset += len;

    return TDB_OK;
}
