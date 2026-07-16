/*
 * ============================================================================
 * Event Ticketing Platform — Server Entry Point
 * ============================================================================
 * Initializes all subsystems and starts the TCP server.
 *
 * Startup sequence:
 *   1. Create data directory
 *   2. Initialize storage layer (tables for users, events, seats, bookings)
 *   3. Initialize WAL (Write-Ahead Log)
 *   4. Initialize lock manager + transaction manager
 *   5. Initialize auth, event manager, booking engine, reports
 *   6. Initialize IPC (pipe logger, message queue notifier, shared memory stats)
 *   7. Bootstrap admin user (if first run)
 *   8. Start TCP server (blocking — runs accept loop)
 *
 * Shutdown:
 *   - SIGINT/SIGTERM triggers graceful shutdown
 *   - Flush all dirty pages, close WAL, destroy subsystems
 * ============================================================================
 */

#include "../common/types.h"
#include "../common/config.h"
#include "../common/protocol.h"
#include "../common/utils.h"

#include "storage/table.h"
#include "storage/buffer_pool.h"
#include "storage/wal.h"
#include "network/tcp_server.h"
#include "network/thread_pool.h"
#include "auth/auth.h"
#include "auth/rbac.h"
#include "protocol/parser.h"
#include "txn/lock_manager.h"
#include "txn/txn_manager.h"
#include "core/event_mgr.h"
#include "core/booking_engine.h"
#include "core/reports.h"
#include "ipc/logger.h"
#include "ipc/notifier.h"
#include "ipc/stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>  /* shutdown(), SHUT_RDWR */

/* ================================================================
 * Global Server Context — all subsystems
 * ================================================================ */
typedef struct {
    /* Storage */
    buffer_pool_t   *buffer_pool;
    wal_t           *wal;
    table_t         *users_table;
    table_t         *events_table;
    table_t         *seats_table;
    table_t         *bookings_table;
    table_t         *booking_seats_table;

    /* Concurrency */
    lock_manager_t  *lock_mgr;
    txn_manager_t   *txn_mgr;

    /* Auth */
    auth_manager_t  *auth_mgr;

    /* Business logic */
    event_manager_t *event_mgr;
    booking_engine_t *booking_engine;
    reports_engine_t *reports;

    /* IPC */
    pipe_logger_t   *logger;
    notifier_t      *notifier;
    shm_stats_t     *stats;

    /* Network */
    tcp_server_t    *tcp_server;
} server_context_t;

static server_context_t g_ctx;
static volatile int g_shutdown = 0;

/* ================================================================
 * Signal Handler — graceful shutdown on Ctrl+C
 * ================================================================ */
static void signal_handler(int sig) {
    (void)sig;
    /* Only use async-signal-safe operations here.
     * tcp_server_stop() calls etp_log() which uses printf — NOT safe.
     * Instead, just set the flag and close the listening socket to
     * unblock accept(). The main thread handles proper shutdown. */
    const char *msg = "\n  Shutting down...\n";
    write(STDOUT_FILENO, msg, 20);
    g_shutdown = 1;
    if (g_ctx.tcp_server && g_ctx.tcp_server->server_fd >= 0) {
        shutdown(g_ctx.tcp_server->server_fd, SHUT_RDWR);
    }
}

/* ================================================================
 * Client Handler — the glue between network and business logic
 *
 * Runs in a worker thread for each connected client.
 * Loop: read command → parse → check RBAC → execute → respond
 * ================================================================ */
static void handle_command(server_context_t *ctx, session_t *session,
                            parsed_command_t *cmd, char *response, size_t resp_size);

