/*
 * ============================================================================
 * Event Ticketing Platform — Pipe-Based Logger (Implementation)
 * ============================================================================
 * Architecture:
 *
 *   Worker Thread 1 ──write()──┐
 *   Worker Thread 2 ──write()──┤──→ pipe[1]  ═══>  pipe[0] ──→ Writer Thread ──→ Log File
 *   Worker Thread N ──write()──┘
 *
 * The pipe acts as a bounded buffer (kernel-managed, typically 64KB).
 * If the pipe is full, write() blocks the worker — natural back-pressure.
 *
 * OS concepts demonstrated:
 *   - pipe()           — creates a unidirectional IPC channel
 *   - write()/read()   — atomic writes ≤ PIPE_BUF (4KB on Linux)
 *   - pthread_create   — dedicated I/O thread
 *   - close()          — signaling EOF to the reader
 * ============================================================================
 */

#include "logger.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* ================================================================
 * Logger Structure
 * ================================================================ */
struct pipe_logger {
    int             pipe_fds[2];    /* pipe_fds[0]=read, pipe_fds[1]=write  */
    pthread_t       writer_thread;  /* Dedicated writer thread              */
    FILE           *log_fp;         /* Output log file                      */
    volatile int    running;        /* 1 while logger is active             */
};

/* ================================================================
 * Writer Thread — reads from pipe, writes to log file
 *
 * Runs until the read end of the pipe returns 0 (EOF), which
 * happens when all write ends are closed during shutdown.
 * ================================================================ */
static void *logger_writer_fn(void *arg) {
    pipe_logger_t *logger = (pipe_logger_t *)arg;
    char buf[LOG_MSG_MAX];
    ssize_t n;

    while ((n = read(logger->pipe_fds[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';

        /* Write to log file */
        if (logger->log_fp) {
            fprintf(logger->log_fp, "%s", buf);
            fflush(logger->log_fp);
        }
    }

    return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

pipe_logger_t *pipe_logger_create(const char *log_file) {
    pipe_logger_t *logger = calloc(1, sizeof(pipe_logger_t));
    if (!logger) return NULL;

    /* Create the pipe — the core OS IPC primitive */
    if (pipe(logger->pipe_fds) < 0) {
        etp_log(LOG_ERROR, "pipe_logger: pipe() failed");
        free(logger);
        return NULL;
    }

    /* Open log file for appending */
    logger->log_fp = fopen(log_file, "a");
    if (!logger->log_fp) {
        etp_log(LOG_ERROR, "pipe_logger: cannot open '%s'", log_file);
        close(logger->pipe_fds[0]);
        close(logger->pipe_fds[1]);
        free(logger);
        return NULL;
    }

    logger->running = 1;

    /* Spawn the writer thread */
    if (pthread_create(&logger->writer_thread, NULL, logger_writer_fn, logger) != 0) {
        etp_log(LOG_ERROR, "pipe_logger: pthread_create failed");
        fclose(logger->log_fp);
        close(logger->pipe_fds[0]);
        close(logger->pipe_fds[1]);
        free(logger);
        return NULL;
    }

    etp_log(LOG_INFO, "pipe_logger: created (file='%s')", log_file);
    return logger;
}

void pipe_logger_destroy(pipe_logger_t *logger) {
    if (!logger) return;

    logger->running = 0;

    /* Close the write end — this signals EOF to the reader thread */
    close(logger->pipe_fds[1]);

    /* Wait for the writer thread to finish */
    pthread_join(logger->writer_thread, NULL);

    /* Cleanup */
    close(logger->pipe_fds[0]);
    if (logger->log_fp) fclose(logger->log_fp);
    free(logger);

    etp_log(LOG_INFO, "pipe_logger: destroyed");
}

void pipe_logger_write(pipe_logger_t *logger, const char *fmt, ...) {
    if (!logger || !logger->running) return;

    char msg[LOG_MSG_MAX];

    /* Add timestamp prefix */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    int offset = strftime(msg, 32, "[%Y-%m-%d %H:%M:%S] ", tm_info);

    /* Format the message */
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(msg + offset, sizeof(msg) - offset - 1, fmt, args);
    va_end(args);

    if (written > 0) {
        int total = offset + written;
        if (total < (int)sizeof(msg) - 1) {
            msg[total] = '\n';
            msg[total + 1] = '\0';
            total++;
        }

        /*
         * Write to the pipe. For messages ≤ PIPE_BUF (4096 on Linux),
         * write() is guaranteed to be atomic — no interleaving between
         * concurrent writers. This is an OS guarantee!
         */
        write(logger->pipe_fds[1], msg, total);
    }
}
