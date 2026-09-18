/*
 * ============================================================================
 * Event Ticketing Platform — Thread Pool (Header)
 * ============================================================================
 * Fixed-size thread pool with a bounded task queue. Demonstrates core OS
 * concurrency concepts: pthread creation, mutexes, condition variables.
 *
 * Workers block on a condition variable when the queue is empty.
 * Submitters block when the queue is full (bounded producer-consumer).
 * ============================================================================
 */

#ifndef ETP_THREAD_POOL_H
#define ETP_THREAD_POOL_H

/* Opaque thread pool handle */
typedef struct thread_pool thread_pool_t;

/* Task function signature */
typedef void (*task_fn)(void *arg);

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create a pool with `num_threads` workers and a task queue of `max_queue_size` */
thread_pool_t *thread_pool_create(int num_threads, int max_queue_size);

/* Graceful shutdown: finish queued tasks, then join all threads and free memory */
void thread_pool_destroy(thread_pool_t *pool);

/* ================================================================
 * Task Submission
 * ================================================================ */

/* Submit a task. Blocks if queue is full. Returns 0 on success, -1 on error. */
int thread_pool_submit(thread_pool_t *pool, task_fn function, void *arg);

/* ================================================================
 * Stats
 * ================================================================ */

/* Current number of tasks waiting in the queue */
int thread_pool_queue_size(thread_pool_t *pool);

/* Number of worker threads */
int thread_pool_thread_count(thread_pool_t *pool);

#endif /* ETP_THREAD_POOL_H */