static void client_handler(int client_fd, void *context) {
    server_context_t *ctx = (server_context_t *)context;

    /* Track connection */
    stats_inc_connections(ctx->stats);

    /* Create session for this client */
    session_t *session = auth_create_session(ctx->auth_mgr, client_fd);
    if (!session) {
        const char *err = "ERROR Server full, try later\n";
        write(client_fd, err, strlen(err));
        stats_dec_connections(ctx->stats);
        return;
    }

    /* Send welcome banner */
    const char *banner =
        "\n"
        "  ╔═══════════════════════════════════════════════════╗\n"
        "  ║     Welcome to Event Ticketing Platform           ║\n"
        "  ║     Type HELP for available commands              ║\n"
        "  ╚═══════════════════════════════════════════════════╝\n"
        "\n"
        "etp> ";
    write(client_fd, banner, strlen(banner));

    /* Heap-allocate the response buffer to avoid stack overflow.
     * MAX_RESPONSE_SIZE (64KB) is too large for a worker thread's stack. */
    char *response = malloc(MAX_RESPONSE_SIZE);
    if (!response) {
        const char *err = "ERROR Server out of memory\n";
        write(client_fd, err, strlen(err));
        auth_remove_session(ctx->auth_mgr, client_fd);
        stats_dec_connections(ctx->stats);
        close(client_fd);
        return;
    }

    /* Command loop */
    char buf[MAX_CMD_LENGTH];
    while (!g_shutdown) {
        memset(buf, 0, sizeof(buf));

        /* Read one line from client */
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;  /* Client disconnected or error */

        buf[n] = '\0';

        /* Strip trailing newline */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n == 0) {
            write(client_fd, "etp> ", 5);
            continue;
        }

        stats_inc_requests(ctx->stats);
        stats_update_last_request(ctx->stats);

        /* Parse the command */
        parsed_command_t cmd;
        if (parse_command(buf, &cmd) != 0) {
            const char *err = "ERROR Invalid command. Type HELP for usage.\n";
            write(client_fd, err, strlen(err));
            write(client_fd, "etp> ", 5);
            continue;
        }

        /* QUIT — disconnect */
        if (cmd.type == CMD_QUIT) {
            const char *bye = "OK Goodbye!\n";
            write(client_fd, bye, strlen(bye));
            break;
        }

        /* Check RBAC permission */
        if (!rbac_check(session->role, cmd.type)) {
            char deny[256];
            snprintf(deny, sizeof(deny), "DENIED %s\n",
                     rbac_denial_reason(session->role, cmd.type));
            write(client_fd, deny, strlen(deny));
            write(client_fd, "etp> ", 5);
            stats_inc_failed_requests(ctx->stats);
            continue;
        }

        /* Execute the command */
        response[0] = '\0';
        handle_command(ctx, session, &cmd, response, MAX_RESPONSE_SIZE);

        /* Send response */
        write(client_fd, response, strlen(response));
        write(client_fd, "etp> ", 5);
    }

    /* Cleanup */
    free(response);
    auth_remove_session(ctx->auth_mgr, client_fd);
    stats_dec_connections(ctx->stats);
    /* NOTE: Do NOT close(client_fd) here — tcp_server.c:client_task_wrapper()
     * closes the fd after this handler returns. Double-close would corrupt
     * a recycled fd belonging to another connection. */

    if (ctx->logger) {
        pipe_logger_write(ctx->logger, "Client fd=%d disconnected", client_fd);
    }
}

/* ================================================================
 * Command Execution Router
 * ================================================================ */
