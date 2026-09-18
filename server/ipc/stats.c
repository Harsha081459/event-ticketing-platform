/*
 * ============================================================================
 * Event Ticketing Platform — Shared Memory Stats (Implementation)
 * ============================================================================
 * Uses System V shared memory to expose server statistics in a memory
 * segment that external tools can read without IPC overhead.
 *
 * Memory layout:
 *   ┌──────────────────────────────────────┐
 *   │         server_stats_t               │  ← shmat() maps this into
 *   │   (active_connections, bookings,     │     each attached process
 *   │    buffer pool hits, timing, ...)    │
 *   └──────────────────────────────────────┘
 *
 * All stat updates use GCC __atomic builtins for lock-free thread safety.
 * This means no mutex overhead on the hot path (every request update).
 * ============================================================================
 */

#include "stats.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* ================================================================
 * Stats Handle
 * ================================================================ */
struct shm_stats {
    int              shm_id;     /* Shared memory segment ID       */
    server_stats_t  *data;       /* Pointer to mapped stats struct  */
};

/* ================================================================
 * Public API: Lifecycle
 * ================================================================ */

shm_stats_t *shm_stats_create(void) {
    shm_stats_t *s = calloc(1, sizeof(shm_stats_t));
    if (!s) return NULL;

    /*
     * Create a shared memory segment.
     * shmget(key, size, flags):
     *   - ETP_SHM_KEY: unique key for our application
     *   - size: sizeof(server_stats_t)
     *   - IPC_CREAT | 0666: create if new, read/write for all
     */
    s->shm_id = shmget(ETP_SHM_KEY, sizeof(server_stats_t),
                        IPC_CREAT | 0666);
    if (s->shm_id < 0) {
        etp_log(LOG_ERROR, "shm_stats: shmget() failed: %s", strerror(errno));
        free(s);
        return NULL;
    }

    /*
     * Attach the shared memory segment to our address space.
     * shmat() returns a pointer we can read/write directly.
     * The kernel handles page mapping — same physical pages are
     * visible to all attached processes.
     */
    s->data = (server_stats_t *)shmat(s->shm_id, NULL, 0);
    if (s->data == (server_stats_t *)-1) {
        etp_log(LOG_ERROR, "shm_stats: shmat() failed: %s", strerror(errno));
        shmctl(s->shm_id, IPC_RMID, NULL);
        free(s);
        return NULL;
    }

    /* Initialize stats to zero */
    memset(s->data, 0, sizeof(server_stats_t));
    s->data->server_start_time = etp_get_timestamp();

    etp_log(LOG_INFO, "shm_stats: created (shm_id=%d, key=0x%X, size=%zu bytes)",
            s->shm_id, ETP_SHM_KEY, sizeof(server_stats_t));
    return s;
}

void shm_stats_destroy(shm_stats_t *s) {
    if (!s) return;

    /* Detach from our address space */
    if (s->data && s->data != (server_stats_t *)-1) {
        shmdt(s->data);
    }

    /* Remove the shared memory segment (frees kernel resources) */
    if (s->shm_id >= 0) {
        shmctl(s->shm_id, IPC_RMID, NULL);
    }

    free(s);
    etp_log(LOG_INFO, "shm_stats: destroyed");
}

/* ================================================================
 * Atomic Stat Updates
 *
 * Using __atomic_add_fetch / __atomic_sub_fetch with
 * __ATOMIC_RELAXED ordering. This is sufficient because:
 *   - Each counter is independent
 *   - We don't need happens-before relationships between counters
 *   - Relaxed ordering is the cheapest on x86 (no fence needed)
 * ================================================================ */

void stats_inc_connections(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->active_connections, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->data->total_connections, 1, __ATOMIC_RELAXED);
}

void stats_dec_connections(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_sub_fetch(&s->data->active_connections, 1, __ATOMIC_RELAXED);
}

void stats_inc_requests(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->total_requests, 1, __ATOMIC_RELAXED);
}

void stats_inc_failed_requests(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->failed_requests, 1, __ATOMIC_RELAXED);
}

void stats_inc_bookings(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->total_bookings, 1, __ATOMIC_RELAXED);
}

void stats_inc_cancelled_bookings(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->cancelled_bookings, 1, __ATOMIC_RELAXED);
}

void stats_inc_events(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->total_events, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->data->active_events, 1, __ATOMIC_RELAXED);
}

void stats_inc_buffer_hits(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->buffer_pool_hits, 1, __ATOMIC_RELAXED);
}

void stats_inc_buffer_misses(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->buffer_pool_misses, 1, __ATOMIC_RELAXED);
}

void stats_inc_locks(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->locks_acquired, 1, __ATOMIC_RELAXED);
}

void stats_inc_lock_contentions(shm_stats_t *s) {
    if (!s || !s->data) return;
    __atomic_add_fetch(&s->data->lock_contentions, 1, __ATOMIC_RELAXED);
}

void stats_update_last_request(shm_stats_t *s) {
    if (!s || !s->data) return;
    int64_t now = etp_get_timestamp();
    __atomic_store(&s->data->last_request_time, &now, __ATOMIC_RELAXED);
}

/* ================================================================
 * Stat Reading
 * ================================================================ */

void stats_get_snapshot(shm_stats_t *s, server_stats_t *out) {
    if (!s || !s->data || !out) return;
    /* Simple memcpy — for a consistent snapshot, you'd want a seqlock.
     * For monitoring/display purposes, a slightly stale read is fine. */
    memcpy(out, s->data, sizeof(server_stats_t));
}

int stats_format(server_stats_t *stats, char *buf, size_t buf_size) {
    if (!stats || !buf) return -1;

    int64_t now = etp_get_timestamp();
    int64_t uptime_secs = now - stats->server_start_time;
    int hours = (int)(uptime_secs / 3600);
    int mins  = (int)((uptime_secs % 3600) / 60);
    int secs  = (int)(uptime_secs % 60);

    return snprintf(buf, buf_size,
        "╔════════════════════════════════════════╗\n"
        "║        Server Statistics               ║\n"
        "╠════════════════════════════════════════╣\n"
        "║ Uptime:          %02d:%02d:%02d             ║\n"
        "║ Connections:     %u active / %u total   \n"
        "║ Requests:        %lu total / %lu failed  \n"
        "║ Bookings:        %u confirmed / %u cancelled\n"
        "║ Events:          %u active / %u total   \n"
        "║ Buffer Pool:     %u hits / %u misses    \n"
        "║ Locks:           %u acquired / %u contentions\n"
        "╚════════════════════════════════════════╝\n",
        hours, mins, secs,
        stats->active_connections, stats->total_connections,
        (unsigned long)stats->total_requests, (unsigned long)stats->failed_requests,
        stats->total_bookings, stats->cancelled_bookings,
        stats->active_events, stats->total_events,
        stats->buffer_pool_hits, stats->buffer_pool_misses,
        stats->locks_acquired, stats->lock_contentions);
}
