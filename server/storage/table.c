/*
 * ============================================================================
 * Event Ticketing Platform — Table Abstraction Implementation
 * ============================================================================
 * Integrates page I/O, buffer pool caching, B+ Tree indexing, and WAL logging
 * into a unified CRUD interface for each table.
 *
 * Design decisions:
 *   - txn_id is hardcoded to 0 (transaction manager is Phase 3)
 *   - Free-space management uses a simple "try last page first" strategy
 *   - Soft deletes: records are flagged deleted, not physically removed
 *   - is_deleted flag is located at a table-specific offset; we use
 *     etp_record_size() to determine record layout
 * ============================================================================
 */

#include "table.h"
#include "page.h"
#include "buffer_pool.h"
#include "btree.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Placeholder txn_id until transaction manager is built (Phase 3) */
#define CURRENT_TXN_ID  0

/* ================================================================
 * Internal: Get the offset of the is_deleted flag within a record
 *
 * All record types share the convention that is_deleted is a uint8_t
 * positioned after the fixed fields. We compute it per table type
 * from the known record structures in types.h.
 * ================================================================ */
static size_t get_deleted_flag_offset(table_id_t table)
{
    switch (table) {
        case TABLE_USERS:
            return offsetof(user_record_t, is_deleted);
        case TABLE_EVENTS:
            return offsetof(event_record_t, is_deleted);
        case TABLE_SEATS:
            return offsetof(seat_record_t, is_deleted);
        case TABLE_BOOKINGS:
            return offsetof(booking_record_t, is_deleted);
        case TABLE_BOOKING_SEATS:
            return offsetof(booking_seat_record_t, is_deleted);
        default:
            return 0;
    }
}

/*
 * Check if a record is soft-deleted by reading its is_deleted flag.
 */
static int is_record_deleted(table_id_t table, const void *record)
{
    size_t offset = get_deleted_flag_offset(table);
    const uint8_t *flag = (const uint8_t *)record + offset;
    return (*flag != 0);
}

/*
 * Set the is_deleted flag in a record buffer.
 */