static void handle_command(server_context_t *ctx, session_t *session,
                            parsed_command_t *cmd, char *response, size_t resp_size) {
    etp_result_t rc;

    switch (cmd->type) {

    /* ── REGISTER username password [role] ─────────────────── */
    case CMD_REGISTER: {
        if (cmd->argc < 2) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: REGISTER <username> <password> [customer|organizer]");
            return;
        }
        user_role_t role = ROLE_CUSTOMER;
        if (cmd->argc >= 3) {
            if (strcasecmp(cmd->args[2], "organizer") == 0) role = ROLE_ORGANIZER;
        }
        uint32_t user_id = 0;
        rc = auth_register(ctx->auth_mgr, cmd->args[0], cmd->args[1], role, &user_id);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK,
                           "Registered user '%s' (ID: %u, Role: %s)",
                           cmd->args[0], user_id,
                           role == ROLE_ORGANIZER ? "ORGANIZER" : "CUSTOMER");
        } else if (rc == ETP_ERR_DUPLICATE) {
            format_response(response, resp_size, RESP_ERROR,
                           "Username '%s' already taken", cmd->args[0]);
        } else {
            format_response(response, resp_size, RESP_ERROR, "Registration failed");
        }
        return;
    }

    /* ── LOGIN username password ───────────────────────────── */
    case CMD_LOGIN: {
        if (cmd->argc < 2) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: LOGIN <username> <password>");
            return;
        }
        if (session->authenticated) {
            format_response(response, resp_size, RESP_ERROR,
                           "Already logged in as '%s'. LOGOUT first.", session->username);
            return;
        }
        rc = auth_login(ctx->auth_mgr, session->fd, cmd->args[0], cmd->args[1]);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK,
                           "Welcome, %s! (Role: %s)",
                           session->username,
                           session->role == ROLE_ADMIN ? "ADMIN" :
                           session->role == ROLE_ORGANIZER ? "ORGANIZER" : "CUSTOMER");
        } else {
            format_response(response, resp_size, RESP_ERROR, "Invalid credentials");
            stats_inc_failed_requests(ctx->stats);
        }
        return;
    }

    /* ── LOGOUT ────────────────────────────────────────────── */
    case CMD_LOGOUT: {
        rc = auth_logout(ctx->auth_mgr, session->fd);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK, "Logged out successfully");
        } else {
            format_response(response, resp_size, RESP_ERROR, "Not logged in");
        }
        return;
    }

    /* ── CREATE_EVENT name venue date time rows seats_per_row price ── */
    case CMD_CREATE_EVENT: {
        if (cmd->argc < 7) {
            format_response(response, resp_size, RESP_ERROR,
                "Usage: CREATE_EVENT <name> <venue> <date> <time> <rows> <seats_per_row> <price>");
            return;
        }
        uint32_t event_id = 0;
        rc = event_mgr_create_event(ctx->event_mgr,
                cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3],
                session->user_id,
                (uint16_t)atoi(cmd->args[4]), (uint16_t)atoi(cmd->args[5]),
                (float)atof(cmd->args[6]),
                &event_id);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK,
                "Event '%s' created (ID: %u, %d seats)",
                cmd->args[0], event_id,
                atoi(cmd->args[4]) * atoi(cmd->args[5]));
            stats_inc_events(ctx->stats);
            if (ctx->notifier)
                notify_event_created(ctx->notifier, event_id, cmd->args[0]);
        } else {
            format_response(response, resp_size, RESP_ERROR, "Failed to create event");
        }
        return;
    }

    /* ── DELETE_EVENT event_id ──────────────────────────────── */
    case CMD_DELETE_EVENT: {
        if (cmd->argc < 1) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: DELETE_EVENT <event_id>");
            return;
        }
        uint32_t eid = (uint32_t)atoi(cmd->args[0]);
        rc = event_mgr_delete_event(ctx->event_mgr, eid);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK, "Event %u deleted", eid);
        } else {
            format_response(response, resp_size, RESP_ERROR, "Event not found");
        }
        return;
    }

    /* ── LIST_EVENTS ───────────────────────────────────────── */
    case CMD_LIST_EVENTS: {
        event_record_t events[50];
        int count = event_mgr_list_events(ctx->event_mgr, events, 50);
        if (count == 0) {
            format_response(response, resp_size, RESP_OK, "No active events");
            return;
        }
        int off = snprintf(response, resp_size,
            "OK %d active event(s):\n"
            "  %-5s %-20s %-15s %-11s %-5s  %-6s  Rs.\n"
            "  ───── ──────────────────── ─────────────── ─────────── ─────  ──────  ────\n",
            count, "ID", "Name", "Venue", "Date", "Time", "Seats");
        for (int i = 0; i < count && off < (int)resp_size - 100; i++) {
            off += snprintf(response + off, resp_size - off,
                "  %-5u %-20.20s %-15.15s %-11s %-5s  %u×%-3u  %.0f\n",
                events[i].event_id, events[i].name, events[i].venue,
                events[i].event_date, events[i].event_time,
                events[i].total_rows, events[i].seats_per_row,
                events[i].price);
        }
        return;
    }

    /* ── VIEW_EVENT event_id ───────────────────────────────── */
    case CMD_VIEW_EVENT: {
        if (cmd->argc < 1) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: VIEW_EVENT <event_id>");
            return;
        }
        event_record_t evt;
        rc = event_mgr_get_event(ctx->event_mgr, (uint32_t)atoi(cmd->args[0]), &evt);
        if (rc != ETP_OK) {
            format_response(response, resp_size, RESP_ERROR, "Event not found");
            return;
        }
        snprintf(response, resp_size,
            "OK Event Details:\n"
            "  ID:      %u\n"
            "  Name:    %s\n"
            "  Venue:   %s\n"
            "  Date:    %s  Time: %s\n"
            "  Seats:   %u rows × %u per row = %u total\n"
            "  Price:   Rs. %.2f\n"
            "  Status:  %s\n",
            evt.event_id, evt.name, evt.venue,
            evt.event_date, evt.event_time,
            evt.total_rows, evt.seats_per_row,
            evt.total_rows * evt.seats_per_row,
            evt.price,
            evt.status == EVENT_ACTIVE ? "ACTIVE" : "CANCELLED");
        return;
    }

    /* ── VIEW_SEATS event_id ───────────────────────────────── */
    case CMD_VIEW_SEATS: {
        if (cmd->argc < 1) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: VIEW_SEATS <event_id>");
            return;
        }
        uint32_t eid = (uint32_t)atoi(cmd->args[0]);

        /* Heap-allocate to avoid stack overflow (2600 × 32 = 83KB) */
        int max_seats = 2600;
        seat_record_t *seats = malloc(max_seats * sizeof(seat_record_t));
        if (!seats) {
            format_response(response, resp_size, RESP_ERROR, "Out of memory");
            return;
        }
        int count = event_mgr_get_seats(ctx->event_mgr, eid, seats, max_seats);
        if (count == 0) {
            free(seats);
            format_response(response, resp_size, RESP_ERROR, "No seats found");
            return;
        }
        int off = snprintf(response, resp_size,
            "OK Seats for event %u (%d total):\n"
            "  %-6s %-5s %-8s\n"
            "  ────── ───── ────────\n",
            eid, count, "SeatID", "Seat", "Status");
        for (int i = 0; i < count && off < (int)resp_size - 60; i++) {
            const char *sts = seats[i].status == SEAT_AVAILABLE ? "AVAIL" :
                              seats[i].status == SEAT_BOOKED    ? "BOOKED" : "LOCKED";
            off += snprintf(response + off, resp_size - off,
                "  %-6u %c%-4u %-8s\n",
                seats[i].seat_id, seats[i].row_label, seats[i].seat_number, sts);
        }
        free(seats);
        return;
    }

    /* ── BOOK event_id seat_id1 seat_id2 ... ──────────────── */
    case CMD_BOOK: {
        if (cmd->argc < 2) {
            format_response(response, resp_size, RESP_ERROR,
                "Usage: BOOK <event_id> <seat_id1> [seat_id2] ...");
            return;
        }
        uint32_t eid = (uint32_t)atoi(cmd->args[0]);
        int num_seats = cmd->argc - 1;
        /* Cap to prevent buffer overread */
        if (num_seats > MAX_SEATS_PER_BOOKING) num_seats = MAX_SEATS_PER_BOOKING;
        uint32_t seat_ids[MAX_SEATS_PER_BOOKING];
        for (int i = 0; i < num_seats; i++) {
            seat_ids[i] = (uint32_t)atoi(cmd->args[i + 1]);
        }
        uint32_t booking_id = 0;
        rc = booking_book_seats(ctx->booking_engine, session->user_id,
                                eid, seat_ids, num_seats, &booking_id);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK,
                "Booking confirmed! (Booking ID: %u, %d seats)", booking_id, num_seats);
            stats_inc_bookings(ctx->stats);
            if (ctx->notifier)
                notify_booking_confirmed(ctx->notifier, booking_id,
                                         session->user_id, eid, num_seats);
        } else if (rc == ETP_ERR_SEAT_UNAVAIL) {
            format_response(response, resp_size, RESP_ERROR,
                "One or more seats are not available");
        } else if (rc == ETP_ERR_NOT_FOUND) {
            format_response(response, resp_size, RESP_ERROR,
                "Event or seat not found");
        } else {
            format_response(response, resp_size, RESP_ERROR, "Booking failed");
        }
        return;
    }

    /* ── CANCEL booking_id ─────────────────────────────────── */
    case CMD_CANCEL: {
        if (cmd->argc < 1) {
            format_response(response, resp_size, RESP_ERROR,
                           "Usage: CANCEL <booking_id>");
            return;
        }
        uint32_t bid = (uint32_t)atoi(cmd->args[0]);
        rc = booking_cancel(ctx->booking_engine, bid, session->user_id);
        if (rc == ETP_OK) {
            format_response(response, resp_size, RESP_OK,
                           "Booking %u cancelled, seats released", bid);
            stats_inc_cancelled_bookings(ctx->stats);
            if (ctx->notifier)
                notify_booking_cancelled(ctx->notifier, bid, session->user_id);
        } else if (rc == ETP_ERR_AUTH) {
            format_response(response, resp_size, RESP_ERROR,
                           "You can only cancel your own bookings");
        } else {
            format_response(response, resp_size, RESP_ERROR, "Cancellation failed");
        }
        return;
    }

    /* ── MY_BOOKINGS ───────────────────────────────────────── */
    case CMD_MY_BOOKINGS: {
        booking_record_t bookings[20];
        int count = booking_list_by_user(ctx->booking_engine, session->user_id,
                                          bookings, 20);
        if (count == 0) {
            format_response(response, resp_size, RESP_OK, "No active bookings");
            return;
        }
        int off = snprintf(response, resp_size,
            "OK Your bookings (%d):\n"
            "  %-8s %-8s %-6s %-10s\n"
            "  ──────── ──────── ────── ──────────\n",
            count, "BookID", "EventID", "Seats", "Amount");
        for (int i = 0; i < count && off < (int)resp_size - 60; i++) {
            off += snprintf(response + off, resp_size - off,
                "  %-8u %-8u %-6u Rs.%.2f\n",
                bookings[i].booking_id, bookings[i].event_id,
                bookings[i].seat_count, bookings[i].total_amount);
        }
        return;
    }

    /* ── REVENUE [event_id] ────────────────────────────────── */
    case CMD_REVENUE: {
        if (cmd->argc >= 1) {
            event_revenue_t rev;
            rc = reports_event_revenue(ctx->reports, (uint32_t)atoi(cmd->args[0]), &rev);
            if (rc == ETP_OK) {
                char report[512];
                reports_format_revenue(&rev, report, sizeof(report));
                format_response(response, resp_size, RESP_OK, "\n%s", report);
            } else {
                format_response(response, resp_size, RESP_ERROR, "Event not found");
            }
        } else {
            /* Show revenue for all events by this organizer */
            event_revenue_t revs[20];
            int count = reports_organizer_revenue(ctx->reports, session->user_id,
                                                   revs, 20);
            if (count == 0) {
                format_response(response, resp_size, RESP_OK, "No events found");
                return;
            }
            int off = snprintf(response, resp_size, "OK Revenue for your events:\n");
            for (int i = 0; i < count && off < (int)resp_size - 200; i++) {
                char report[512];
                reports_format_revenue(&revs[i], report, sizeof(report));
                off += snprintf(response + off, resp_size - off, "%s\n", report);
            }
        }
        return;
    }

    /* ── LIST_USERS (admin only) ───────────────────────────── */
    case CMD_LIST_USERS: {
        user_record_t users[50];
        int count = table_scan(ctx->users_table, NULL, NULL, users, 50);
        if (count == 0) {
            format_response(response, resp_size, RESP_OK, "No users");
            return;
        }
        const char *role_names[] = {"GUEST","CUSTOMER","ORGANIZER","ADMIN"};
        int off = snprintf(response, resp_size,
            "OK %d user(s):\n"
            "  %-5s %-20s %-10s\n"
            "  ───── ──────────────────── ──────────\n",
            count, "ID", "Username", "Role");
        for (int i = 0; i < count && off < (int)resp_size - 60; i++) {
            off += snprintf(response + off, resp_size - off,
                "  %-5u %-20s %-10s\n",
                users[i].user_id, users[i].username,
                users[i].role <= ROLE_ADMIN ? role_names[users[i].role] : "?");
        }
        return;
    }

    /* ── SYSTEM_STATS (admin only) ─────────────────────────── */
    case CMD_SYSTEM_STATS: {
        server_stats_t snapshot;
        stats_get_snapshot(ctx->stats, &snapshot);
        stats_format(&snapshot, response, resp_size);
        return;
    }

    /* ── HELP ──────────────────────────────────────────────── */
    case CMD_HELP: {
        const char *role_str =
            session->role == ROLE_ADMIN     ? "ADMIN" :
            session->role == ROLE_ORGANIZER ? "ORGANIZER" :
            session->role == ROLE_CUSTOMER  ? "CUSTOMER" : "GUEST";
        int off = snprintf(response, resp_size,
            "OK Available commands (role: %s):\n"
            "  ── Auth ─────────────────────────────────────────────\n"
            "  REGISTER <user> <pass> [customer|organizer]\n"
            "  LOGIN    <user> <pass>\n"
            "  LOGOUT\n"
            "  ── Events ───────────────────────────────────────────\n"
            "  LIST_EVENTS\n"
            "  VIEW_EVENT  <event_id>\n"
            "  VIEW_SEATS  <event_id>\n",
            role_str);
        if (session->role >= ROLE_ORGANIZER) {
            off += snprintf(response + off, resp_size - off,
                "  CREATE_EVENT <name> <venue> <date> <time> <rows> <seats_per_row> <price>\n"
                "  DELETE_EVENT <event_id>\n");
        }
        if (session->role >= ROLE_CUSTOMER) {
            off += snprintf(response + off, resp_size - off,
                "  ── Booking ──────────────────────────────────────────\n"
                "  BOOK     <event_id> <seat_id1> [seat_id2] ...\n"
                "  CANCEL   <booking_id>\n"
                "  MY_BOOKINGS\n");
        }
        if (session->role >= ROLE_ORGANIZER) {
            off += snprintf(response + off, resp_size - off,
                "  ── Reports ──────────────────────────────────────────\n"
                "  REVENUE  [event_id]\n");
        }
        if (session->role == ROLE_ADMIN) {
            off += snprintf(response + off, resp_size - off,
                "  ── Admin ────────────────────────────────────────────\n"
                "  LIST_USERS\n"
                "  SYSTEM_STATS\n");
        }
        snprintf(response + off, resp_size - off,
            "  ── General ──────────────────────────────────────────\n"
            "  HELP\n"
            "  QUIT\n");
        return;
    }

    default:
        format_response(response, resp_size, RESP_ERROR,
                       "Unknown command. Type HELP for usage.");
        return;
    }
}

