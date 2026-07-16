/*
 * ============================================================================
 * Event Ticketing Platform — Transaction Manager (Implementation)
 * ============================================================================
 * Manages the lifecycle of ACID transactions:
 *   - begin:  allocate a txn slot, assign txn_id
 *   - lock:   acquire lock via lock_manager, register with txn (2PL growing phase)
 *   - commit: write COMMIT to WAL, flush, release all locks (2PL shrinking phase)
 *   - abort:  write ABORT to WAL, release all locks
 *
 * Two-Phase Locking (2PL) guarantee:
 *   - Growing phase: locks are acquired (via txn_lock) but never released
 *   - Shrinking phase: on commit/abort, ALL locks are released at once
 *   - This prevents cascading rollbacks and ensures serializability
 * ============================================================================
 */

#include "txn_manager.h"
#include "../../common/utils.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Lifecycle
 * ================================================================ */

txn_manager_t *txn_mgr_create(lock_manager_t *lm, wal_t *wal) {
    if (!lm || !wal) {
        etp_log(LOG_ERROR, "txn_mgr_create: NULL lock_manager or WAL");
        return NULL;
    }

    txn_manager_t *tm = calloc(1, sizeof(txn_manager_t));
    if (!tm) return NULL;

    tm->lock_mgr    = lm;
    tm->wal         = wal;
    tm->next_txn_id = 1;
    pthread_mutex_init(&tm->mutex, NULL);

    /* Mark all slots as inactive */
    for (int i = 0; i < MAX_ACTIVE_TXNS; i++) {
        tm->txns[i].active = 0;
    }

    etp_log(LOG_INFO, "txn_mgr: created (capacity=%d concurrent txns)", MAX_ACTIVE_TXNS);
    return tm;
}

void txn_mgr_destroy(txn_manager_t *tm) {
    if (!tm) return;

    /* Abort any still-active transactions */
    for (int i = 0; i < MAX_ACTIVE_TXNS; i++) {
        if (tm->txns[i].active && tm->txns[i].state == TXN_ACTIVE) {
            etp_log(LOG_WARN, "txn_mgr: force-aborting txn %u on shutdown",
                    tm->txns[i].id);
            txn_abort(tm, tm->txns[i].id);
        }
    }

    pthread_mutex_destroy(&tm->mutex);
    free(tm);
    etp_log(LOG_INFO, "txn_mgr: destroyed");
}

/* ================================================================
 * Internal: Find slot by txn_id
 * (Caller must hold tm->mutex)
 * ================================================================ */
static txn_t *find_txn_locked(txn_manager_t *tm, txn_id_t txn_id) {
    for (int i = 0; i < MAX_ACTIVE_TXNS; i++) {
        if (tm->txns[i].active && tm->txns[i].id == txn_id) {
            return &tm->txns[i];
        }
    }
    return NULL;
}

/* ================================================================
 * Internal: Release all locks held by a transaction
 * (Called during commit/abort — the 2PL shrinking phase)
 * ================================================================ */
/* NOTE: release_txn_locks was removed. Lock release is now done
 * inline in txn_commit/txn_abort AFTER releasing tm->mutex to
 * prevent ABBA deadlocks between tm->mutex and bucket->mutex. */

/* ================================================================
 * Public API: Transaction Operations
 * ================================================================ */

txn_id_t txn_begin(txn_manager_t *tm) {
    if (!tm) return INVALID_TXN_ID;

    pthread_mutex_lock(&tm->mutex);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_TXNS; i++) {
        if (!tm->txns[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&tm->mutex);
        etp_log(LOG_ERROR, "txn_mgr: no free transaction slots");
        return INVALID_TXN_ID;
    }

    txn_t *txn     = &tm->txns[slot];
    txn->id        = tm->next_txn_id++;
    /* Guard against wraparound to INVALID_TXN_ID (0) */
    if (tm->next_txn_id == INVALID_TXN_ID) tm->next_txn_id = 1;
    txn->state     = TXN_ACTIVE;
    txn->num_locks = 0;
    txn->active    = 1;

    etp_log(LOG_DEBUG, "txn_mgr: BEGIN txn %u (slot=%d)", txn->id, slot);

    pthread_mutex_unlock(&tm->mutex);
    return txn->id;
}