static void mark_record_deleted(table_id_t table, void *record)
{
    size_t offset = get_deleted_flag_offset(table);
    uint8_t *flag = (uint8_t *)record + offset;
    *flag = 1;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * table_create — Create a new table with fresh data file and index
 *
 * 1. Allocate and initialize table_t
 * 2. Create the page-based data file
 * 3. Create a new B+ Tree index file
 * 4. Set record_size from the global type definitions
 */
table_t *table_create(table_id_t id, const char *data_file,
                       const char *index_file,
                       buffer_pool_t *pool, wal_t *wal)
{
    if (!data_file || !index_file || !pool || !wal) {
        etp_log(LOG_ERROR, "table_create: NULL argument");
        return NULL;
    }

    table_t *table = (table_t *)calloc(1, sizeof(table_t));
    if (!table) {
        etp_log(LOG_ERROR, "table_create: Out of memory");
        return NULL;
    }

    table->id          = id;
    table->record_size = etp_record_size(id);
    table->pool        = pool;
    table->wal         = wal;
    etp_strlcpy(table->name, etp_table_name(id), sizeof(table->name));

    if (table->record_size == 0) {
        etp_log(LOG_ERROR, "table_create: Unknown table id %d", id);
        free(table);
        return NULL;
    }

    /* Create the page-based data file */
    table->data_fd = page_file_create(data_file);
    if (table->data_fd < 0) {
        etp_log(LOG_ERROR, "table_create: Failed to create data file '%s'", data_file);
        free(table);
        return NULL;
    }

    /* Create the B+ Tree primary key index */
    table->pk_index = btree_create(index_file, BTREE_DEFAULT_ORDER);
    if (!table->pk_index) {
        etp_log(LOG_ERROR, "table_create: Failed to create index '%s'", index_file);
        page_file_close(table->data_fd);
        free(table);
        return NULL;
    }

    etp_log(LOG_INFO, "Table '%s' created (record_size=%u)", table->name, table->record_size);
    return table;
}

/*
 * table_open — Open an existing table's data file and index
 */
table_t *table_open(table_id_t id, const char *data_file,
                     const char *index_file,
                     buffer_pool_t *pool, wal_t *wal)
{
    if (!data_file || !index_file || !pool || !wal) {
        etp_log(LOG_ERROR, "table_open: NULL argument");
        return NULL;
    }

    table_t *table = (table_t *)calloc(1, sizeof(table_t));
    if (!table) {
        etp_log(LOG_ERROR, "table_open: Out of memory");
        return NULL;
    }

    table->id          = id;
    table->record_size = etp_record_size(id);
    table->pool        = pool;
    table->wal         = wal;
    etp_strlcpy(table->name, etp_table_name(id), sizeof(table->name));

    if (table->record_size == 0) {
        etp_log(LOG_ERROR, "table_open: Unknown table id %d", id);
        free(table);
        return NULL;
    }

    /* Open existing data file */
    table->data_fd = page_file_open(data_file);
    if (table->data_fd < 0) {
        etp_log(LOG_ERROR, "table_open: Failed to open data file '%s'", data_file);
        free(table);
        return NULL;
    }

    /* Open existing B+ Tree index */
    table->pk_index = btree_open(index_file);
    if (!table->pk_index) {
        etp_log(LOG_ERROR, "table_open: Failed to open index '%s'", index_file);
        page_file_close(table->data_fd);
        free(table);
        return NULL;
    }

    etp_log(LOG_INFO, "Table '%s' opened (record_size=%u)", table->name, table->record_size);
    return table;
}

/*
 * table_close — Flush dirty pages and release table resources
 *
 * Does NOT destroy the shared buffer pool or WAL (they are shared).
 */
void table_close(table_t *table)
{
    if (!table) return;

    etp_log(LOG_INFO, "Closing table '%s'", table->name);

    /* Flush any dirty pages for this table */
    table_flush(table);

    /* Close the B+ Tree index */
    if (table->pk_index) {
        btree_close(table->pk_index);
        table->pk_index = NULL;
    }

    /* Close the data file */
    if (table->data_fd >= 0) {
        page_file_close(table->data_fd);
        table->data_fd = -1;
    }

    free(table);
}

/* ================================================================
 * CRUD Operations
 * ================================================================ */

/*
 * table_insert — Insert a new record into the table
 *
 * Steps:
 *   1. Auto-assign ID via etp_next_id() — stored in first 4 bytes
 *   2. Find a data page with free space (try last page, else allocate)
 *   3. Log INSERT to WAL (before modifying data)
 *   4. Insert record into page via buffer pool
 *   5. Index the record in the B+ Tree (key=ID, value={page_id,slot_id})
 *   6. Return the assigned ID
 */
etp_result_t table_insert(table_t *table, void *record, uint32_t *out_id)
{
    if (!table || !record) return ETP_ERR_INVALID_ARG;

    /* Step 1: Auto-assign primary key */
    uint32_t new_id = etp_next_id(table->id);
    *((uint32_t *)record) = new_id;

    /* Step 2: Find a page with free space */
    page_id_t target_page = INVALID_PAGE_ID;
    uint32_t num_pages = page_get_count(table->data_fd);

    if (num_pages > 0) {
        /* Try the last page first */
        page_id_t last_page = num_pages - 1;
        void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, last_page);
        if (page_buf) {
            if (!page_is_full(page_buf)) {
                target_page = last_page;
            }
            buffer_pool_unpin(table->pool, table->data_fd, last_page);
        }
    }

    /* If no existing page has space, allocate a new one */
    if (target_page == INVALID_PAGE_ID) {
        target_page = page_alloc(table->data_fd, table->record_size);
        if (target_page == INVALID_PAGE_ID) {
            etp_log(LOG_ERROR, "table_insert [%s]: Failed to allocate new page", table->name);
            return ETP_ERR_IO;
        }
    }

    /* Step 3: Log to WAL BEFORE modifying data (Write-Ahead!) */
    lsn_t lsn = wal_log_insert(table->wal, CURRENT_TXN_ID, table->id,
                                new_id, record, table->record_size);
    if (lsn == INVALID_LSN) {
        etp_log(LOG_ERROR, "table_insert [%s]: WAL logging failed for id=%u",
                table->name, new_id);
        return ETP_ERR_IO;
    }

    /* Step 4: Insert into the data page via the buffer pool */
    void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, target_page);
    if (!page_buf) {
        etp_log(LOG_ERROR, "table_insert [%s]: Failed to fetch page %u",
                table->name, target_page);
        return ETP_ERR_IO;
    }

    int slot_id = page_insert_record(page_buf, record);
    if (slot_id < 0) {
        /* Page became full between check and insert (race condition).
         * Try allocating a new page as fallback. */
        buffer_pool_unpin(table->pool, table->data_fd, target_page);

        target_page = page_alloc(table->data_fd, table->record_size);
        if (target_page == INVALID_PAGE_ID) {
            etp_log(LOG_ERROR, "table_insert [%s]: Fallback page alloc failed", table->name);
            return ETP_ERR_FULL;
        }

        page_buf = buffer_pool_fetch(table->pool, table->data_fd, target_page);
        if (!page_buf) {
            etp_log(LOG_ERROR, "table_insert [%s]: Failed to fetch fallback page %u",
                    table->name, target_page);
            return ETP_ERR_IO;
        }

        slot_id = page_insert_record(page_buf, record);
        if (slot_id < 0) {
            buffer_pool_unpin(table->pool, table->data_fd, target_page);
            etp_log(LOG_ERROR, "table_insert [%s]: Insert failed on fresh page", table->name);
            return ETP_ERR_FULL;
        }
    }

    buffer_pool_mark_dirty(table->pool, table->data_fd, target_page);
    buffer_pool_unpin(table->pool, table->data_fd, target_page);

    /* Step 5: Index in the B+ Tree */
    record_ptr_t ptr = { .page_id = target_page, .slot_id = (uint16_t)slot_id };
    if (btree_insert(table->pk_index, new_id, ptr) != 0) {
        etp_log(LOG_ERROR, "table_insert [%s]: B+ Tree insert failed for id=%u",
                table->name, new_id);
        return ETP_ERR_IO;
    }

    /* Step 6: Return the assigned ID */
    if (out_id) *out_id = new_id;

    etp_log(LOG_DEBUG, "table_insert [%s]: Inserted id=%u at page=%u slot=%d (LSN=%lu)",
            table->name, new_id, target_page, slot_id, (unsigned long)lsn);

    return ETP_OK;
}

