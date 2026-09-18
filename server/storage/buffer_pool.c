/*
 * ============================================================================
 * Event Ticketing Platform — Buffer Pool Implementation
 * ============================================================================
 * In-memory page cache with LRU eviction. Sits between the table/index
 * layer and the page-level disk I/O.
 *
 * Design overview
 * ---------------
 *   Frames       — array of `num_frames` page-sized buffers + metadata.
 *   Hash map     — open-addressed table mapping (fd, page_id) → frame index
 *                  for O(1) lookups.  Size = num_frames * 4 to keep load
 *                  factor ≤ 0.25.
 *   LRU list     — doubly-linked list threaded through the frame array.
 *                  Tail = least recently used, Head = most recently used.
 *   Mutex        — single pthread_mutex protects all pool state.
 *
 * Eviction policy
 * ---------------
 *   On a cache miss with no free frames, walk the LRU list from the tail
 *   and pick the first frame with pin_count == 0.  If that frame is dirty,
 *   flush it to disk before reuse.  If every frame is pinned, return NULL.
 * ============================================================================
 */

#include "buffer_pool.h"
#include "page.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ================================================================
 * Internal Constants
 * ================================================================ */

/* Sentinel index meaning "no frame" (linked-list terminators, empty slots) */
#define FRAME_NONE  ((uint32_t)0xFFFFFFFF)

/* Hash map load-factor multiplier (capacity = num_frames * MAP_FACTOR) */
#define MAP_FACTOR  4

/* ================================================================
 * Frame Metadata — one per buffer frame
 * ================================================================ */
typedef struct {
    uint8_t     page_buf[PAGE_SIZE];    /* The actual page data              */

    /* Identification */
    int         fd;                     /* File descriptor of the source     */
    page_id_t   page_id;               /* Which page from that file         */

    /* State */
    uint32_t    pin_count;              /* >0 ⇒ frame is in use             */
    uint8_t     is_dirty;               /* 1 ⇒ needs flush before eviction  */
    uint8_t     is_valid;               /* 1 ⇒ frame holds a loaded page    */

    /* Doubly-linked LRU list pointers (indices into frames array) */
    uint32_t    lru_prev;
    uint32_t    lru_next;
} frame_t;

/* ================================================================
 * Hash Map Bucket — open-addressed (linear probing)
 * ================================================================ */
typedef struct {
    int         fd;                     /* Key part 1                        */
    page_id_t   page_id;               /* Key part 2                        */
    uint32_t    frame_idx;              /* Value — index into frames[]       */
    uint8_t     occupied;               /* 1 ⇒ bucket is in use             */
} hash_bucket_t;

/* ================================================================
 * Buffer Pool Structure
 * ================================================================ */
struct buffer_pool {
    /* Frame storage */
    frame_t        *frames;             /* Array of num_frames frames        */
    uint32_t        num_frames;         /* Total number of frames            */

    /* Hash map for (fd, page_id) → frame_idx lookups */
    hash_bucket_t  *map;                /* Array of map_capacity buckets     */
    uint32_t        map_capacity;       /* num_frames * MAP_FACTOR           */

    /* LRU doubly-linked list (indices into frames[]) */
    uint32_t        lru_head;           /* Most recently used                */
    uint32_t        lru_tail;           /* Least recently used               */

    /* Statistics */
    uint32_t        hits;
    uint32_t        misses;

    /* Thread safety */
    pthread_mutex_t lock;
};

/* ================================================================
 * Hash Map Helpers
 * ================================================================ */

/* Primary hash — deterministic, fast, reasonable distribution */
static inline uint32_t _hash(int fd, page_id_t page_id, uint32_t capacity)
{
    return ((uint32_t)fd * 31u + page_id) % capacity;
}

/*
 * _map_find — Locate the bucket holding (fd, page_id).
 * Returns the bucket index, or FRAME_NONE if not found.
 * Uses linear probing with wrap-around.
 */
static uint32_t _map_find(const buffer_pool_t *pool, int fd, page_id_t page_id)
{
    uint32_t idx = _hash(fd, page_id, pool->map_capacity);

    for (uint32_t i = 0; i < pool->map_capacity; i++) {
        uint32_t probe = (idx + i) % pool->map_capacity;
        const hash_bucket_t *b = &pool->map[probe];

        if (!b->occupied) {
            return FRAME_NONE;  /* Empty slot ⇒ key not present */
        }
        if (b->fd == fd && b->page_id == page_id) {
            return probe;
        }
    }
    return FRAME_NONE;  /* Full scan, not found (shouldn't happen) */
}

/*
 * _map_insert — Insert (fd, page_id) → frame_idx into the hash map.
 * Assumes there is room (load factor is kept low).
 */