int txn_commit(txn_manager_t *tm, txn_id_t txn_id) {
    if (!tm) return -1;

    pthread_mutex_lock(&tm->mutex);

    txn_t *txn = find_txn_locked(tm, txn_id);
    if (!txn || txn->state != TXN_ACTIVE) {
        pthread_mutex_unlock(&tm->mutex);
        etp_log(LOG_ERROR, "txn_mgr: cannot commit txn %u (not found or not active)",
                txn_id);
        return -1;
    }

    /* Write COMMIT record to WAL */
    lsn_t lsn = wal_log_commit(tm->wal, txn_id);
    if (lsn == INVALID_LSN) {
        pthread_mutex_unlock(&tm->mutex);
        etp_log(LOG_ERROR, "txn_mgr: WAL commit failed for txn %u", txn_id);
        return -1;
    }

    /* Flush WAL to ensure durability */
    wal_flush(tm->wal);

    /* Copy lock info before releasing tm->mutex.
     * IMPORTANT: We must NOT hold tm->mutex while calling lock_mgr_unlock,
     * because lock_mgr_unlock locks bucket->mutex. If another thread holds
     * bucket->mutex (blocked in cond_wait inside lock_mgr_lock) and then
     * tries to acquire tm->mutex, we get a classic ABBA deadlock. */
    held_lock_t locks_copy[MAX_LOCKS_PER_TXN];
    int num_locks = txn->num_locks;
    memcpy(locks_copy, txn->locks, num_locks * sizeof(held_lock_t));

    txn->state     = TXN_COMMITTED;
    txn->num_locks = 0;
    txn->active    = 0;

    pthread_mutex_unlock(&tm->mutex);

    /* Release all locks OUTSIDE tm->mutex (2PL shrinking phase) */
    for (int i = 0; i < num_locks; i++) {
        lock_mgr_unlock(tm->lock_mgr, locks_copy[i].table,
                        locks_copy[i].record_id);
    }
    etp_log(LOG_DEBUG, "txn_mgr: COMMIT txn %u (LSN=%lu, released %d locks)",
            txn_id, (unsigned long)lsn, num_locks);

    return 0;
}

int txn_abort(txn_manager_t *tm, txn_id_t txn_id) {
    if (!tm) return -1;

    pthread_mutex_lock(&tm->mutex);

    txn_t *txn = find_txn_locked(tm, txn_id);
    if (!txn || txn->state != TXN_ACTIVE) {
        pthread_mutex_unlock(&tm->mutex);
        return -1;
    }

    /* Write ABORT record to WAL */
    wal_log_abort(tm->wal, txn_id);

    /* Copy lock info before releasing tm->mutex (same deadlock prevention
     * as txn_commit — see comment there) */
    held_lock_t locks_copy[MAX_LOCKS_PER_TXN];
    int num_locks = txn->num_locks;
    memcpy(locks_copy, txn->locks, num_locks * sizeof(held_lock_t));

    txn->state     = TXN_ABORTED;
    txn->num_locks = 0;
    txn->active    = 0;

    pthread_mutex_unlock(&tm->mutex);

    /* Release all locks OUTSIDE tm->mutex */
    for (int i = 0; i < num_locks; i++) {
        lock_mgr_unlock(tm->lock_mgr, locks_copy[i].table,
                        locks_copy[i].record_id);
    }
    etp_log(LOG_DEBUG, "txn_mgr: ABORT txn %u (released %d locks)",
            txn_id, num_locks);

    return 0;
}

/* ================================================================
 * Public API: Lock Integration (2PL)
 * ================================================================ */

int txn_lock(txn_manager_t *tm, txn_id_t txn_id,
             table_id_t table, uint32_t record_id, lock_mode_t mode) {
    if (!tm) return -1;

    pthread_mutex_lock(&tm->mutex);

    txn_t *txn = find_txn_locked(tm, txn_id);
    if (!txn || txn->state != TXN_ACTIVE) {
        pthread_mutex_unlock(&tm->mutex);
        return -1;
    }

    /* Check if we already hold this lock */
    for (int i = 0; i < txn->num_locks; i++) {
        if (txn->locks[i].table == table &&
            txn->locks[i].record_id == record_id) {
            pthread_mutex_unlock(&tm->mutex);
            return 0;  /* Already holding — skip */
        }
    }

    if (txn->num_locks >= MAX_LOCKS_PER_TXN) {
        pthread_mutex_unlock(&tm->mutex);
        etp_log(LOG_ERROR, "txn_mgr: txn %u exceeded max locks (%d)",
                txn_id, MAX_LOCKS_PER_TXN);
        return -1;
    }

    pthread_mutex_unlock(&tm->mutex);

    /* Acquire lock through the lock manager (may block!) */
    int rc = lock_mgr_lock(tm->lock_mgr, table, record_id, mode);
    if (rc != 0) return -1;

    /* Register the lock with the transaction.
     * While we were blocked in lock_mgr_lock, the txn could have been
     * aborted by another thread. If so, we must release the lock we
     * just acquired to prevent a permanent lock leak. */
    pthread_mutex_lock(&tm->mutex);
    txn = find_txn_locked(tm, txn_id);
    if (txn && txn->state == TXN_ACTIVE) {
        txn->locks[txn->num_locks].table     = table;
        txn->locks[txn->num_locks].record_id = record_id;
        txn->num_locks++;
        pthread_mutex_unlock(&tm->mutex);
        return 0;
    }

    /* Transaction was aborted while we waited — release the orphaned lock */
    pthread_mutex_unlock(&tm->mutex);
    lock_mgr_unlock(tm->lock_mgr, table, record_id);
    etp_log(LOG_WARN, "txn_mgr: txn %u aborted during lock acquisition, released orphaned lock",
            txn_id);
    return -1;
}

txn_t *txn_get(txn_manager_t *tm, txn_id_t txn_id) {
    if (!tm) return NULL;

    pthread_mutex_lock(&tm->mutex);
    txn_t *txn = find_txn_locked(tm, txn_id);
    pthread_mutex_unlock(&tm->mutex);
    return txn;
}