/* ================================================================
 * Server Initialization
 * ================================================================ */
static int server_init(server_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    etp_log(LOG_INFO, "server: initializing...");

    /* Create data directory */
    etp_ensure_dir(DATA_DIR);

    /* Buffer pool */
    ctx->buffer_pool = buffer_pool_create(BUFFER_POOL_FRAMES);
    if (!ctx->buffer_pool) {
        etp_log(LOG_FATAL, "server: buffer pool creation failed");
        return -1;
    }

    /* WAL */
    if (etp_file_exists(WAL_FILE)) {
        ctx->wal = wal_open(WAL_FILE);
    } else {
        ctx->wal = wal_create(WAL_FILE);
    }
    if (!ctx->wal) {
        etp_log(LOG_FATAL, "server: WAL initialization failed");
        return -1;
    }

    /* Tables — open if files exist, create otherwise */
    #define OPEN_OR_CREATE_TABLE(var, tid) do { \
        if (etp_file_exists(etp_data_file(tid))) { \
            var = table_open(tid, etp_data_file(tid), etp_index_file(tid), \
                             ctx->buffer_pool, ctx->wal); \
        } else { \
            var = table_create(tid, etp_data_file(tid), etp_index_file(tid), \
                               ctx->buffer_pool, ctx->wal); \
        } \
    } while(0)

    OPEN_OR_CREATE_TABLE(ctx->users_table, TABLE_USERS);
    OPEN_OR_CREATE_TABLE(ctx->events_table, TABLE_EVENTS);
    OPEN_OR_CREATE_TABLE(ctx->seats_table, TABLE_SEATS);
    OPEN_OR_CREATE_TABLE(ctx->bookings_table, TABLE_BOOKINGS);
    OPEN_OR_CREATE_TABLE(ctx->booking_seats_table, TABLE_BOOKING_SEATS);

    if (!ctx->users_table || !ctx->events_table || !ctx->seats_table ||
        !ctx->bookings_table || !ctx->booking_seats_table) {
        etp_log(LOG_FATAL, "server: table creation failed");
        return -1;
    }

    /* Lock manager + Transaction manager */
    ctx->lock_mgr = lock_mgr_create();
    ctx->txn_mgr = txn_mgr_create(ctx->lock_mgr, ctx->wal);

    /* Auth */
    ctx->auth_mgr = auth_create(ctx->users_table);
    auth_bootstrap_admin(ctx->auth_mgr);

    /* Business logic */
    ctx->event_mgr = event_mgr_create(ctx->events_table, ctx->seats_table);
    ctx->booking_engine = booking_engine_create(ctx->bookings_table,
                            ctx->booking_seats_table, ctx->seats_table,
                            ctx->events_table, ctx->txn_mgr);
    ctx->reports = reports_create(ctx->events_table, ctx->seats_table,
                                   ctx->bookings_table);

    /* IPC */
    ctx->logger   = pipe_logger_create(SERVER_LOG_FILE);
    ctx->notifier = notifier_create();
    ctx->stats    = shm_stats_create();

    if (ctx->logger)
        pipe_logger_write(ctx->logger, "Server initialized successfully");

    etp_log(LOG_INFO, "server: all subsystems initialized");
    return 0;
}

