/*
 * ============================================================================
 * Event Ticketing Platform — Protocol Parser (Implementation)
 * ============================================================================
 * Tokenizes raw text commands and maps them to structured command types.
 * ============================================================================
 */

#include "parser.h"
#include "../../server/auth/rbac.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ================================================================
 * Parse Command
 *
 * Tokenizes by whitespace. First token is the command name,
 * remaining tokens are arguments.
 *
 * Handles:
 *   - Leading/trailing whitespace
 *   - Multiple spaces between tokens
 *   - Empty input
 *   - Commands with no arguments (e.g., "LIST_EVENTS")
 * ================================================================ */
int parse_command(const char *raw, parsed_command_t *cmd) {
    if (!raw || !cmd) return -1;

    /* Initialize */
    memset(cmd, 0, sizeof(parsed_command_t));
    cmd->type = CMD_UNKNOWN;
    cmd->argc = 0;

    /* Make a mutable copy */
    char buf[MAX_CMD_LENGTH];
    etp_strlcpy(buf, raw, sizeof(buf));

    /* Strip trailing newline/carriage return */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    /* Skip leading whitespace */
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p == '\0') return -1;  /* Empty command */

    /* Extract command name (first token) */
    char cmd_name[MAX_ARG_LEN];
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < MAX_ARG_LEN - 1) {
        cmd_name[i++] = *p++;
    }
    cmd_name[i] = '\0';

    /* Map command name to type */
    cmd->type = rbac_parse_command_type(cmd_name);

    /* Extract arguments */
    while (*p && cmd->argc < MAX_CMD_ARGS) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        /* Extract argument */
        i = 0;
        while (*p && !isspace((unsigned char)*p) && i < MAX_ARG_LEN - 1) {
            cmd->args[cmd->argc][i++] = *p++;
        }
        cmd->args[cmd->argc][i] = '\0';
        cmd->argc++;
    }

    return 0;
}

/* ================================================================
 * Format Response
 *
 * Prepends the status prefix (OK / ERROR / DENIED) to the payload.
 * ================================================================ */
int format_response(char *buf, size_t buf_size, response_status_t status,
                    const char *fmt, ...) {
    if (!buf || buf_size == 0) return -1;

    const char *prefix;
    switch (status) {
        case RESP_OK:     prefix = "OK";      break;
        case RESP_ERROR:  prefix = "ERROR";   break;
        case RESP_DENIED: prefix = "DENIED";  break;
        default:          prefix = "ERROR";   break;
    }

    /* Write prefix */
    int offset = snprintf(buf, buf_size, "%s ", prefix);
    if (offset < 0 || (size_t)offset >= buf_size) return -1;

    /* Write formatted payload */
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + offset, buf_size - offset, fmt, args);
    va_end(args);

    if (written < 0) return -1;

    /* Ensure newline at end */
    size_t total = offset + written;
    if (total < buf_size - 1) {
        buf[total] = '\n';
        buf[total + 1] = '\0';
    }

    return (int)(total + 1);
}
