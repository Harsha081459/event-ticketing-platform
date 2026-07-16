/*
 * ============================================================================
 * Event Ticketing Platform — Protocol Definitions
 * ============================================================================
 * Shared command types, response codes, session structure, and parsed
 * command format used by the parser, RBAC, auth, and TCP server modules.
 * ============================================================================
 */

#ifndef ETP_PROTOCOL_H
#define ETP_PROTOCOL_H

#include "types.h"

/* ================================================================
 * Protocol Constants
 * ================================================================ */
#define MAX_CMD_ARGS        16          /* Max arguments per command      */
#define MAX_ARG_LEN         256         /* Max length of each argument   */
#define RESPONSE_BUFFER_SIZE 65536      /* 64 KB response buffer         */

/* ================================================================
 * Command Types — every action the system supports
 * ================================================================ */
typedef enum {
    /* Auth commands */
    CMD_REGISTER        = 0,
    CMD_LOGIN           = 1,
    CMD_LOGOUT          = 2,

    /* Event commands (read) */
    CMD_LIST_EVENTS     = 3,
    CMD_VIEW_EVENT      = 4,
    CMD_VIEW_SEATS      = 5,

    /* Event commands (write) */
    CMD_CREATE_EVENT    = 6,
    CMD_DELETE_EVENT    = 7,

    /* Booking commands */
    CMD_BOOK            = 8,
    CMD_CANCEL          = 9,
    CMD_MY_BOOKINGS     = 10,

    /* Admin commands */
    CMD_LIST_USERS      = 11,
    CMD_REVENUE         = 12,
    CMD_SYSTEM_STATS    = 13,

    /* Utility commands */
    CMD_HELP            = 14,
    CMD_QUIT            = 15,

    CMD_UNKNOWN         = 16,
    CMD_COUNT           = 17
} command_type_t;

/* ================================================================
 * Response Status
 * ================================================================ */
typedef enum {
    RESP_OK     = 0,    /* Success — payload follows      */
    RESP_ERROR  = 1,    /* Error — error message follows   */
    RESP_DENIED = 2     /* Permission denied               */
} response_status_t;

/* ================================================================
 * Parsed Command — result of parsing a raw text command
 * ================================================================ */
typedef struct {
    command_type_t  type;
    int             argc;                       /* Number of arguments   */
    char            args[MAX_CMD_ARGS][MAX_ARG_LEN];  /* Argument values */
} parsed_command_t;

/* ================================================================
 * Client Session — tracks a connected client's auth state
 * ================================================================ */
typedef struct {
    int             fd;             /* Client socket file descriptor     */
    uint32_t        user_id;        /* 0 if not logged in                */
    uint8_t         role;           /* user_role_t (GUEST if not authed) */
    char            username[32];   /* Empty if not logged in            */
    int64_t         last_active;    /* Timestamp for session timeout     */
    int             authenticated;  /* 1 if logged in                    */
} session_t;

/* ================================================================
 * Session Initialization
 * ================================================================ */
static inline void session_init(session_t *s, int fd) {
    s->fd            = fd;
    s->user_id       = 0;
    s->role           = ROLE_GUEST;
    s->username[0]   = '\0';
    s->last_active   = 0;
    s->authenticated = 0;
}

#endif /* ETP_PROTOCOL_H */