/* ================================================================
 * Server Shutdown
 * ================================================================ */
static void server_shutdown(server_context_t *ctx) {
    etp_log(LOG_INFO, "server: shutting down...");

    if (ctx->logger)
        pipe_logger_write(ctx->logger, "Server shutting down");

    /* Network */
    if (ctx->tcp_server)    tcp_server_destroy(ctx->tcp_server);

    /* IPC */
    if (ctx->stats)         shm_stats_destroy(ctx->stats);
    if (ctx->notifier)      notifier_destroy(ctx->notifier);
    if (ctx->logger)        pipe_logger_destroy(ctx->logger);

    /* Business logic */
    if (ctx->reports)       reports_destroy(ctx->reports);
    if (ctx->booking_engine) booking_engine_destroy(ctx->booking_engine);
    if (ctx->event_mgr)    event_mgr_destroy(ctx->event_mgr);

    /* Auth */
    if (ctx->auth_mgr)     auth_destroy(ctx->auth_mgr);

    /* Concurrency */
    if (ctx->txn_mgr)      txn_mgr_destroy(ctx->txn_mgr);
    if (ctx->lock_mgr)     lock_mgr_destroy(ctx->lock_mgr);

    /* Storage (flush everything to disk) */
    if (ctx->buffer_pool)  buffer_pool_flush_all(ctx->buffer_pool);

    if (ctx->booking_seats_table) table_close(ctx->booking_seats_table);
    if (ctx->bookings_table)      table_close(ctx->bookings_table);
    if (ctx->seats_table)         table_close(ctx->seats_table);
    if (ctx->events_table)        table_close(ctx->events_table);
    if (ctx->users_table)         table_close(ctx->users_table);

    if (ctx->buffer_pool)  buffer_pool_destroy(ctx->buffer_pool);
    if (ctx->wal)          wal_close(ctx->wal);

    etp_log(LOG_INFO, "server: shutdown complete");
}

