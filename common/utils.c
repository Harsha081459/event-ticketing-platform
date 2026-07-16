/*
 * ============================================================================
 * Event Ticketing Platform — Utility Function Implementations
 * ============================================================================
 */

#include "utils.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

/* ================================================================
 * Logging
 * ================================================================ */

static log_level_t g_log_level = LOG_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void etp_set_log_level(log_level_t level) {
    g_log_level = level;
}

const char *etp_log_level_str(log_level_t level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
        default:        return "?????";
    }
}

void etp_log(log_level_t level, const char *fmt, ...) {
    if (level < g_log_level) return;

    /* Get current time */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    /* Thread-safe print */
    pthread_mutex_lock(&g_log_mutex);

    fprintf(stderr, "[%s] [%s] ", time_str, etp_log_level_str(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);

    pthread_mutex_unlock(&g_log_mutex);

    /* Fatal errors terminate the process */
    if (level == LOG_FATAL) {
        abort();
    }
}

/* ================================================================
 * Password Hashing
 *
 * Implementation: DJB2a (xor variant) applied in two passes for
 * a 128-bit hash, output as 32-char hex string.
 * This is NOT cryptographically secure — it is sufficient for
 * demonstrating role-based auth in an academic project. Production
 * systems must use bcrypt, argon2, or scrypt.
 * ================================================================ */

static uint64_t djb2_hash(const char *str, uint64_t seed) {
    uint64_t hash = seed;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) ^ c;   /* hash * 33 ^ c */
    }
    return hash;
}

void etp_hash_password(const char *password, char *hash_out, size_t hash_size) {
    if (!password || !hash_out || hash_size < 33) {
        if (hash_out && hash_size > 0) hash_out[0] = '\0';
        return;
    }

    /* Two passes with different seeds → 128-bit hash as 32 hex chars */
    uint64_t h1 = djb2_hash(password, 5381);
    uint64_t h2 = djb2_hash(password, 7919);
    snprintf(hash_out, hash_size, "%016lx%016lx", h1, h2);
}

int etp_verify_password(const char *password, const char *stored_hash) {
    char computed[65];
    etp_hash_password(password, computed, sizeof(computed));
    return strcmp(computed, stored_hash) == 0;
}

/* ================================================================
 * ID Generation
 *
 * Thread-safe auto-increment using GCC atomic built-ins.
 * On startup, these counters are initialized by scanning existing
 * records to find the max ID per table.
 * ================================================================ */

static volatile uint32_t g_next_ids[TABLE_COUNT] = {1, 1, 1, 1, 1};

uint32_t etp_next_id(table_id_t table) {
    if (table >= TABLE_COUNT) return 0;
    return __sync_fetch_and_add(&g_next_ids[table], 1);
}

void etp_set_next_id(table_id_t table, uint32_t next) {
    if (table < TABLE_COUNT) {
        g_next_ids[table] = next;
    }
}

uint32_t etp_peek_next_id(table_id_t table) {
    if (table >= TABLE_COUNT) return 0;
    return g_next_ids[table];
}

/* ================================================================
 * Timestamp
 * ================================================================ */

int64_t etp_get_timestamp(void) {
    return (int64_t)time(NULL);
}

void etp_format_timestamp(int64_t ts, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    time_t t = (time_t)ts;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

/* ================================================================
 * File System
 * ================================================================ */

int etp_ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        etp_log(LOG_ERROR, "Failed to create directory '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int etp_file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

int64_t etp_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

/* ================================================================
 * String Utilities
 * ================================================================ */

size_t etp_strlcpy(char *dst, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t copy_len = (src_len >= size) ? (size - 1) : src_len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}

void etp_trim(char *str) {
    if (!str) return;

    /* Trim leading whitespace */
    char *start = str;
    while (isspace((unsigned char)*start)) start++;

    /* Trim trailing whitespace */
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    /* Shift to beginning if needed */
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/* ================================================================
 * Error Reporting
 * ================================================================ */

const char *etp_result_str(etp_result_t code) {
    switch (code) {
        case ETP_OK:                    return "OK";
        case ETP_ERR_GENERIC:           return "Generic error";
        case ETP_ERR_NOT_FOUND:         return "Not found";
        case ETP_ERR_DUPLICATE:         return "Duplicate entry";
        case ETP_ERR_FULL:              return "Storage full";
        case ETP_ERR_IO:                return "I/O error";
        case ETP_ERR_LOCK_CONFLICT:     return "Lock conflict";
        case ETP_ERR_AUTH:              return "Authentication failed";
        case ETP_ERR_PERMISSION:        return "Permission denied";
        case ETP_ERR_INVALID_ARG:       return "Invalid argument";
        case ETP_ERR_TXN_ABORT:         return "Transaction aborted";
        case ETP_ERR_NO_MEMORY:         return "Out of memory";
        case ETP_ERR_CORRUPTION:        return "Data corruption detected";
        case ETP_ERR_SEAT_UNAVAIL:      return "Seat unavailable";
        case ETP_ERR_ALREADY_EXISTS:    return "Already exists";
        default:                        return "Unknown error";
    }
}
