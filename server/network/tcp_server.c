/*
 * ============================================================================
 * Event Ticketing Platform — TCP Server (Implementation)
 * ============================================================================
 * Sets up a TCP listener, accepts client connections, and dispatches each
 * to the thread pool for concurrent handling.
 *
 * OS concepts demonstrated:
 *   - socket(), bind(), listen(), accept()  — Socket Programming
 *   - setsockopt(SO_REUSEADDR)              — Port reuse
 *   - Thread pool dispatch                  — Concurrency
 *   - Semaphore for connection limiting     — Resource control
 * ============================================================================
 */

#include "tcp_server.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>

/* ================================================================
 * Internal: Client task argument (passed to thread pool)
 * ================================================================ */
typedef struct {
    int                 client_fd;
    client_handler_fn   handler;
    void               *context;
    sem_t              *conn_semaphore;  /* Release slot when client disconnects */
} client_task_arg_t;

/*
 * Wrapper that the thread pool executes. Calls the user-provided
 * handler, then cleans up the socket and releases the connection
 * semaphore slot.
 */
static void client_task_wrapper(void *arg) {
    client_task_arg_t *task = (client_task_arg_t *)arg;

    etp_log(LOG_DEBUG, "tcp: handling client fd=%d", task->client_fd);

    /* Call the user's handler — this runs the full client session */
    task->handler(task->client_fd, task->context);

    /* Cleanup */
    close(task->client_fd);
    etp_log(LOG_DEBUG, "tcp: client fd=%d disconnected", task->client_fd);

    /* Release connection slot */
    if (task->conn_semaphore) {
        sem_post(task->conn_semaphore);
    }

    free(task);
}

/* ================================================================
 * Connection-limiting semaphore (global for this server instance)
 *
 * Limits the number of concurrent client connections to MAX_CONNECTIONS.
 * This demonstrates the OS concept of semaphores for resource control.
 * ================================================================ */
static sem_t g_conn_semaphore;
static int   g_sem_initialized = 0;

/* ================================================================
 * Public API
 * ================================================================ */

tcp_server_t *tcp_server_create(int port, int num_threads,
                                 client_handler_fn handler, void *context) {
    if (!handler) {
        etp_log(LOG_ERROR, "tcp_server_create: NULL handler");
        return NULL;
    }

    tcp_server_t *server = calloc(1, sizeof(tcp_server_t));
    if (!server) return NULL;

    server->port            = port;
    server->handler         = handler;
    server->handler_context = context;
    server->running         = 0;

    /* ── Create socket ────────────────────────────────────────── */
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) {
        etp_log(LOG_FATAL, "tcp: socket() failed: %s", strerror(errno));
        free(server);
        return NULL;
    }

    /* Allow port reuse (avoid "Address already in use" on restart) */
    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ── Bind ─────────────────────────────────────────────────── */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        etp_log(LOG_FATAL, "tcp: bind() on port %d failed: %s", port, strerror(errno));
        close(server->server_fd);
        free(server);
        return NULL;
    }

    /* ── Listen ───────────────────────────────────────────────── */
    if (listen(server->server_fd, SERVER_BACKLOG) < 0) {
        etp_log(LOG_FATAL, "tcp: listen() failed: %s", strerror(errno));
        close(server->server_fd);
        free(server);
        return NULL;
    }

    /* ── Create thread pool ───────────────────────────────────── */
    server->pool = thread_pool_create(num_threads, MAX_CONNECTIONS);
    if (!server->pool) {
        etp_log(LOG_FATAL, "tcp: failed to create thread pool");
        close(server->server_fd);
        free(server);
        return NULL;
    }

    /* ── Initialize connection-limiting semaphore ─────────────── */
    if (!g_sem_initialized) {
        sem_init(&g_conn_semaphore, 0, MAX_CONNECTIONS);
        g_sem_initialized = 1;
    }

    etp_log(LOG_INFO, "tcp: server created on port %d (%d worker threads)",
            port, num_threads);
    return server;
}

void tcp_server_start(tcp_server_t *server) {
    if (!server) return;

    server->running = 1;

    /* Ignore SIGPIPE so writes to closed sockets don't kill the server */
    signal(SIGPIPE, SIG_IGN);

    etp_log(LOG_INFO, "tcp: server listening on port %d...", server->port);
    printf("\n  ╔═══════════════════════════════════════════════════╗\n");
    printf("  ║  Event Ticketing Platform — Server Running        ║\n");
    printf("  ║  Port: %-5d  |  Workers: %-3d                    ║\n",
           server->port, thread_pool_thread_count(server->pool));
    printf("  ║  Press Ctrl+C to stop                             ║\n");
    printf("  ╚═══════════════════════════════════════════════════╝\n\n");

    /* ── Accept Loop ──────────────────────────────────────────── */
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        /* Wait for connection semaphore (limits concurrent connections) */
        sem_wait(&g_conn_semaphore);

        if (!server->running) {
            sem_post(&g_conn_semaphore);
            break;
        }

        int client_fd = accept(server->server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (server->running) {
                etp_log(LOG_ERROR, "tcp: accept() failed: %s", strerror(errno));
            }
            sem_post(&g_conn_semaphore);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);
        etp_log(LOG_INFO, "tcp: client connected from %s:%d (fd=%d)",
                client_ip, client_port, client_fd);

        /* Create task argument */
        client_task_arg_t *task_arg = malloc(sizeof(client_task_arg_t));
        if (!task_arg) {
            etp_log(LOG_ERROR, "tcp: out of memory for client task");
            close(client_fd);
            sem_post(&g_conn_semaphore);
            continue;
        }

        task_arg->client_fd      = client_fd;
        task_arg->handler        = server->handler;
        task_arg->context        = server->handler_context;
        task_arg->conn_semaphore = &g_conn_semaphore;

        /* Dispatch to thread pool */
        if (thread_pool_submit(server->pool, client_task_wrapper, task_arg) != 0) {
            etp_log(LOG_ERROR, "tcp: failed to submit client task to pool");
            close(client_fd);
            free(task_arg);
            sem_post(&g_conn_semaphore);
        }
    }

    etp_log(LOG_INFO, "tcp: accept loop stopped");
}

void tcp_server_stop(tcp_server_t *server) {
    if (!server) return;
    server->running = 0;

    /* Close the listening socket to unblock accept() */
    if (server->server_fd >= 0) {
        shutdown(server->server_fd, SHUT_RDWR);
        close(server->server_fd);
        server->server_fd = -1;
    }

    /* Post to semaphore to unblock if waiting there */
    sem_post(&g_conn_semaphore);

    etp_log(LOG_INFO, "tcp: server stop signal sent");
}

void tcp_server_destroy(tcp_server_t *server) {
    if (!server) return;

    tcp_server_stop(server);

    if (server->pool) {
        thread_pool_destroy(server->pool);
    }

    if (g_sem_initialized) {
        sem_destroy(&g_conn_semaphore);
        g_sem_initialized = 0;
    }

    free(server);
    etp_log(LOG_INFO, "tcp: server destroyed");
}