static void _map_insert(buffer_pool_t *pool, int fd, page_id_t page_id,
                         uint32_t frame_idx)
{
    uint32_t idx = _hash(fd, page_id, pool->map_capacity);

    for (uint32_t i = 0; i < pool->map_capacity; i++) {
        uint32_t probe = (idx + i) % pool->map_capacity;
        hash_bucket_t *b = &pool->map[probe];

        if (!b->occupied) {
            b->fd        = fd;
            b->page_id   = page_id;
            b->frame_idx = frame_idx;
            b->occupied  = 1;
            return;
        }
    }
    /* Should never reach here with MAP_FACTOR = 4 */
    etp_log(LOG_ERROR, "buffer_pool: hash map full — this should not happen");
}

/*
 * _map_remove — Remove the bucket for (fd, page_id).
 * After removal, re-insert all subsequent contiguous entries to maintain
 * linear-probing invariants.
 */
static void _map_remove(buffer_pool_t *pool, int fd, page_id_t page_id)
{
    uint32_t bucket_idx = _map_find(pool, fd, page_id);
    if (bucket_idx == FRAME_NONE) return;

    /* Clear the bucket */
    pool->map[bucket_idx].occupied = 0;

    /* Re-hash subsequent contiguous entries (backward-shift deletion) */
    uint32_t probe = (bucket_idx + 1) % pool->map_capacity;
    while (pool->map[probe].occupied) {
        hash_bucket_t tmp = pool->map[probe];
        pool->map[probe].occupied = 0;
        _map_insert(pool, tmp.fd, tmp.page_id, tmp.frame_idx);
        probe = (probe + 1) % pool->map_capacity;
    }
}

/* ================================================================
 * LRU List Helpers
 *
 * The list is ordered from head (MRU) to tail (LRU).
 * Only VALID frames participate in the list.
 * ================================================================ */

/* Remove frame_idx from wherever it sits in the LRU list */
static void _lru_remove(buffer_pool_t *pool, uint32_t frame_idx)
{
    frame_t *f = &pool->frames[frame_idx];

    if (f->lru_prev != FRAME_NONE)
        pool->frames[f->lru_prev].lru_next = f->lru_next;
    else
        pool->lru_head = f->lru_next;  /* Was head */

    if (f->lru_next != FRAME_NONE)
        pool->frames[f->lru_next].lru_prev = f->lru_prev;
    else
        pool->lru_tail = f->lru_prev;  /* Was tail */

    f->lru_prev = FRAME_NONE;
    f->lru_next = FRAME_NONE;
}

/* Push frame_idx to the head (MRU position) of the LRU list */
static void _lru_push_front(buffer_pool_t *pool, uint32_t frame_idx)
{
    frame_t *f = &pool->frames[frame_idx];
    f->lru_prev = FRAME_NONE;
    f->lru_next = pool->lru_head;

    if (pool->lru_head != FRAME_NONE)
        pool->frames[pool->lru_head].lru_prev = frame_idx;
    pool->lru_head = frame_idx;

    if (pool->lru_tail == FRAME_NONE)
        pool->lru_tail = frame_idx;
}

/* Move frame_idx to MRU position (remove + push front) */
static void _lru_touch(buffer_pool_t *pool, uint32_t frame_idx)
{
    _lru_remove(pool, frame_idx);
    _lru_push_front(pool, frame_idx);
}

/* ================================================================
 * Internal: find a free or evictable frame
 * ================================================================ */

/*
 * _find_victim — Return the index of a frame that can be reused.
 *   1. First scan for an invalid (unused) frame.
 *   2. Then walk the LRU list from the tail for an unpinned frame.
 *   3. If a dirty victim is found, flush it to disk.
 *   4. Returns FRAME_NONE if every frame is pinned.
 */
static uint32_t _find_victim(buffer_pool_t *pool)
{
    /* Pass 1: find an entirely unused frame */
    for (uint32_t i = 0; i < pool->num_frames; i++) {
        if (!pool->frames[i].is_valid) {
            return i;
        }
    }

    /* Pass 2: walk LRU from tail (least recently used) */
    uint32_t cur = pool->lru_tail;
    while (cur != FRAME_NONE) {
        frame_t *f = &pool->frames[cur];
        if (f->pin_count == 0) {
            /* Found an unpinned victim */
            if (f->is_dirty) {
                /* Flush before eviction */
                if (page_write(f->fd, f->page_id, f->page_buf) != 0) {
                    etp_log(LOG_ERROR, "buffer_pool: failed to flush dirty "
                            "page %u (fd=%d) during eviction", f->page_id, f->fd);
                    /* Continue anyway — data may be lost */
                }
                f->is_dirty = 0;
            }
            /* Remove from hash map and LRU list */
            _map_remove(pool, f->fd, f->page_id);
            _lru_remove(pool, cur);
            f->is_valid = 0;

            etp_log(LOG_DEBUG, "buffer_pool: evicted page %u (fd=%d) from "
                    "frame %u", f->page_id, f->fd, cur);
            return cur;
        }
        cur = f->lru_prev;  /* Move toward head */
    }

    /* Every frame is pinned — cannot evict */
    etp_log(LOG_WARN, "buffer_pool: all %u frames are pinned, cannot evict",
            pool->num_frames);
    return FRAME_NONE;
}

