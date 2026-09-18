/*
 * ============================================================================
 * Event Ticketing Platform — Transaction Manager (Header)
 * ============================================================================
 * Lightweight transaction lifecycle management. Provides begin/commit/abort
 * semantics backed by the WAL and lock manager.
 *
 * DBMS concepts demonstrated:
 *   - ACID transactions (Atomicity via WAL, Isolation via 2PL)
 *   - Two-Phase Locking protocol
 *   - Write-Ahead Logging for durability
 * ============================================================================
 */

#ifndef ETP_TXN_MANAGER_H
#define ETP_TXN_MANAGER_H

#include "../../common/types.h"
#include "lock_manager.h"
#include "../../server/storage/wal.h"

#include <pthread.h>

/* Maximum concurrent transactions */
#define MAX_ACTIVE_TXNS     128

/* Maximum locks a single transaction can hold (for 2PL tracking) */
#define MAX_LOCKS_PER_TXN   64

/* Transaction states */
typedef enum {
    TXN_ACTIVE      = 0,
    TXN_COMMITTED   = 1,
    TXN_ABORTED     = 2
} txn_state_t;

/* Lock held by a transaction (for release on commit/abort) */
typedef struct {
    table_id_t  table;
    uint32_t    record_id;
} held_lock_t;

/* Transaction descriptor */
typedef struct {
    txn_id_t        id;
    txn_state_t     state;
    held_lock_t     locks[MAX_LOCKS_PER_TXN];
    int             num_locks;
    int             active;         /* 1 if slot is in use */
} txn_t;

/* Transaction Manager */
typedef struct {
    txn_t               txns[MAX_ACTIVE_TXNS];
    txn_id_t            next_txn_id;
    pthread_mutex_t     mutex;
    lock_manager_t     *lock_mgr;   /* Shared lock manager          */
    wal_t              *wal;         /* Shared WAL                   */
} txn_manager_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
txn_manager_t *txn_mgr_create(lock_manager_t *lm, wal_t *wal);
void           txn_mgr_destroy(txn_manager_t *tm);

/* ================================================================
 * Transaction Operations
 * ================================================================ */

/* Begin a new transaction. Returns the transaction ID, or INVALID_TXN_ID on error. */
txn_id_t    txn_begin(txn_manager_t *tm);

/* Commit a transaction — flush WAL, release all locks. */
int         txn_commit(txn_manager_t *tm, txn_id_t txn_id);

/* Abort a transaction — log abort, release all locks. */
int         txn_abort(txn_manager_t *tm, txn_id_t txn_id);

/* ================================================================
 * Lock Integration (2PL)
 *
 * These acquire locks through the lock manager AND register them
 * with the transaction so they can be released on commit/abort.
 * ================================================================ */

/* Acquire a lock on behalf of a transaction. */
int txn_lock(txn_manager_t *tm, txn_id_t txn_id,
             table_id_t table, uint32_t record_id, lock_mode_t mode);

/* Get a transaction descriptor by ID (NULL if not found). */
txn_t *txn_get(txn_manager_t *tm, txn_id_t txn_id);

#endif /* ETP_TXN_MANAGER_H */
