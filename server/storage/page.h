/*
 * ============================================================================
 * Event Ticketing Platform — Page I/O Interface
 * ============================================================================
 * Lowest level of the storage engine. Each data file is divided into
 * fixed-size pages (PAGE_SIZE = 4096 bytes). This module provides:
 *
 *   1. File operations  — create / open / close data files
 *   2. Disk I/O         — read / write / allocate pages (pread/pwrite)
 *   3. Record ops       — insert / get / update / delete records in a
 *                          page buffer that lives in memory
 *
 * Thread safety: pread/pwrite are used for positional I/O so no global
 * file offset is shared.  Advisory file locks (flock) are applied when
 * a file is opened and released on close.
 * ============================================================================
 */

#ifndef ETP_PAGE_H
#define ETP_PAGE_H

#include <stdint.h>
#include "../../common/types.h"
#include "../../common/config.h"

/* ================================================================
 * Page Header — first PAGE_HEADER_SIZE (16) bytes of every page
 * ================================================================
 *
 * Byte layout:
 *   [0..3]   page_id            uint32_t
 *   [4..5]   record_count       uint16_t   — current live records
 *   [6..7]   free_space_offset  uint16_t   — byte offset of next free slot
 *   [8..9]   record_size        uint16_t   — fixed size of each record
 *   [10..11] flags              uint16_t   — status bit-flags
 *   [12..15] reserved           uint32_t   — future use (zero-filled)
 */
typedef struct __attribute__((packed)) {
    uint32_t    page_id;
    uint16_t    record_count;
    uint16_t    free_space_offset;
    uint16_t    record_size;
    uint16_t    flags;
    uint32_t    reserved;
} page_header_t;

_Static_assert(sizeof(page_header_t) == PAGE_HEADER_SIZE,
               "page_header_t must be PAGE_HEADER_SIZE bytes");

/* ================================================================
 * Page Header Flag Bits
 * ================================================================ */
#define PAGE_FLAG_DIRTY         0x0001  /* Page has been modified in memory  */
#define PAGE_FLAG_HAS_DELETED   0x0002  /* Page contains soft-deleted slots  */

/* ================================================================
 * File Operations
 * ================================================================ */

/*
 * page_file_create — Create a new, empty data file.
 * Returns a file descriptor on success, or -1 on failure.
 * The file is opened for read/write and an advisory lock is acquired.
 */
int page_file_create(const char *filename);

/*
 * page_file_open — Open an existing data file for read/write.
 * Returns a file descriptor on success, or -1 on failure.
 * An advisory lock is acquired on the file.
 */
int page_file_open(const char *filename);

/*
 * page_file_close — Release the advisory lock and close the file descriptor.
 */
void page_file_close(int fd);

/* ================================================================
 * Page-Level Disk I/O
 * ================================================================ */

/*
 * page_read — Read page `page_id` from file `fd` into `page_buf`.
 * `page_buf` must point to at least PAGE_SIZE bytes.
 * Returns 0 on success, -1 on error.
 */
int page_read(int fd, page_id_t page_id, void *page_buf);

/*
 * page_write — Write `page_buf` (PAGE_SIZE bytes) to page `page_id` in file.
 * Returns 0 on success, -1 on error.
 */
int page_write(int fd, page_id_t page_id, const void *page_buf);

/*
 * page_alloc — Append a new page to the file, initialise its header
 * with the given record_size, and return the new page's id.
 * Returns INVALID_PAGE_ID on failure.
 */
page_id_t page_alloc(int fd, uint16_t record_size);

/*
 * page_get_count — Return the number of pages currently in the file.
 * Calculated from the file size (file_size / PAGE_SIZE).
 */
uint32_t page_get_count(int fd);

/* ================================================================
 * In-Memory Record Operations (work on a page buffer in RAM)
 * ================================================================ */

/*
 * page_init_header — Zero the page buffer and write a fresh header.
 */
void page_init_header(void *page_buf, page_id_t page_id, uint16_t record_size);

/*
 * page_insert_record — Append `record` (record_size bytes) into the next
 * free slot.  Returns the slot index (0-based) on success, or -1 if the
 * page is full.
 */
int page_insert_record(void *page_buf, const void *record);

/*
 * page_get_record — Return a pointer directly into `page_buf` at the
 * given slot.  No data is copied.  Returns NULL if slot_id is out of range.
 */
void *page_get_record(const void *page_buf, uint16_t slot_id);

/*
 * page_update_record — Overwrite the record at `slot_id` with `record`.
 * Returns 0 on success, -1 if slot_id is out of range.
 */
int page_update_record(void *page_buf, uint16_t slot_id, const void *record);

/*
 * page_delete_record — Soft-delete the record at `slot_id` by setting
 * its first byte (the is_deleted flag) to 1.
 * Returns 0 on success, -1 if slot_id is out of range.
 */
int page_delete_record(void *page_buf, uint16_t slot_id);

/*
 * page_get_record_count — Return the record_count field from the header.
 */
uint16_t page_get_record_count(const void *page_buf);

/*
 * page_get_max_records — How many records of `record_size` fit in the
 * usable area of a page (PAGE_SIZE − PAGE_HEADER_SIZE).
 */
uint16_t page_get_max_records(uint16_t record_size);

/*
 * page_is_full — Returns 1 if the page cannot accept another record,
 * 0 otherwise.
 */
int page_is_full(const void *page_buf);

#endif /* ETP_PAGE_H */