/* ================================================================
 * Main Entry Point
 * ================================================================ */
int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    int threads = THREAD_POOL_SIZE;

    /* Parse optional arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            etp_set_log_level(LOG_DEBUG);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-p port] [-t threads] [-v] [-h]\n", argv[0]);
            printf("  -p port     Listen port (default: %d)\n", SERVER_PORT);
            printf("  -t threads  Worker threads (default: %d)\n", THREAD_POOL_SIZE);
            printf("  -v          Verbose (debug) logging\n");
            printf("  -h          Show this help\n");
            return 0;
        }
    }

    printf("\n  Event Ticketing Platform v1.0\n");
    printf("  ─────────────────────────────\n\n");

    /* Install signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize all subsystems */
    if (server_init(&g_ctx) != 0) {
        fprintf(stderr, "  FATAL: Server initialization failed.\n");
        server_shutdown(&g_ctx);
        return 1;
    }

    /* Create and start TCP server */
    g_ctx.tcp_server = tcp_server_create(port, threads, client_handler, &g_ctx);
    if (!g_ctx.tcp_server) {
        fprintf(stderr, "  FATAL: Could not start TCP server on port %d.\n", port);
        server_shutdown(&g_ctx);
        return 1;
    }

    /* This blocks until shutdown */
    tcp_server_start(g_ctx.tcp_server);

    /* Graceful shutdown */
    server_shutdown(&g_ctx);
    printf("  Server stopped.\n\n");

    return 0;
}
