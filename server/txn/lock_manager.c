/*
 * ============================================================================
 * Event Ticketing Platform — Lock Manager (Implementation)
 * ============================================================================
 * Hash-table based record locking with shared/exclusive modes.
 *
 * Design:
 *   - Fixed array of LOCK_TABLE_SIZE buckets, each with a linked list
 *   - Hash key = (table_id * 10007 + record_id) % LOCK_TABLE_SIZE
 *   - Each lock entry: table_id, record_id, mode, shared_count, exclusive_held
 *   - A per-bucket mutex protects the linked list
 *   - A per-entry condition variable handles waiters
 *
 * Compatibility matrix:
 *   ┌──────────────┬──────────┬───────────┐
 *   │  Request \Held│  SHARED  │ EXCLUSIVE │
 *   ├──────────────┼──────────┼───────────┤
 *   │  SHARED      │    ✓     │     ✗     │
 *   │  EXCLUSIVE   │    ✗     │     ✗     │
 *   └──────────────┴──────────┴───────────┘
 * ============================================================================
 */

#include "lock_manager.h"
#include "../../common/utils.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ================================================================
 * Internal: Lock Entry (linked list node within a bucket)
 * ================================================================ */
typedef struct lock_entry {
    table_id_t          table;
    uint32_t            record_id;
    lock_mode_t         mode;           /* Current granted mode          */
    int                 shared_count;   /* Number of shared holders      */
    int                 exclusive_held; /* 1 if exclusively held         */
    int                 waiters;        /* Number of threads waiting     */
    pthread_cond_t      cond;           /* Waiters sleep here            */
    struct lock_entry  *next;           /* Next in bucket chain          */
} lock_entry_t;

/* ================================================================
 * Internal: Bucket (one per hash slot)
 * ================================================================ */
typedef struct {
    lock_entry_t   *head;
    pthread_mutex_t mutex;
} lock_bucket_t;

/* ================================================================
 * Lock Manager Structure
 * ================================================================ */
struct lock_manager {
    lock_bucket_t buckets[LOCK_TABLE_SIZE];
};

/* ================================================================
 * Internal: Hash Function
 * ================================================================ */
static inline uint32_t lock_hash(table_id_t table, uint32_t record_id) {
    return ((uint32_t)table * 10007u + record_id) % LOCK_TABLE_SIZE;
}

/* ================================================================
 * Internal: Find or create a lock entry in a bucket
 * (Caller must hold bucket mutex)
 * ================================================================ */
