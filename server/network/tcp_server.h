/*
 * ============================================================================
 * Event Ticketing Platform — TCP Server (Header)
 * ============================================================================
 * Multi-threaded TCP server using POSIX sockets and a thread pool.
 *
 * OS concepts demonstrated:
 *   - socket(), bind(), listen(), accept() — Socket Programming
 *   - Thread pool dispatch — Concurrency Control
 *   - Per-client handler — Client-Server Model
 * ============================================================================
 */

#ifndef ETP_TCP_SERVER_H
#define ETP_TCP_SERVER_H

#include "thread_pool.h"
#include "../../common/config.h"

/* ================================================================
 * Client Handler Callback
 *
 * Called by a worker thread for each accepted client connection.
 * The handler is responsible for the full client lifecycle:
 *   - Read commands
 *   - Parse, check auth/RBAC, execute
 *   - Send responses
 *   - Close connection on QUIT or error
 *
 * `client_fd` is the accepted socket. `context` is user-provided.
 * ================================================================ */
typedef void (*client_handler_fn)(int client_fd, void *context);

/* ================================================================
 * TCP Server Handle
 * ================================================================ */
typedef struct {
    int                 server_fd;       /* Listening socket fd          */
    int                 port;            /* Port number                  */
    thread_pool_t      *pool;            /* Worker thread pool           */
    volatile int        running;         /* 1 while accepting clients    */
    client_handler_fn   handler;         /* Per-client handler callback  */
    void               *handler_context; /* Passed to handler            */
} tcp_server_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * Create and bind a TCP server on the given port.
 * Does NOT start accepting — call tcp_server_start() for that.
 */
tcp_server_t *tcp_server_create(int port, int num_threads,
                                 client_handler_fn handler, void *context);

/*
 * Start the accept loop (BLOCKING — runs until tcp_server_stop() is called).
 * Each accepted connection is dispatched to the thread pool.
 */
void tcp_server_start(tcp_server_t *server);

/*
 * Signal the server to stop accepting new connections.
 * Currently connected clients will finish their handler.
 */
void tcp_server_stop(tcp_server_t *server);

/*
 * Cleanup and free the server. Destroys the thread pool.
 */
void tcp_server_destroy(tcp_server_t *server);

#endif /* ETP_TCP_SERVER_H */