/* ================================================================
 * Public API — Lifecycle
 * ================================================================ */

buffer_pool_t *buffer_pool_create(uint32_t num_frames)
{
    if (num_frames == 0) {
        etp_log(LOG_ERROR, "buffer_pool_create: num_frames must be > 0");
        return NULL;
    }

    buffer_pool_t *pool = calloc(1, sizeof(buffer_pool_t));
    if (!pool) {
        etp_log(LOG_ERROR, "buffer_pool_create: failed to allocate pool struct");
        return NULL;
    }

    pool->num_frames   = num_frames;
    pool->map_capacity = num_frames * MAP_FACTOR;
    pool->lru_head     = FRAME_NONE;
    pool->lru_tail     = FRAME_NONE;
    pool->hits         = 0;
    pool->misses       = 0;

    /* Allocate frame array */
    pool->frames = calloc(num_frames, sizeof(frame_t));
    if (!pool->frames) {
        etp_log(LOG_ERROR, "buffer_pool_create: failed to allocate frames");
        free(pool);
        return NULL;
    }

    /* Initialise every frame's LRU pointers and state */
    for (uint32_t i = 0; i < num_frames; i++) {
        pool->frames[i].fd        = -1;
        pool->frames[i].page_id   = INVALID_PAGE_ID;
        pool->frames[i].pin_count = 0;
        pool->frames[i].is_dirty  = 0;
        pool->frames[i].is_valid  = 0;
        pool->frames[i].lru_prev  = FRAME_NONE;
        pool->frames[i].lru_next  = FRAME_NONE;
    }

    /* Allocate hash map */
    pool->map = calloc(pool->map_capacity, sizeof(hash_bucket_t));
    if (!pool->map) {
        etp_log(LOG_ERROR, "buffer_pool_create: failed to allocate hash map");
        free(pool->frames);
        free(pool);
        return NULL;
    }

    /* Initialise mutex */
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        etp_log(LOG_ERROR, "buffer_pool_create: pthread_mutex_init failed");
        free(pool->map);
        free(pool->frames);
        free(pool);
        return NULL;
    }

    etp_log(LOG_INFO, "buffer_pool_create: pool ready with %u frames, "
            "hash map capacity %u", num_frames, pool->map_capacity);
    return pool;
}

void buffer_pool_destroy(buffer_pool_t *pool)
{
    if (!pool) return;

    /* Flush all dirty pages before tearing down */
    buffer_pool_flush_all(pool);

    pthread_mutex_destroy(&pool->lock);
    free(pool->map);
    free(pool->frames);
    free(pool);

    etp_log(LOG_INFO, "buffer_pool_destroy: pool destroyed");
}

/* ================================================================
 * Public API — Core Operations
 * ================================================================ */

