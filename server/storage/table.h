/*
 * ============================================================================
 * Event Ticketing Platform — Table Abstraction
 * ============================================================================
 * Highest-level storage engine module. Integrates page management, buffer pool,
 * B+ Tree indexing, and Write-Ahead Logging into a clean CRUD API for the
 * business logic layer.
 *
 * Each table owns its data file and primary-key index, but shares a common
 * buffer pool and WAL with other tables.
 * ============================================================================
 */

#ifndef ETP_TABLE_H
#define ETP_TABLE_H

#include <stdint.h>
#include "../../common/types.h"
#include "wal.h"

/* Forward declarations for modules built in parallel */
typedef struct buffer_pool buffer_pool_t;
typedef struct btree btree_t;

/* ================================================================
 * Table Handle
 * ================================================================ */
typedef struct {
    table_id_t      id;             /* Which table this is                     */
    char            name[32];       /* Table name (e.g., "users")              */
    uint16_t        record_size;    /* Size of one record in bytes             */
    int             data_fd;        /* File descriptor for .dat file           */
    btree_t        *pk_index;       /* Primary key B+ Tree index               */
    buffer_pool_t  *pool;           /* Shared buffer pool (NOT owned)          */
    wal_t          *wal;            /* Shared WAL (NOT owned)                  */
} table_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create a new table — creates data file and index from scratch */
table_t *table_create(table_id_t id, const char *data_file,
                       const char *index_file,
                       buffer_pool_t *pool, wal_t *wal);

/* Open an existing table — opens data file and index for use */
table_t *table_open(table_id_t id, const char *data_file,
                     const char *index_file,
                     buffer_pool_t *pool, wal_t *wal);

/* Close table — flush and release resources (does NOT destroy pool/wal) */
void     table_close(table_t *table);

/* ================================================================
 * CRUD Operations
 * ================================================================ */

/*
 * Insert a new record.
 *   - Auto-assigns an ID (set in the record's first 4 bytes)
 *   - Logs to WAL before writing
 *   - Inserts into a data page and indexes in the B+ Tree
 *   - Returns the assigned ID via *out_id
 */
etp_result_t table_insert(table_t *table, void *record, uint32_t *out_id);

/*
 * Find a record by primary key.
 *   - B+ Tree lookup → buffer pool fetch → copy to out_record
 *   - Returns ETP_ERR_NOT_FOUND if deleted or missing
 */
etp_result_t table_find_by_id(table_t *table, uint32_t id, void *out_record);

/*
 * Update an existing record.
 *   - Finds the existing record, logs old+new to WAL
 *   - Updates the data page in-place
 */
etp_result_t table_update(table_t *table, uint32_t id, const void *new_record);

/*
 * Delete a record (soft-delete).
 *   - Finds the existing record, logs to WAL
 *   - Marks as deleted in the data page, removes from B+ Tree
 */
etp_result_t table_delete(table_t *table, uint32_t id);

/* ================================================================
 * Scanning & Aggregation
 * ================================================================ */

/*
 * Scan all non-deleted records, applying an optional filter.
 *   - filter_fn returns 1 to include, 0 to skip (NULL = include all)
 *   - Copies matching records into `results` buffer
 *   - Returns the number of matching records found (up to max_results)
 */
typedef int (*table_filter_fn)(const void *record, void *context);

int table_scan(table_t *table, table_filter_fn filter, void *context,
               void *results, int max_results);

/* Count all non-deleted records in the table */
int table_count(table_t *table);

/* ================================================================
 * Durability
 * ================================================================ */

/* Flush all dirty pages for this table through the buffer pool */
int table_flush(table_t *table);

#endif /* ETP_TABLE_H */