/*
 * table_find_by_id — Find a record by its primary key
 *
 * Steps:
 *   1. Search the B+ Tree for the record pointer
 *   2. Fetch the page through the buffer pool
 *   3. Get the record slot and check is_deleted
 *   4. Copy record to caller's buffer
 *   5. Unpin the page
 */
etp_result_t table_find_by_id(table_t *table, uint32_t id, void *out_record)
{
    if (!table || !out_record) return ETP_ERR_INVALID_ARG;

    /* Step 1: B+ Tree lookup */
    record_ptr_t ptr;
    if (btree_search(table->pk_index, id, &ptr) != 0) {
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 2: Fetch the data page */
    void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, ptr.page_id);
    if (!page_buf) {
        etp_log(LOG_ERROR, "table_find_by_id [%s]: Failed to fetch page %u",
                table->name, ptr.page_id);
        return ETP_ERR_IO;
    }

    /* Step 3: Get the record from the slot */
    void *record = page_get_record(page_buf, ptr.slot_id);
    if (!record) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 4: Check soft-delete flag */
    if (is_record_deleted(table->id, record)) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 5: Copy record to output buffer */
    memcpy(out_record, record, table->record_size);

    /* Step 6: Unpin */
    buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);

    return ETP_OK;
}

/*
 * table_update — Update an existing record in-place
 *
 * Steps:
 *   1. Find the existing record via B+ Tree + buffer pool
 *   2. Verify it's not deleted
 *   3. Log UPDATE to WAL (old + new images)
 *   4. Update the record in the page
 *   5. Mark page dirty and unpin
 */
etp_result_t table_update(table_t *table, uint32_t id, const void *new_record)
{
    if (!table || !new_record) return ETP_ERR_INVALID_ARG;

    /* Step 1: B+ Tree lookup */
    record_ptr_t ptr;
    if (btree_search(table->pk_index, id, &ptr) != 0) {
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 2: Fetch the page and get old record */
    void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, ptr.page_id);
    if (!page_buf) {
        etp_log(LOG_ERROR, "table_update [%s]: Failed to fetch page %u",
                table->name, ptr.page_id);
        return ETP_ERR_IO;
    }

    void *old_record = page_get_record(page_buf, ptr.slot_id);
    if (!old_record) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Check soft-delete */
    if (is_record_deleted(table->id, old_record)) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 3: Log UPDATE to WAL (old + new images, before modifying data) */
    lsn_t lsn = wal_log_update(table->wal, CURRENT_TXN_ID, table->id,
                                id, old_record, new_record, table->record_size);
    if (lsn == INVALID_LSN) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        etp_log(LOG_ERROR, "table_update [%s]: WAL logging failed for id=%u",
                table->name, id);
        return ETP_ERR_IO;
    }

    /* Step 4: Update the record in the page */
    if (page_update_record(page_buf, ptr.slot_id, new_record) != 0) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        etp_log(LOG_ERROR, "table_update [%s]: page_update_record failed for id=%u",
                table->name, id);
        return ETP_ERR_IO;
    }

    /* Step 5: Mark dirty and unpin */
    buffer_pool_mark_dirty(table->pool, table->data_fd, ptr.page_id);
    buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);

    etp_log(LOG_DEBUG, "table_update [%s]: Updated id=%u (LSN=%lu)",
            table->name, id, (unsigned long)lsn);

    return ETP_OK;
}

/*
 * table_delete — Soft-delete a record
 *
 * Steps:
 *   1. Find the record via B+ Tree + buffer pool
 *   2. Verify it's not already deleted
 *   3. Log DELETE to WAL (old image)
 *   4. Set is_deleted = 1 in the page
 *   5. Remove from B+ Tree index
 *   6. Mark page dirty and unpin
 */
