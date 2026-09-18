/*
 * ============================================================================
 * Event Ticketing Platform — Utility Function Declarations
 * ============================================================================
 */

#ifndef ETP_UTILS_H
#define ETP_UTILS_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"

/* ================================================================
 * Logging
 * ================================================================ */
typedef enum {
    LOG_DEBUG   = 0,
    LOG_INFO    = 1,
    LOG_WARN    = 2,
    LOG_ERROR   = 3,
    LOG_FATAL   = 4
} log_level_t;

void        etp_log(log_level_t level, const char *fmt, ...);
void        etp_set_log_level(log_level_t level);
const char *etp_log_level_str(log_level_t level);

/* ================================================================
 * Password Hashing
 *
 * Uses a simple hash for demonstration purposes.
 * Production systems should use bcrypt, argon2, or scrypt.
 * ================================================================ */
void    etp_hash_password(const char *password, char *hash_out, size_t hash_size);
int     etp_verify_password(const char *password, const char *stored_hash);

/* ================================================================
 * ID Generation (thread-safe auto-increment)
 * ================================================================ */
uint32_t    etp_next_id(table_id_t table);
void        etp_set_next_id(table_id_t table, uint32_t next);
uint32_t    etp_peek_next_id(table_id_t table);

/* ================================================================
 * Timestamp
 * ================================================================ */
int64_t     etp_get_timestamp(void);
void        etp_format_timestamp(int64_t ts, char *buf, size_t buf_size);

/* ================================================================
 * File System
 * ================================================================ */
int         etp_ensure_dir(const char *path);
int         etp_file_exists(const char *path);
int64_t     etp_file_size(const char *path);

/* ================================================================
 * String Utilities
 * ================================================================ */
size_t      etp_strlcpy(char *dst, const char *src, size_t size);
void        etp_trim(char *str);

/* ================================================================
 * Error Reporting
 * ================================================================ */
const char *etp_result_str(etp_result_t code);

#endif /* ETP_UTILS_H */
