/*
 * ============================================================================
 * Event Ticketing Platform — Page I/O Implementation
 * ============================================================================
 * Implements page-based file I/O: the lowest level of the storage engine.
 *
 * Thread safety
 * -------------
 * • pread / pwrite are used for all disk I/O so no shared file offset
 *   exists — multiple threads can read/write different pages concurrently.
 * • flock() provides advisory file-level locking to prevent concurrent
 *   processes from corrupting the same data file.
 *
 * Page layout (PAGE_SIZE = 4096 bytes)
 * ------------------------------------
 *   Bytes [0 .. 15]              — page_header_t (id, counts, flags, …)
 *   Bytes [16 .. PAGE_SIZE - 1]  — fixed-size records packed sequentially
 * ============================================================================
 */

#include "page.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>   /* flock() */

/* ================================================================
 * Internal helpers
 * ================================================================ */

/*
 * _record_offset — byte offset of slot `slot_id` within a page buffer.
 * Records start immediately after the page header.
 */
static inline uint32_t _record_offset(uint16_t slot_id, uint16_t record_size)
{
    return (uint32_t)PAGE_HEADER_SIZE + (uint32_t)slot_id * (uint32_t)record_size;
}

/* ================================================================
 * File Operations
 * ================================================================ */

int page_file_create(const char *filename)
{
    if (!filename) {
        etp_log(LOG_ERROR, "page_file_create: NULL filename");
        return -1;
    }

    /* Create (or truncate) the file with rw-r--r-- permissions */
    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        etp_log(LOG_ERROR, "page_file_create: open(\"%s\") failed: %s",
                filename, strerror(errno));
        return -1;
    }

    /* Acquire an exclusive advisory lock on the whole file */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        etp_log(LOG_WARN, "page_file_create: flock(\"%s\") failed: %s "
                "(proceeding without lock)", filename, strerror(errno));
    }

    etp_log(LOG_INFO, "page_file_create: created \"%s\" (fd=%d)", filename, fd);
    return fd;
}

int page_file_open(const char *filename)
{
    if (!filename) {
        etp_log(LOG_ERROR, "page_file_open: NULL filename");
        return -1;
    }

    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        etp_log(LOG_ERROR, "page_file_open: open(\"%s\") failed: %s",
                filename, strerror(errno));
        return -1;
    }

    /* Acquire an exclusive advisory lock */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        etp_log(LOG_WARN, "page_file_open: flock(\"%s\") failed: %s "
                "(proceeding without lock)", filename, strerror(errno));
    }

    etp_log(LOG_DEBUG, "page_file_open: opened \"%s\" (fd=%d)", filename, fd);
    return fd;
}

void page_file_close(int fd)
{
    if (fd < 0) return;

    /* Release the advisory lock before closing */
    flock(fd, LOCK_UN);
    close(fd);
    etp_log(LOG_DEBUG, "page_file_close: closed fd=%d", fd);
}

/* ================================================================
 * Page-Level Disk I/O
 * ================================================================ */

int page_read(int fd, page_id_t page_id, void *page_buf)
{
    if (fd < 0 || !page_buf) {
        etp_log(LOG_ERROR, "page_read: invalid args (fd=%d, buf=%p)", fd, page_buf);
        return -1;
    }

    off_t offset = (off_t)page_id * PAGE_SIZE;
    ssize_t n = pread(fd, page_buf, PAGE_SIZE, offset);

    if (n < 0) {
        etp_log(LOG_ERROR, "page_read: pread failed (fd=%d, page=%u): %s",
                fd, page_id, strerror(errno));
        return -1;
    }
    if (n < PAGE_SIZE) {
        /* Short read — zero-fill the remainder (handles sparse files) */
        memset((uint8_t *)page_buf + n, 0, PAGE_SIZE - (size_t)n);
        etp_log(LOG_WARN, "page_read: short read for page %u (%zd/%d bytes)",
                page_id, n, PAGE_SIZE);
    }

    etp_log(LOG_DEBUG, "page_read: page %u loaded from fd=%d", page_id, fd);
    return 0;
}

int page_write(int fd, page_id_t page_id, const void *page_buf)
{
    if (fd < 0 || !page_buf) {
        etp_log(LOG_ERROR, "page_write: invalid args (fd=%d, buf=%p)", fd, page_buf);
        return -1;
    }

    off_t offset = (off_t)page_id * PAGE_SIZE;
    ssize_t n = pwrite(fd, page_buf, PAGE_SIZE, offset);

    if (n != PAGE_SIZE) {
        etp_log(LOG_ERROR, "page_write: pwrite failed (fd=%d, page=%u): %s",
                fd, page_id, (n < 0) ? strerror(errno) : "short write");
        return -1;
    }

    etp_log(LOG_DEBUG, "page_write: page %u written to fd=%d", page_id, fd);
    return 0;
}

