/*
 * ============================================================================
 * Event Ticketing Platform — Thread Pool (Implementation)
 * ============================================================================
 * Bounded producer-consumer thread pool using POSIX threads.
 *
 * OS concepts demonstrated:
 *   - pthread_create / pthread_join       (thread lifecycle)
 *   - pthread_mutex_t                     (mutual exclusion)
 *   - pthread_cond_t                      (condition variables)
 *   - Bounded buffer / producer-consumer  (classic sync pattern)
 *
 * The queue is a singly-linked list. Workers dequeue from the head,
 * submitters enqueue at the tail. Shutdown is cooperative: we set a
 * flag, broadcast to all workers, and let them drain the queue before
 * exiting.
 * ============================================================================
 */

#include "thread_pool.h"
#include "../../common/utils.h"

#include <stdlib.h>
#include <pthread.h>

/* ================================================================
 * Internal: Task Node (linked list element)
 * ================================================================ */
typedef struct task_node {
    task_fn             function;   /* Function to execute          */
    void               *arg;        /* Argument to pass             */
    struct task_node   *next;       /* Next task in queue            */
} task_node_t;

/* ================================================================
 * Thread Pool Structure
 * ================================================================ */
struct thread_pool {
    /* Worker threads */
    pthread_t      *threads;
    int             num_threads;

    /* Task queue (singly-linked list) */
    task_node_t    *queue_head;
    task_node_t    *queue_tail;
    int             queue_size;
    int             max_queue_size;

    /* Synchronization primitives — core OS concepts */
    pthread_mutex_t mutex;          /* Protects all shared state    */
    pthread_cond_t  not_empty;      /* Workers wait here            */
    pthread_cond_t  not_full;       /* Submitters wait here         */

    /* Shutdown flag */
    int             shutdown;
};

/* ================================================================
 * Worker Thread Function
 *
 * Each worker loops forever:
 *   1. Lock mutex
 *   2. Wait on `not_empty` while queue is empty (and not shutting down)
 *   3. Dequeue a task
 *   4. Signal `not_full` (space available for submitters)
 *   5. Unlock mutex
 *   6. Execute the task
 *
 * On shutdown: drain remaining tasks, then exit.
 * ================================================================ */
static void *worker_thread(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;

    etp_log(LOG_DEBUG, "thread_pool: worker started (tid=%lu)",
            (unsigned long)pthread_self());

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        /* Wait while queue is empty AND not shutting down */
        while (pool->queue_size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        /* If shutting down and queue is drained, exit */
        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* Dequeue task from head */
        task_node_t *task = pool->queue_head;
        pool->queue_head = task->next;
        if (!pool->queue_head) {
            pool->queue_tail = NULL;
        }
        pool->queue_size--;

        /* Signal submitters that space is available */
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        /* Execute the task (outside the lock!) */
        task->function(task->arg);
        free(task);
    }

    etp_log(LOG_DEBUG, "thread_pool: worker exiting (tid=%lu)",
            (unsigned long)pthread_self());
    return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

thread_pool_t *thread_pool_create(int num_threads, int max_queue_size) {
    if (num_threads <= 0 || max_queue_size <= 0) {
        etp_log(LOG_ERROR, "thread_pool_create: invalid args (threads=%d, queue=%d)",
                num_threads, max_queue_size);
        return NULL;
    }

    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (!pool) {
        etp_log(LOG_ERROR, "thread_pool_create: out of memory");
        return NULL;
    }

    pool->num_threads    = num_threads;
    pool->max_queue_size = max_queue_size;
    pool->shutdown       = 0;
    pool->queue_head     = NULL;
    pool->queue_tail     = NULL;
    pool->queue_size     = 0;

    /* Initialize synchronization primitives */
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    /* Create worker threads */
    pool->threads = calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->not_empty);
        pthread_cond_destroy(&pool->not_full);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            etp_log(LOG_ERROR, "thread_pool_create: pthread_create failed for thread %d", i);
            /* Partial creation — shut down what we have */
            pool->num_threads = i;
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    etp_log(LOG_INFO, "thread_pool: created with %d workers, queue capacity %d",
            num_threads, max_queue_size);
    return pool;
}

void thread_pool_destroy(thread_pool_t *pool) {
    if (!pool) return;

    etp_log(LOG_INFO, "thread_pool: shutting down (%d tasks remaining)", pool->queue_size);

    /* Signal shutdown */
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);   /* Wake all workers      */
    pthread_cond_broadcast(&pool->not_full);    /* Wake any blocked submitters */
    pthread_mutex_unlock(&pool->mutex);

    /* Join all worker threads (they will drain remaining tasks first) */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Free any remaining tasks (shouldn't be any if workers drained) */
    task_node_t *task = pool->queue_head;
    while (task) {
        task_node_t *next = task->next;
        free(task);
        task = next;
    }

    /* Cleanup */
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->threads);
    free(pool);

    etp_log(LOG_INFO, "thread_pool: destroyed");
}

int thread_pool_submit(thread_pool_t *pool, task_fn function, void *arg) {
    if (!pool || !function) return -1;

    pthread_mutex_lock(&pool->mutex);

    /* Block while queue is full (bounded producer) */
    while (pool->queue_size >= pool->max_queue_size && !pool->shutdown) {
        etp_log(LOG_DEBUG, "thread_pool: queue full (%d/%d), submitter blocking",
                pool->queue_size, pool->max_queue_size);
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    /* Create and enqueue task */
    task_node_t *task = malloc(sizeof(task_node_t));
    if (!task) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    task->function = function;
    task->arg      = arg;
    task->next     = NULL;

    if (pool->queue_tail) {
        pool->queue_tail->next = task;
    } else {
        pool->queue_head = task;
    }
    pool->queue_tail = task;
    pool->queue_size++;

    /* Signal one waiting worker */
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

int thread_pool_queue_size(thread_pool_t *pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mutex);
    int size = pool->queue_size;
    pthread_mutex_unlock(&pool->mutex);
    return size;
}

int thread_pool_thread_count(thread_pool_t *pool) {
    return pool ? pool->num_threads : 0;
}
