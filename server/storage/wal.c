/*
 * ============================================================================
 * Event Ticketing Platform — Write-Ahead Log (WAL) Implementation
 * ============================================================================
 * Append-only log file for crash recovery. Every data mutation is logged here
 * BEFORE it is applied to the data files. On recovery, the WAL is replayed
 * sequentially; the caller determines redo/undo semantics from COMMIT/ABORT
 * markers.
 *
 * Thread safety: All write operations acquire the WAL mutex. Recovery is
 * expected to run single-threaded during startup.
 * ============================================================================
 */

#include "wal.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ================================================================
 * Internal: Write a complete WAL record (header + optional data)
 *
 * This is the core append routine. It:
 *   1. Assigns the next LSN under the mutex
 *   2. Writes header + old_data + new_data atomically (single mutex scope)
 *   3. Returns the assigned LSN, or INVALID_LSN on failure
 * ================================================================ */
static lsn_t wal_append(wal_t *wal, txn_id_t txn_id, wal_op_type_t op_type,
                         table_id_t table, uint32_t record_id,
                         const void *old_data, const void *new_data,
                         uint16_t data_size)
{
    if (!wal) return INVALID_LSN;

    pthread_mutex_lock(&wal->mutex);

    /* Build the header */
    wal_record_header_t header;
    memset(&header, 0, sizeof(header));
    header.lsn       = wal->next_lsn;
    header.txn_id    = txn_id;
    header.op_type   = (uint8_t)op_type;
    header.table_id  = (uint8_t)table;
    header.data_size = data_size;
    header.record_id = record_id;

    /* Write header */
    ssize_t written = write(wal->fd, &header, sizeof(header));
    if (written != (ssize_t)sizeof(header)) {
        etp_log(LOG_ERROR, "WAL: Failed to write header (LSN=%lu): %s",
                (unsigned long)header.lsn, strerror(errno));
        pthread_mutex_unlock(&wal->mutex);
        return INVALID_LSN;
    }

    /* Write old_data if present */
    if (data_size > 0 && old_data) {
        written = write(wal->fd, old_data, data_size);
        if (written != (ssize_t)data_size) {
            etp_log(LOG_ERROR, "WAL: Failed to write old_data (LSN=%lu): %s",
                    (unsigned long)header.lsn, strerror(errno));
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
    } else if (data_size > 0) {
        /* old_data is NULL but data_size > 0 — write zeros (e.g. INSERT) */
        void *zeros = calloc(1, data_size);
        if (!zeros) {
            etp_log(LOG_ERROR, "WAL: Out of memory for zero padding");
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
        written = write(wal->fd, zeros, data_size);
        free(zeros);
        if (written != (ssize_t)data_size) {
            etp_log(LOG_ERROR, "WAL: Failed to write zero old_data (LSN=%lu): %s",
                    (unsigned long)header.lsn, strerror(errno));
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
    }

    /* Write new_data if present */
    if (data_size > 0 && new_data) {
        written = write(wal->fd, new_data, data_size);
        if (written != (ssize_t)data_size) {
            etp_log(LOG_ERROR, "WAL: Failed to write new_data (LSN=%lu): %s",
                    (unsigned long)header.lsn, strerror(errno));
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
    } else if (data_size > 0) {
        /* new_data is NULL but data_size > 0 — write zeros (e.g. DELETE) */
        void *zeros = calloc(1, data_size);
        if (!zeros) {
            etp_log(LOG_ERROR, "WAL: Out of memory for zero padding");
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
        written = write(wal->fd, zeros, data_size);
        free(zeros);
        if (written != (ssize_t)data_size) {
            etp_log(LOG_ERROR, "WAL: Failed to write zero new_data (LSN=%lu): %s",
                    (unsigned long)header.lsn, strerror(errno));
            pthread_mutex_unlock(&wal->mutex);
            return INVALID_LSN;
        }
    }

    lsn_t assigned_lsn = wal->next_lsn;
    wal->next_lsn++;

    pthread_mutex_unlock(&wal->mutex);

    etp_log(LOG_DEBUG, "WAL: Appended record LSN=%lu op=%d table=%d record=%u",
            (unsigned long)assigned_lsn, op_type, table, record_id);

    return assigned_lsn;
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * wal_create — Create a new WAL file (truncates if exists)
 *
 * Opens the file with O_CREAT | O_TRUNC for a fresh WAL.
 * Initializes the LSN counter to 1 (LSN 0 is reserved as INVALID_LSN).
 */
wal_t *wal_create(const char *filename)
{
    if (!filename) {
        etp_log(LOG_ERROR, "WAL: Cannot create WAL with NULL filename");
        return NULL;
    }

    wal_t *wal = (wal_t *)calloc(1, sizeof(wal_t));
    if (!wal) {
        etp_log(LOG_ERROR, "WAL: Failed to allocate wal_t");
        return NULL;
    }

    /* Open file — create new, truncate if existing */
    wal->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wal->fd < 0) {
        etp_log(LOG_ERROR, "WAL: Failed to create '%s': %s", filename, strerror(errno));
        free(wal);
        return NULL;
    }

    wal->next_lsn    = 1;  /* LSN 0 = INVALID_LSN */
    wal->flushed_lsn = 0;
    etp_strlcpy(wal->filename, filename, sizeof(wal->filename));
    pthread_mutex_init(&wal->mutex, NULL);

    etp_log(LOG_INFO, "WAL: Created new WAL file '%s'", filename);
    return wal;
}

/*
 * wal_open — Open an existing WAL file for appending
 *
 * Scans the file to determine the next LSN by reading all existing headers.
 * This ensures new records continue from the correct LSN after a restart.
 */
wal_t *wal_open(const char *filename)
{
    if (!filename) {
        etp_log(LOG_ERROR, "WAL: Cannot open WAL with NULL filename");
        return NULL;
    }

    wal_t *wal = (wal_t *)calloc(1, sizeof(wal_t));
    if (!wal) {
        etp_log(LOG_ERROR, "WAL: Failed to allocate wal_t");
        return NULL;
    }

    /* Open file for reading to scan existing records */
    int read_fd = open(filename, O_RDONLY);
    if (read_fd < 0) {
        etp_log(LOG_ERROR, "WAL: Failed to open '%s' for reading: %s",
                filename, strerror(errno));
        free(wal);
        return NULL;
    }

    /* Scan to find the highest LSN */
    lsn_t max_lsn = 0;
    wal_record_header_t header;
    ssize_t bytes_read;

    while ((bytes_read = read(read_fd, &header, sizeof(header))) == (ssize_t)sizeof(header)) {
        if (header.lsn > max_lsn) {
            max_lsn = header.lsn;
        }
        /* Skip past old_data + new_data */
        if (header.data_size > 0) {
            off_t skip = (off_t)header.data_size * 2;
            if (lseek(read_fd, skip, SEEK_CUR) == (off_t)-1) {
                etp_log(LOG_WARN, "WAL: Truncated record at LSN=%lu during scan",
                        (unsigned long)header.lsn);
                break;
            }
        }
    }
    close(read_fd);

    /* Now open for appending */
    wal->fd = open(filename, O_WRONLY | O_APPEND);
    if (wal->fd < 0) {
        etp_log(LOG_ERROR, "WAL: Failed to open '%s' for appending: %s",
                filename, strerror(errno));
        free(wal);
        return NULL;
    }

    wal->next_lsn    = max_lsn + 1;
    wal->flushed_lsn = max_lsn;    /* Assume existing data is flushed */
    etp_strlcpy(wal->filename, filename, sizeof(wal->filename));
    pthread_mutex_init(&wal->mutex, NULL);

    etp_log(LOG_INFO, "WAL: Opened existing WAL '%s' (next_lsn=%lu)",
            filename, (unsigned long)wal->next_lsn);
    return wal;
}

/*
 * wal_close — Flush all pending writes and close the WAL
 */
void wal_close(wal_t *wal)
{
    if (!wal) return;

    wal_flush(wal);

    pthread_mutex_lock(&wal->mutex);
    if (wal->fd >= 0) {
        close(wal->fd);
        wal->fd = -1;
    }
    pthread_mutex_unlock(&wal->mutex);

    pthread_mutex_destroy(&wal->mutex);

    etp_log(LOG_INFO, "WAL: Closed WAL '%s' (last_lsn=%lu)",
            wal->filename, (unsigned long)(wal->next_lsn - 1));
    free(wal);
}

/* ================================================================
 * Logging Operations
 * ================================================================ */

/*
 * wal_log_insert — Log an INSERT operation
 *
 * old_data = zeros (written automatically), new_data = the inserted record.
 */
lsn_t wal_log_insert(wal_t *wal, txn_id_t txn_id, table_id_t table,
                      uint32_t record_id, const void *new_data,
                      uint16_t data_size)
{
    return wal_append(wal, txn_id, WAL_INSERT, table, record_id,
                      NULL, new_data, data_size);
}

/*
 * wal_log_update — Log an UPDATE operation
 *
 * Both old_data (before image) and new_data (after image) are stored.
 */
lsn_t wal_log_update(wal_t *wal, txn_id_t txn_id, table_id_t table,
                      uint32_t record_id, const void *old_data,
                      const void *new_data, uint16_t data_size)
{
    return wal_append(wal, txn_id, WAL_UPDATE, table, record_id,
                      old_data, new_data, data_size);
}

/*
 * wal_log_delete — Log a DELETE operation
 *
 * old_data = the record being deleted, new_data = zeros (written automatically).
 */
lsn_t wal_log_delete(wal_t *wal, txn_id_t txn_id, table_id_t table,
                      uint32_t record_id, const void *old_data,
                      uint16_t data_size)
{
    return wal_append(wal, txn_id, WAL_DELETE, table, record_id,
                      old_data, NULL, data_size);
}

/*
 * wal_log_commit — Log a COMMIT marker (no data payload)
 */
lsn_t wal_log_commit(wal_t *wal, txn_id_t txn_id)
{
    return wal_append(wal, txn_id, WAL_COMMIT, 0, 0, NULL, NULL, 0);
}

/*
 * wal_log_abort — Log an ABORT marker (no data payload)
 */
lsn_t wal_log_abort(wal_t *wal, txn_id_t txn_id)
{
    return wal_append(wal, txn_id, WAL_ABORT, 0, 0, NULL, NULL, 0);
}

/*
 * wal_log_checkpoint — Log a CHECKPOINT marker
 *
 * Signals that all data prior to this point has been flushed to data files.
 * During recovery, redo can start from the last checkpoint instead of
 * scanning the entire WAL.
 */
lsn_t wal_log_checkpoint(wal_t *wal)
{
    return wal_append(wal, 0, WAL_CHECKPOINT, 0, 0, NULL, NULL, 0);
}

/* ================================================================
 * Durability
 * ================================================================ */

/*
 * wal_flush — Force all pending WAL data to persistent storage
 *
 * Calls fsync() on the WAL file descriptor and updates flushed_lsn.
 * Returns 0 on success, -1 on failure.
 */
int wal_flush(wal_t *wal)
{
    if (!wal || wal->fd < 0) return -1;

    pthread_mutex_lock(&wal->mutex);

    if (fsync(wal->fd) != 0) {
        etp_log(LOG_ERROR, "WAL: fsync failed for '%s': %s",
                wal->filename, strerror(errno));
        pthread_mutex_unlock(&wal->mutex);
        return -1;
    }

    wal->flushed_lsn = wal->next_lsn - 1;

    pthread_mutex_unlock(&wal->mutex);

    etp_log(LOG_DEBUG, "WAL: Flushed to LSN=%lu", (unsigned long)wal->flushed_lsn);
    return 0;
}

/* ================================================================
 * Recovery
 * ================================================================ */

/*
 * wal_recover — Replay all records in the WAL file
 *
 * Opens the WAL file for reading and scans sequentially from the
 * beginning. For each valid record, the callback is invoked with:
 *   - header:   pointer to the record header
 *   - old_data: pointer to old data (NULL if data_size == 0)
 *   - new_data: pointer to new data (NULL if data_size == 0)
 *   - context:  opaque pointer passed through from caller
 *
 * The callback is responsible for redo/undo logic based on whether
 * the transaction committed or aborted.
 *
 * Returns the number of records replayed, or -1 on error.
 */
int wal_recover(wal_t *wal, wal_replay_fn callback, void *context)
{
    if (!wal || !callback) return -1;

    /* Open WAL for sequential reading */
    int read_fd = open(wal->filename, O_RDONLY);
    if (read_fd < 0) {
        etp_log(LOG_ERROR, "WAL: Cannot open '%s' for recovery: %s",
                wal->filename, strerror(errno));
        return -1;
    }

    int record_count = 0;
    wal_record_header_t header;
    void *old_data = NULL;
    void *new_data = NULL;
    ssize_t bytes_read;

    etp_log(LOG_INFO, "WAL: Starting recovery from '%s'", wal->filename);

    while ((bytes_read = read(read_fd, &header, sizeof(header))) == (ssize_t)sizeof(header)) {
        /* Validate basic sanity */
        if (header.lsn == INVALID_LSN) {
            etp_log(LOG_WARN, "WAL: Encountered INVALID_LSN, stopping recovery");
            break;
        }

        /* Read old_data and new_data if present */
        if (header.data_size > 0) {
            old_data = malloc(header.data_size);
            new_data = malloc(header.data_size);
            if (!old_data || !new_data) {
                etp_log(LOG_ERROR, "WAL: Out of memory during recovery (data_size=%u)",
                        header.data_size);
                free(old_data);
                free(new_data);
                close(read_fd);
                return -1;
            }

            /* Read old_data */
            bytes_read = read(read_fd, old_data, header.data_size);
            if (bytes_read != (ssize_t)header.data_size) {
                etp_log(LOG_WARN, "WAL: Truncated old_data at LSN=%lu (expected %u, got %zd)",
                        (unsigned long)header.lsn, header.data_size, bytes_read);
                free(old_data);
                free(new_data);
                break;
            }

            /* Read new_data */
            bytes_read = read(read_fd, new_data, header.data_size);
            if (bytes_read != (ssize_t)header.data_size) {
                etp_log(LOG_WARN, "WAL: Truncated new_data at LSN=%lu (expected %u, got %zd)",
                        (unsigned long)header.lsn, header.data_size, bytes_read);
                free(old_data);
                free(new_data);
                break;
            }
        } else {
            old_data = NULL;
            new_data = NULL;
        }

        /* Invoke the callback */
        callback(&header, old_data, new_data, context);
        record_count++;

        /* Free data buffers */
        if (old_data) { free(old_data); old_data = NULL; }
        if (new_data) { free(new_data); new_data = NULL; }
    }

    close(read_fd);

    /* Handle partial header read (truncated WAL at end) */
    if (bytes_read > 0 && bytes_read < (ssize_t)sizeof(header)) {
        etp_log(LOG_WARN, "WAL: Truncated header at end of WAL (partial %zd bytes)",
                bytes_read);
    }

    etp_log(LOG_INFO, "WAL: Recovery complete — replayed %d records", record_count);
    return record_count;
}

/* ================================================================
 * Stats
 * ================================================================ */

/*
 * wal_get_current_lsn — Return the next LSN that will be assigned
 *
 * This is thread-safe (reads under mutex).
 */
lsn_t wal_get_current_lsn(wal_t *wal)
{
    if (!wal) return INVALID_LSN;

    pthread_mutex_lock(&wal->mutex);
    lsn_t lsn = wal->next_lsn;
    pthread_mutex_unlock(&wal->mutex);

    return lsn;
}
