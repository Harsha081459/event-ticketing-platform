/*
 * ============================================================================
 * Event Ticketing Platform — Pipe-Based Logger (Header)
 * ============================================================================
 * Demonstrates OS pipe IPC: a dedicated writer thread reads log messages
 * from a pipe and appends them to a log file.
 *
 * OS concepts:
 *   - pipe()              — creating an IPC channel
 *   - write()/read()      — blocking I/O on pipe fds
 *   - Producer-consumer   — workers write, logger thread reads
 *   - Dedicated I/O thread — offloads disk I/O from workers
 * ============================================================================
 */

#ifndef ETP_LOGGER_H
#define ETP_LOGGER_H

#include <stddef.h>

/* Maximum length of a single log message sent through the pipe */
#define LOG_MSG_MAX     512

/* Opaque logger handle */
typedef struct pipe_logger pipe_logger_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/*
 * Create the pipe logger.
 * Spawns a background thread that reads from the pipe and writes to `log_file`.
 */
pipe_logger_t *pipe_logger_create(const char *log_file);

/*
 * Destroy the logger — closes pipe, joins the writer thread.
 */
void pipe_logger_destroy(pipe_logger_t *logger);

/* ================================================================
 * Logging
 * ================================================================ */

/*
 * Send a log message through the pipe.
 * Thread-safe — multiple workers can call this concurrently.
 * The message is asynchronously written to the log file by the writer thread.
 */
void pipe_logger_write(pipe_logger_t *logger, const char *fmt, ...);

#endif /* ETP_LOGGER_H */