static lock_entry_t *find_entry(lock_bucket_t *bucket, table_id_t table,
                                 uint32_t record_id) {
    lock_entry_t *e = bucket->head;
    while (e) {
        if (e->table == table && e->record_id == record_id) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

static lock_entry_t *create_entry(lock_bucket_t *bucket, table_id_t table,
                                   uint32_t record_id) {
    lock_entry_t *e = calloc(1, sizeof(lock_entry_t));
    if (!e) return NULL;

    e->table          = table;
    e->record_id      = record_id;
    e->shared_count   = 0;
    e->exclusive_held = 0;
    e->waiters        = 0;
    pthread_cond_init(&e->cond, NULL);

    /* Prepend to bucket list */
    e->next       = bucket->head;
    bucket->head  = e;

    return e;
}

/* ================================================================
 * Internal: Remove entry from bucket if no holders or waiters
 * (Caller must hold bucket mutex)
 * ================================================================ */
static void try_remove_entry(lock_bucket_t *bucket, lock_entry_t *target) {
    if (target->shared_count > 0 || target->exclusive_held || target->waiters > 0) {
        return;  /* Still in use */
    }

    lock_entry_t **pp = &bucket->head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            pthread_cond_destroy(&target->cond);
            free(target);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

lock_manager_t *lock_mgr_create(void) {
    lock_manager_t *lm = calloc(1, sizeof(lock_manager_t));
    if (!lm) return NULL;

    for (int i = 0; i < LOCK_TABLE_SIZE; i++) {
        lm->buckets[i].head = NULL;
        pthread_mutex_init(&lm->buckets[i].mutex, NULL);
    }

    etp_log(LOG_INFO, "lock_mgr: created (%d buckets)", LOCK_TABLE_SIZE);
    return lm;
}

void lock_mgr_destroy(lock_manager_t *lm) {
    if (!lm) return;

    for (int i = 0; i < LOCK_TABLE_SIZE; i++) {
        lock_entry_t *e = lm->buckets[i].head;
        while (e) {
            lock_entry_t *next = e->next;
            pthread_cond_destroy(&e->cond);
            free(e);
            e = next;
        }
        pthread_mutex_destroy(&lm->buckets[i].mutex);
    }

    free(lm);
    etp_log(LOG_INFO, "lock_mgr: destroyed");
}

int lock_mgr_lock(lock_manager_t *lm, table_id_t table,
                  uint32_t record_id, lock_mode_t mode) {
    if (!lm) return -1;

    uint32_t idx = lock_hash(table, record_id);
    lock_bucket_t *bucket = &lm->buckets[idx];

    pthread_mutex_lock(&bucket->mutex);

    /* Find or create the lock entry */
    lock_entry_t *entry = find_entry(bucket, table, record_id);
    if (!entry) {
        entry = create_entry(bucket, table, record_id);
        if (!entry) {
            pthread_mutex_unlock(&bucket->mutex);
            return -1;
        }
    }

    /* Wait until the lock is compatible */
    if (mode == LOCK_SHARED) {
        /* SHARED: wait while an exclusive lock is held */
        while (entry->exclusive_held) {
            entry->waiters++;
            etp_log(LOG_DEBUG, "lock_mgr: waiting for SHARED lock on (%d, %u)",
                    table, record_id);
            pthread_cond_wait(&entry->cond, &bucket->mutex);
            entry->waiters--;
        }
        entry->shared_count++;
        entry->mode = LOCK_SHARED;
    } else {
        /* EXCLUSIVE: wait while any lock is held (shared or exclusive) */
        while (entry->exclusive_held || entry->shared_count > 0) {
            entry->waiters++;
            etp_log(LOG_DEBUG, "lock_mgr: waiting for EXCLUSIVE lock on (%d, %u)",
                    table, record_id);
            pthread_cond_wait(&entry->cond, &bucket->mutex);
            entry->waiters--;
        }
        entry->exclusive_held = 1;
        entry->mode = LOCK_EXCLUSIVE;
    }

    pthread_mutex_unlock(&bucket->mutex);

    etp_log(LOG_DEBUG, "lock_mgr: acquired %s lock on (%d, %u)",
            mode == LOCK_SHARED ? "SHARED" : "EXCLUSIVE",
            table, record_id);
    return 0;
}

int lock_mgr_unlock(lock_manager_t *lm, table_id_t table,
                    uint32_t record_id) {
    if (!lm) return -1;

    uint32_t idx = lock_hash(table, record_id);
    lock_bucket_t *bucket = &lm->buckets[idx];

    pthread_mutex_lock(&bucket->mutex);

    lock_entry_t *entry = find_entry(bucket, table, record_id);
    if (!entry) {
        pthread_mutex_unlock(&bucket->mutex);
        return -1;  /* Lock not found */
    }

    if (entry->exclusive_held) {
        entry->exclusive_held = 0;
    } else if (entry->shared_count > 0) {
        entry->shared_count--;
    } else {
        pthread_mutex_unlock(&bucket->mutex);
        return -1;  /* Nothing to unlock */
    }

    /* Wake up waiters */
    pthread_cond_broadcast(&entry->cond);

    /* Cleanup entry if no longer needed */
    try_remove_entry(bucket, entry);

    pthread_mutex_unlock(&bucket->mutex);

    etp_log(LOG_DEBUG, "lock_mgr: released lock on (%d, %u)", table, record_id);
    return 0;
}

void lock_mgr_release_all(lock_manager_t *lm) {
    if (!lm) return;

    for (int i = 0; i < LOCK_TABLE_SIZE; i++) {
        lock_bucket_t *bucket = &lm->buckets[i];
        pthread_mutex_lock(&bucket->mutex);

        lock_entry_t *e = bucket->head;
        while (e) {
            lock_entry_t *next = e->next;
            e->shared_count   = 0;
            e->exclusive_held = 0;
            pthread_cond_broadcast(&e->cond);
            try_remove_entry(bucket, e);
            e = next;
        }

        pthread_mutex_unlock(&bucket->mutex);
    }

    etp_log(LOG_DEBUG, "lock_mgr: released all locks");
}
