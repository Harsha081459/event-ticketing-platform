/*
 * ============================================================================
 * Event Ticketing Platform — Role-Based Access Control (Implementation)
 * ============================================================================
 * Permission matrix: each cell is 1 (allowed) or 0 (denied).
 *
 *   Roles:    GUEST  CUSTOMER  ORGANIZER  ADMIN
 *   ─────────────────────────────────────────────
 *   REGISTER    ✓       ✓         ✓         ✓
 *   LOGIN       ✓       ✓         ✓         ✓
 *   LOGOUT      ✗       ✓         ✓         ✓
 *   LIST_EVENTS ✓       ✓         ✓         ✓
 *   VIEW_EVENT  ✓       ✓         ✓         ✓
 *   VIEW_SEATS  ✓       ✓         ✓         ✓
 *   CREATE_EVT  ✗       ✗         ✓         ✓
 *   DELETE_EVT  ✗       ✗         ✓         ✓
 *   BOOK        ✗       ✓         ✓         ✓
 *   CANCEL      ✗       ✓         ✓         ✓
 *   MY_BOOKINGS ✗       ✓         ✓         ✓
 *   LIST_USERS  ✗       ✗         ✗         ✓
 *   REVENUE     ✗       ✗         ✓         ✓
 *   SYS_STATS   ✗       ✗         ✗         ✓
 *   HELP        ✓       ✓         ✓         ✓
 *   QUIT        ✓       ✓         ✓         ✓
 * ============================================================================
 */

#include "rbac.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* ================================================================
 * Permission Matrix
 *
 * Indexed as: permission_matrix[command][role]
 * ================================================================ */
static const int permission_matrix[CMD_COUNT][4] = {
    /*                    GUEST  CUSTOMER  ORGANIZER  ADMIN */
    /* CMD_REGISTER    */ { 1,      1,        1,        1 },
    /* CMD_LOGIN       */ { 1,      1,        1,        1 },
    /* CMD_LOGOUT      */ { 0,      1,        1,        1 },
    /* CMD_LIST_EVENTS */ { 1,      1,        1,        1 },
    /* CMD_VIEW_EVENT  */ { 1,      1,        1,        1 },
    /* CMD_VIEW_SEATS  */ { 1,      1,        1,        1 },
    /* CMD_CREATE_EVENT*/ { 0,      0,        1,        1 },
    /* CMD_DELETE_EVENT*/ { 0,      0,        1,        1 },
    /* CMD_BOOK        */ { 0,      1,        1,        1 },
    /* CMD_CANCEL      */ { 0,      1,        1,        1 },
    /* CMD_MY_BOOKINGS */ { 0,      1,        1,        1 },
    /* CMD_LIST_USERS  */ { 0,      0,        0,        1 },
    /* CMD_REVENUE     */ { 0,      0,        1,        1 },
    /* CMD_SYSTEM_STATS*/ { 0,      0,        0,        1 },
    /* CMD_HELP        */ { 1,      1,        1,        1 },
    /* CMD_QUIT        */ { 1,      1,        1,        1 },
    /* CMD_UNKNOWN     */ { 0,      0,        0,        0 },
};

/* ================================================================
 * Command Name Table
 * ================================================================ */
static const char *command_names[CMD_COUNT] = {
    "REGISTER",
    "LOGIN",
    "LOGOUT",
    "LIST_EVENTS",
    "VIEW_EVENT",
    "VIEW_SEATS",
    "CREATE_EVENT",
    "DELETE_EVENT",
    "BOOK",
    "CANCEL",
    "MY_BOOKINGS",
    "LIST_USERS",
    "REVENUE",
    "SYSTEM_STATS",
    "HELP",
    "QUIT",
    "UNKNOWN"
};

static const char *role_names[] = {
    "GUEST", "CUSTOMER", "ORGANIZER", "ADMIN"
};

/* ================================================================
 * Public API
 * ================================================================ */

int rbac_check(user_role_t role, command_type_t cmd) {
    if (cmd < 0 || cmd >= CMD_COUNT || role > ROLE_ADMIN) {
        return 0;  /* Deny unknown commands/roles */
    }
    return permission_matrix[cmd][role];
}

const char *rbac_command_name(command_type_t cmd) {
    if (cmd >= 0 && cmd < CMD_COUNT) {
        return command_names[cmd];
    }
    return "UNKNOWN";
}

command_type_t rbac_parse_command_type(const char *cmd_str) {
    if (!cmd_str) return CMD_UNKNOWN;

    for (int i = 0; i < CMD_COUNT; i++) {
        if (strcasecmp(cmd_str, command_names[i]) == 0) {
            return (command_type_t)i;
        }
    }
    return CMD_UNKNOWN;
}

const char *rbac_denial_reason(user_role_t role, command_type_t cmd) {
    /* __thread makes this thread-local, preventing race conditions
     * when multiple worker threads call this concurrently */
    static __thread char buf[256];

    if (role == ROLE_GUEST && (cmd == CMD_BOOK || cmd == CMD_CANCEL ||
                               cmd == CMD_MY_BOOKINGS || cmd == CMD_LOGOUT)) {
        return "Please LOGIN first to use this command";
    }

    if (role == ROLE_CUSTOMER && (cmd == CMD_CREATE_EVENT || cmd == CMD_DELETE_EVENT)) {
        return "Only ORGANIZERs can manage events";
    }

    if (role != ROLE_ADMIN && (cmd == CMD_LIST_USERS || cmd == CMD_SYSTEM_STATS)) {
        return "ADMIN access required";
    }

    snprintf(buf, sizeof(buf), "Role '%s' cannot execute '%s'",
             (role <= ROLE_ADMIN) ? role_names[role] : "?",
             rbac_command_name(cmd));
    return buf;
}
