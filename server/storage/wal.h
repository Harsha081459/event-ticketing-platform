/*
 * ============================================================================
 * Event Ticketing Platform — Write-Ahead Log (WAL)
 * ============================================================================
 * Every mutation (insert/update/delete) is logged to the WAL BEFORE being
 * applied to data files. On crash recovery, the WAL is replayed to restore
 * a consistent state.
 *
 * WAL is an append-only file. Records are written sequentially with
 * monotonically increasing Log Sequence Numbers (LSNs).
 * ============================================================================
 */

#ifndef ETP_WAL_H
#define ETP_WAL_H

#include <stdint.h>
#include <pthread.h>
#include "../../common/types.h"

/* ================================================================
 * WAL Record Header — on-disk format (packed)
 *
 * Followed by:
 *   old_data (data_size bytes)  then  new_data (data_size bytes)
 *
 * INSERT:     old_data = zeros,           new_data = inserted record
 * DELETE:     old_data = deleted record,  new_data = zeros
 * UPDATE:     old_data = before image,    new_data = after image
 * COMMIT/ABORT/CHECKPOINT: data_size = 0, no data follows
 * ================================================================ */
typedef struct __attribute__((packed)) {
    lsn_t           lsn;            /* Log Sequence Number (monotonically increasing)  */
    txn_id_t        txn_id;         /* Transaction that made this change               */
    uint8_t         op_type;        /* wal_op_type_t: INSERT, UPDATE, DELETE, etc.      */
    uint8_t         table_id;       /* table_id_t: which table is affected              */
    uint16_t        data_size;      /* Size of old_data / new_data (0 for COMMIT/ABORT) */
    uint32_t        record_id;      /* Primary key of the affected record               */
} wal_record_header_t;

/* ================================================================
 * WAL Handle
 * ================================================================ */
typedef struct {
    int             fd;             /* File descriptor for WAL file     */
    lsn_t           next_lsn;       /* Next LSN to assign               */
    lsn_t           flushed_lsn;    /* Last LSN that was fsync'd        */
    pthread_mutex_t mutex;          /* Thread safety                    */
    char            filename[256];  /* Path to the WAL file             */
} wal_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
wal_t  *wal_create(const char *filename);   /* Create new WAL file    */
wal_t  *wal_open(const char *filename);     /* Open existing WAL      */
void    wal_close(wal_t *wal);              /* Flush and close        */

/* ================================================================
 * Logging Operations
 * ================================================================ */
lsn_t   wal_log_insert(wal_t *wal, txn_id_t txn_id, table_id_t table,
                        uint32_t record_id, const void *new_data,
                        uint16_t data_size);

lsn_t   wal_log_update(wal_t *wal, txn_id_t txn_id, table_id_t table,
                        uint32_t record_id, const void *old_data,
                        const void *new_data, uint16_t data_size);

lsn_t   wal_log_delete(wal_t *wal, txn_id_t txn_id, table_id_t table,
                        uint32_t record_id, const void *old_data,
                        uint16_t data_size);

lsn_t   wal_log_commit(wal_t *wal, txn_id_t txn_id);
lsn_t   wal_log_abort(wal_t *wal, txn_id_t txn_id);
lsn_t   wal_log_checkpoint(wal_t *wal);

/* ================================================================
 * Durability
 * ================================================================ */
int     wal_flush(wal_t *wal);              /* fsync WAL to disk      */

/* ================================================================
 * Recovery
 *
 * Scans the entire WAL file sequentially, calling `callback` for
 * each valid record. The caller decides how to redo/undo based on
 * COMMIT/ABORT status.
 * ================================================================ */
typedef void (*wal_replay_fn)(const wal_record_header_t *header,
                               const void *old_data, const void *new_data,
                               void *context);

int     wal_recover(wal_t *wal, wal_replay_fn callback, void *context);

/* ================================================================
 * Stats
 * ================================================================ */
lsn_t   wal_get_current_lsn(wal_t *wal);

#endif /* ETP_WAL_H */