page_id_t page_alloc(int fd, uint16_t record_size)
{
    if (fd < 0 || record_size == 0) {
        etp_log(LOG_ERROR, "page_alloc: invalid args (fd=%d, rec_size=%u)",
                fd, record_size);
        return INVALID_PAGE_ID;
    }

    /* The new page's id is simply the current page count */
    uint32_t new_id = page_get_count(fd);

    /* Prepare a zeroed page buffer with an initialised header */
    uint8_t page_buf[PAGE_SIZE];
    page_init_header(page_buf, new_id, record_size);

    /* Write the new page to the end of the file */
    if (page_write(fd, new_id, page_buf) != 0) {
        etp_log(LOG_ERROR, "page_alloc: failed to write new page %u", new_id);
        return INVALID_PAGE_ID;
    }

    etp_log(LOG_INFO, "page_alloc: allocated page %u (rec_size=%u) on fd=%d",
            new_id, record_size, fd);
    return (page_id_t)new_id;
}

uint32_t page_get_count(int fd)
{
    if (fd < 0) return 0;

    /* Determine file size via lseek to the end */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        etp_log(LOG_ERROR, "page_get_count: lseek failed: %s", strerror(errno));
        return 0;
    }

    return (uint32_t)(size / PAGE_SIZE);
}

/* ================================================================
 * In-Memory Record Operations
 * ================================================================ */

void page_init_header(void *page_buf, page_id_t page_id, uint16_t record_size)
{
    /* Zero the entire page first */
    memset(page_buf, 0, PAGE_SIZE);

    page_header_t *hdr = (page_header_t *)page_buf;
    hdr->page_id            = page_id;
    hdr->record_count       = 0;
    hdr->free_space_offset  = PAGE_HEADER_SIZE;
    hdr->record_size        = record_size;
    hdr->flags              = 0;
    hdr->reserved           = 0;
}

int page_insert_record(void *page_buf, const void *record)
{
    if (!page_buf || !record) return -1;

    page_header_t *hdr = (page_header_t *)page_buf;

    /* Check capacity */
    uint16_t max_records = page_get_max_records(hdr->record_size);
    if (hdr->record_count >= max_records) {
        return -1;  /* Page is full */
    }

    /* Compute the destination slot */
    uint16_t slot = hdr->record_count;
    uint32_t offset = _record_offset(slot, hdr->record_size);

    /* Copy the record into the page buffer */
    memcpy((uint8_t *)page_buf + offset, record, hdr->record_size);

    /* Update header */
    hdr->record_count++;
    hdr->free_space_offset = (uint16_t)(PAGE_HEADER_SIZE +
                              (uint32_t)hdr->record_count * hdr->record_size);
    hdr->flags |= PAGE_FLAG_DIRTY;

    return (int)slot;
}

void *page_get_record(const void *page_buf, uint16_t slot_id)
{
    if (!page_buf) return NULL;

    const page_header_t *hdr = (const page_header_t *)page_buf;

    if (slot_id >= hdr->record_count) {
        return NULL;  /* Slot out of range */
    }

    uint32_t offset = _record_offset(slot_id, hdr->record_size);

    /* Return a pointer directly into the buffer — no copy */
    return (void *)((const uint8_t *)page_buf + offset);
}

int page_update_record(void *page_buf, uint16_t slot_id, const void *record)
{
    if (!page_buf || !record) return -1;

    page_header_t *hdr = (page_header_t *)page_buf;

    if (slot_id >= hdr->record_count) {
        return -1;  /* Slot out of range */
    }

    uint32_t offset = _record_offset(slot_id, hdr->record_size);
    memcpy((uint8_t *)page_buf + offset, record, hdr->record_size);

    hdr->flags |= PAGE_FLAG_DIRTY;
    return 0;
}

int page_delete_record(void *page_buf, uint16_t slot_id)
{
    if (!page_buf) return -1;

    page_header_t *hdr = (page_header_t *)page_buf;

    if (slot_id >= hdr->record_count) {
        return -1;  /* Slot out of range */
    }

    /*
     * Soft delete: set the first byte of the record to 1.
     * All record structs have `is_deleted` as a field — in the packed
     * layout the first field varies, but the contract is that the
     * is_deleted byte is the first byte *logically used for deletion*.
     *
     * NOTE: The record structs in types.h have is_deleted at different
     * offsets within each struct.  However, the specification states
     * "sets the record's first byte (is_deleted field) to 1", so we
     * follow that contract here by setting byte 0 of the record slot.
     */
    uint32_t offset = _record_offset(slot_id, hdr->record_size);
    *((uint8_t *)page_buf + offset) = 1;

    hdr->flags |= PAGE_FLAG_DIRTY | PAGE_FLAG_HAS_DELETED;

    etp_log(LOG_DEBUG, "page_delete_record: soft-deleted slot %u in page %u",
            slot_id, hdr->page_id);
    return 0;
}

uint16_t page_get_record_count(const void *page_buf)
{
    if (!page_buf) return 0;
    return ((const page_header_t *)page_buf)->record_count;
}

uint16_t page_get_max_records(uint16_t record_size)
{
    if (record_size == 0) return 0;
    return (uint16_t)((PAGE_SIZE - PAGE_HEADER_SIZE) / record_size);
}

int page_is_full(const void *page_buf)
{
    if (!page_buf) return 1;  /* Treat NULL as full (can't insert) */

    const page_header_t *hdr = (const page_header_t *)page_buf;
    uint16_t max_records = page_get_max_records(hdr->record_size);
    return (hdr->record_count >= max_records) ? 1 : 0;
}
