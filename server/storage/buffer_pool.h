/*
 * ============================================================================
 * Event Ticketing Platform — Buffer Pool Interface
 * ============================================================================
 * In-memory page cache sitting between the table layer and disk I/O.
 *
 * Features:
 *   - Fixed-size frame array (default BUFFER_POOL_FRAMES = 64)
 *   - O(1) hash-map lookup: (fd, page_id) → frame index
 *   - Doubly-linked LRU list for eviction of unpinned frames
 *   - Pin counting — pages stay resident while any thread uses them
 *   - Dirty tracking — modified pages are flushed before eviction
 *   - Thread-safe via a single pool-level mutex
 *
 * Usage pattern:
 *   void *page = buffer_pool_fetch(pool, fd, pid);   // pin++
 *   // ... read / mutate the page ...
 *   buffer_pool_mark_dirty(pool, fd, pid);            // if mutated
 *   buffer_pool_unpin(pool, fd, pid);                 // pin--
 * ============================================================================
 */

#ifndef ETP_BUFFER_POOL_H
#define ETP_BUFFER_POOL_H

#include <stdint.h>
#include "../../common/types.h"
#include "../../common/config.h"

/* Opaque handle — implementation details hidden in buffer_pool.c */
typedef struct buffer_pool buffer_pool_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * buffer_pool_create — Allocate and initialise a pool with `num_frames`
 * page frames.  Returns NULL on allocation failure.
 */
buffer_pool_t *buffer_pool_create(uint32_t num_frames);

/*
 * buffer_pool_destroy — Flush all dirty pages, free every frame buffer,
 * and release the pool structure itself.
 */
void buffer_pool_destroy(buffer_pool_t *pool);

/* ================================================================
 * Core Operations
 * ================================================================ */

/*
 * buffer_pool_fetch — Look up (fd, page_id) in the cache.
 *   • Cache hit  — move frame to MRU end, increment pin_count, return ptr.
 *   • Cache miss — find a free or LRU-evictable frame, load the page from
 *                  disk, insert into hash map, pin, and return ptr.
 *   • If all frames are pinned, returns NULL (caller must retry later).
 *
 * The returned pointer is valid until buffer_pool_destroy().  Caller MUST
 * call buffer_pool_unpin() when done.
 */
void *buffer_pool_fetch(buffer_pool_t *pool, int fd, page_id_t page_id);

/*
 * buffer_pool_unpin — Decrement pin_count for (fd, page_id).
 * When pin_count reaches 0 the frame becomes eligible for eviction.
 */
void buffer_pool_unpin(buffer_pool_t *pool, int fd, page_id_t page_id);

/*
 * buffer_pool_mark_dirty — Flag the frame holding (fd, page_id) as dirty.
 * Dirty frames are written back to disk on eviction or explicit flush.
 */
void buffer_pool_mark_dirty(buffer_pool_t *pool, int fd, page_id_t page_id);

/*
 * buffer_pool_flush_page — Write a single dirty page back to disk.
 * Clears the dirty flag on success.  Returns 0 on success, -1 on error.
 */
int buffer_pool_flush_page(buffer_pool_t *pool, int fd, page_id_t page_id);

/*
 * buffer_pool_flush_all — Write ALL dirty pages to disk.
 * Returns 0 if every page flushed successfully, -1 if any write failed.
 */
int buffer_pool_flush_all(buffer_pool_t *pool);

/* ================================================================
 * Statistics
 * ================================================================ */
uint32_t buffer_pool_hit_count(buffer_pool_t *pool);
uint32_t buffer_pool_miss_count(buffer_pool_t *pool);

#endif /* ETP_BUFFER_POOL_H */