etp_result_t table_delete(table_t *table, uint32_t id)
{
    if (!table) return ETP_ERR_INVALID_ARG;

    /* Step 1: B+ Tree lookup */
    record_ptr_t ptr;
    if (btree_search(table->pk_index, id, &ptr) != 0) {
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 2: Fetch page and get record */
    void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, ptr.page_id);
    if (!page_buf) {
        etp_log(LOG_ERROR, "table_delete [%s]: Failed to fetch page %u",
                table->name, ptr.page_id);
        return ETP_ERR_IO;
    }

    void *record = page_get_record(page_buf, ptr.slot_id);
    if (!record) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Already deleted? */
    if (is_record_deleted(table->id, record)) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Step 3: Log DELETE to WAL (before modifying data) */
    lsn_t lsn = wal_log_delete(table->wal, CURRENT_TXN_ID, table->id,
                                id, record, table->record_size);
    if (lsn == INVALID_LSN) {
        buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);
        etp_log(LOG_ERROR, "table_delete [%s]: WAL logging failed for id=%u",
                table->name, id);
        return ETP_ERR_IO;
    }

    /* Step 4: Soft-delete — set the is_deleted flag to 1 */
    mark_record_deleted(table->id, record);

    /* Step 5: Remove from the B+ Tree index */
    btree_delete(table->pk_index, id);

    /* Step 6: Mark dirty and unpin */
    buffer_pool_mark_dirty(table->pool, table->data_fd, ptr.page_id);
    buffer_pool_unpin(table->pool, table->data_fd, ptr.page_id);

    etp_log(LOG_DEBUG, "table_delete [%s]: Deleted id=%u (LSN=%lu)",
            table->name, id, (unsigned long)lsn);

    return ETP_OK;
}

/* ================================================================
 * Scanning & Aggregation
 * ================================================================ */

/*
 * table_scan — Full table scan with optional filter
 *
 * Iterates all pages, examines every slot, skips deleted records.
 * Matching records are copied into the results buffer.
 *
 * Returns the number of matching records found (up to max_results).
 */
int table_scan(table_t *table, table_filter_fn filter, void *context,
               void *results, int max_results)
{
    if (!table || !results || max_results <= 0) return 0;

    uint32_t num_pages = page_get_count(table->data_fd);
    int found = 0;
    uint8_t *result_ptr = (uint8_t *)results;

    for (page_id_t pid = 0; pid < num_pages && found < max_results; pid++) {
        void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, pid);
        if (!page_buf) continue;

        uint16_t num_records = page_get_record_count(page_buf);

        for (uint16_t slot = 0; slot < num_records && found < max_results; slot++) {
            void *record = page_get_record(page_buf, slot);
            if (!record) continue;

            /* Skip soft-deleted records */
            if (is_record_deleted(table->id, record)) continue;

            /* Apply filter if provided */
            if (filter && !filter(record, context)) continue;

            /* Copy matching record to results buffer */
            memcpy(result_ptr + ((size_t)found * table->record_size),
                   record, table->record_size);
            found++;
        }

        buffer_pool_unpin(table->pool, table->data_fd, pid);
    }

    etp_log(LOG_DEBUG, "table_scan [%s]: Found %d records across %u pages",
            table->name, found, num_pages);

    return found;
}

/*
 * table_count — Count all non-deleted records
 *
 * Full scan without copying data — just counts.
 */
int table_count(table_t *table)
{
    if (!table) return 0;

    uint32_t num_pages = page_get_count(table->data_fd);
    int count = 0;

    for (page_id_t pid = 0; pid < num_pages; pid++) {
        void *page_buf = buffer_pool_fetch(table->pool, table->data_fd, pid);
        if (!page_buf) continue;

        uint16_t num_records = page_get_record_count(page_buf);

        for (uint16_t slot = 0; slot < num_records; slot++) {
            void *record = page_get_record(page_buf, slot);
            if (!record) continue;

            if (!is_record_deleted(table->id, record)) {
                count++;
            }
        }

        buffer_pool_unpin(table->pool, table->data_fd, pid);
    }

    return count;
}

/* ================================================================
 * Durability
 * ================================================================ */

/*
 * table_flush — Flush all dirty pages for this table
 *
 * Delegates to the buffer pool's flush-all mechanism.
 * Returns 0 on success, -1 on failure.
 */
int table_flush(table_t *table)
{
    if (!table || !table->pool) return -1;

    /* Flush all dirty pages in the buffer pool.
     * Note: buffer_pool_flush_all flushes pages for ALL tables, not just
     * this one. A per-fd flush would be more efficient but the current
     * buffer pool API doesn't support it. This is acceptable since flush
     * is infrequent (table close, checkpoint). */
    return buffer_pool_flush_all(table->pool);
}
