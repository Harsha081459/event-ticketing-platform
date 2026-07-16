/*
 * ============================================================================
 * Event Ticketing Platform — Shared Memory Stats (Header)
 * ============================================================================
 * Demonstrates System V Shared Memory for real-time server statistics.
 * The server writes stats to a shared memory segment; external monitoring
 * tools can attach and read them without any socket/pipe overhead.
 *
 * OS concepts:
 *   - shmget()    — allocate a shared memory segment
 *   - shmat()     — attach segment to process address space
 *   - shmdt()     — detach segment
 *   - shmctl()    — remove segment on shutdown
 *   - Atomic operations — lock-free stat updates using __atomic builtins
 * ============================================================================
 */

#ifndef ETP_STATS_H
#define ETP_STATS_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Server Statistics — laid out in shared memory
 *
 * This struct is placed directly in the shared memory segment.
 * Multiple processes can read it simultaneously. Updates use
 * atomic operations to avoid tearing.
 * ================================================================ */
typedef struct {
    /* Connection stats */
    uint32_t    active_connections;
    uint32_t    total_connections;

    /* Request stats */
    uint64_t    total_requests;
    uint64_t    failed_requests;

    /* Business stats */
    uint32_t    total_bookings;
    uint32_t    cancelled_bookings;
    uint32_t    total_events;
    uint32_t    active_events;

    /* Resource stats */
    uint32_t    buffer_pool_hits;
    uint32_t    buffer_pool_misses;
    uint32_t    locks_acquired;
    uint32_t    lock_contentions;

    /* Timing */
    int64_t     server_start_time;
    int64_t     last_request_time;
} server_stats_t;

/* Opaque shared memory stats handle */
typedef struct shm_stats shm_stats_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create and attach shared memory segment for stats */
shm_stats_t *shm_stats_create(void);

/* Destroy — detach and remove shared memory segment */
void shm_stats_destroy(shm_stats_t *stats);

/* ================================================================
 * Stat Updates (atomic, lock-free)
 * ================================================================ */
void stats_inc_connections(shm_stats_t *s);
void stats_dec_connections(shm_stats_t *s);
void stats_inc_requests(shm_stats_t *s);
void stats_inc_failed_requests(shm_stats_t *s);
void stats_inc_bookings(shm_stats_t *s);
void stats_inc_cancelled_bookings(shm_stats_t *s);
void stats_inc_events(shm_stats_t *s);
void stats_inc_buffer_hits(shm_stats_t *s);
void stats_inc_buffer_misses(shm_stats_t *s);
void stats_inc_locks(shm_stats_t *s);
void stats_inc_lock_contentions(shm_stats_t *s);
void stats_update_last_request(shm_stats_t *s);

/* ================================================================
 * Stat Reading
 * ================================================================ */

/* Get a snapshot of all stats (copies from shared memory) */
void stats_get_snapshot(shm_stats_t *s, server_stats_t *out);

/* Format stats as a human-readable string */
int stats_format(server_stats_t *stats, char *buf, size_t buf_size);

#endif /* ETP_STATS_H */