void *buffer_pool_fetch(buffer_pool_t *pool, int fd, page_id_t page_id)
{
    if (!pool) return NULL;

    pthread_mutex_lock(&pool->lock);

    /* ---- Cache lookup ---- */
    uint32_t bucket = _map_find(pool, fd, page_id);
    if (bucket != FRAME_NONE) {
        /* Cache HIT */
        uint32_t fi = pool->map[bucket].frame_idx;
        frame_t *f  = &pool->frames[fi];

        f->pin_count++;
        _lru_touch(pool, fi);
        pool->hits++;

        etp_log(LOG_DEBUG, "buffer_pool_fetch: HIT page %u (fd=%d) in frame %u "
                "(pin=%u)", page_id, fd, fi, f->pin_count);

        pthread_mutex_unlock(&pool->lock);
        return f->page_buf;
    }

    /* ---- Cache MISS — need a frame ---- */
    pool->misses++;

    uint32_t fi = _find_victim(pool);
    if (fi == FRAME_NONE) {
        etp_log(LOG_WARN, "buffer_pool_fetch: MISS page %u (fd=%d) — no "
                "evictable frame", page_id, fd);
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* Load the page from disk */
    frame_t *f = &pool->frames[fi];
    if (page_read(fd, page_id, f->page_buf) != 0) {
        etp_log(LOG_ERROR, "buffer_pool_fetch: failed to read page %u (fd=%d)",
                page_id, fd);
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* Set up frame metadata */
    f->fd        = fd;
    f->page_id   = page_id;
    f->pin_count = 1;
    f->is_dirty  = 0;
    f->is_valid  = 1;

    /* Insert into hash map and LRU list (MRU position) */
    _map_insert(pool, fd, page_id, fi);
    _lru_push_front(pool, fi);

    etp_log(LOG_DEBUG, "buffer_pool_fetch: MISS page %u (fd=%d) loaded into "
            "frame %u", page_id, fd, fi);

    pthread_mutex_unlock(&pool->lock);
    return f->page_buf;
}

void buffer_pool_unpin(buffer_pool_t *pool, int fd, page_id_t page_id)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    uint32_t bucket = _map_find(pool, fd, page_id);
    if (bucket == FRAME_NONE) {
        etp_log(LOG_WARN, "buffer_pool_unpin: page %u (fd=%d) not in pool",
                page_id, fd);
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    frame_t *f = &pool->frames[pool->map[bucket].frame_idx];

    if (f->pin_count == 0) {
        etp_log(LOG_WARN, "buffer_pool_unpin: page %u (fd=%d) already at "
                "pin_count 0", page_id, fd);
    } else {
        f->pin_count--;
        etp_log(LOG_DEBUG, "buffer_pool_unpin: page %u (fd=%d) pin_count=%u",
                page_id, fd, f->pin_count);
    }

    pthread_mutex_unlock(&pool->lock);
}

void buffer_pool_mark_dirty(buffer_pool_t *pool, int fd, page_id_t page_id)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);

    uint32_t bucket = _map_find(pool, fd, page_id);
    if (bucket == FRAME_NONE) {
        etp_log(LOG_WARN, "buffer_pool_mark_dirty: page %u (fd=%d) not in pool",
                page_id, fd);
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    pool->frames[pool->map[bucket].frame_idx].is_dirty = 1;
    etp_log(LOG_DEBUG, "buffer_pool_mark_dirty: page %u (fd=%d) marked dirty",
            page_id, fd);

    pthread_mutex_unlock(&pool->lock);
}

int buffer_pool_flush_page(buffer_pool_t *pool, int fd, page_id_t page_id)
{
    if (!pool) return -1;

    pthread_mutex_lock(&pool->lock);

    uint32_t bucket = _map_find(pool, fd, page_id);
    if (bucket == FRAME_NONE) {
        etp_log(LOG_WARN, "buffer_pool_flush_page: page %u (fd=%d) not in pool",
                page_id, fd);
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    frame_t *f = &pool->frames[pool->map[bucket].frame_idx];

    if (!f->is_dirty) {
        /* Not dirty — nothing to do */
        pthread_mutex_unlock(&pool->lock);
        return 0;
    }

    if (page_write(f->fd, f->page_id, f->page_buf) != 0) {
        etp_log(LOG_ERROR, "buffer_pool_flush_page: failed to write page %u "
                "(fd=%d)", f->page_id, f->fd);
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    f->is_dirty = 0;
    etp_log(LOG_DEBUG, "buffer_pool_flush_page: page %u (fd=%d) flushed",
            page_id, fd);

    pthread_mutex_unlock(&pool->lock);
    return 0;
}

int buffer_pool_flush_all(buffer_pool_t *pool)
{
    if (!pool) return -1;

    pthread_mutex_lock(&pool->lock);

    int result = 0;
    uint32_t flushed = 0;

    for (uint32_t i = 0; i < pool->num_frames; i++) {
        frame_t *f = &pool->frames[i];
        if (f->is_valid && f->is_dirty) {
            if (page_write(f->fd, f->page_id, f->page_buf) != 0) {
                etp_log(LOG_ERROR, "buffer_pool_flush_all: failed to write "
                        "page %u (fd=%d) in frame %u", f->page_id, f->fd, i);
                result = -1;  /* Record failure but keep flushing others */
            } else {
                f->is_dirty = 0;
                flushed++;
            }
        }
    }

    etp_log(LOG_INFO, "buffer_pool_flush_all: flushed %u dirty pages "
            "(result=%d)", flushed, result);

    pthread_mutex_unlock(&pool->lock);
    return result;
}

/* ================================================================
 * Public API — Statistics
 * ================================================================ */

uint32_t buffer_pool_hit_count(buffer_pool_t *pool)
{
    if (!pool) return 0;

    pthread_mutex_lock(&pool->lock);
    uint32_t h = pool->hits;
    pthread_mutex_unlock(&pool->lock);
    return h;
}

uint32_t buffer_pool_miss_count(buffer_pool_t *pool)
{
    if (!pool) return 0;

    pthread_mutex_lock(&pool->lock);
    uint32_t m = pool->misses;
    pthread_mutex_unlock(&pool->lock);
    return m;
}
