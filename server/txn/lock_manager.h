/*
 * ============================================================================
 * Event Ticketing Platform — Lock Manager (Header)
 * ============================================================================
 * Record-level locking using a hash table of mutex+condvar pairs.
 * Implements Two-Phase Locking (2PL) — locks acquired during a transaction
 * are only released on commit/abort.
 *
 * OS concepts demonstrated:
 *   - pthread_mutex_t      (mutual exclusion)
 *   - pthread_cond_t       (condition variables for waiters)
 *   - Hash-based resource identification
 * ============================================================================
 */

#ifndef ETP_LOCK_MANAGER_H
#define ETP_LOCK_MANAGER_H

#include "../../common/types.h"
#include <stdint.h>

/* Number of lock buckets — prime number for better hash distribution */
#define LOCK_TABLE_SIZE     251

/* Lock modes — alias for lock_type_t defined in types.h */
typedef lock_type_t lock_mode_t;

/* Opaque lock manager handle */
typedef struct lock_manager lock_manager_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
lock_manager_t *lock_mgr_create(void);
void            lock_mgr_destroy(lock_manager_t *lm);

/* ================================================================
 * Lock Operations
 * ================================================================ */

/*
 * Acquire a lock on a specific record. Blocks until the lock is available.
 * Returns 0 on success, -1 on error.
 */
int lock_mgr_lock(lock_manager_t *lm, table_id_t table,
                  uint32_t record_id, lock_mode_t mode);

/*
 * Release a specific lock.
 * Returns 0 on success, -1 if lock was not held.
 */
int lock_mgr_unlock(lock_manager_t *lm, table_id_t table,
                    uint32_t record_id);

/*
 * Release ALL locks — used during transaction commit/abort.
 * Wakes up any threads waiting on the released locks.
 */
void lock_mgr_release_all(lock_manager_t *lm);

#endif /* ETP_LOCK_MANAGER_H */
