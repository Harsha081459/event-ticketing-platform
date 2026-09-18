/*
 * ============================================================================
 * Event Ticketing Platform — Role-Based Access Control (Header)
 * ============================================================================
 * Implements a permission matrix mapping (role × command) → allowed/denied.
 * This is a mandatory OS concept: Role-Based Authorization.
 * ============================================================================
 */

#ifndef ETP_RBAC_H
#define ETP_RBAC_H

#include "../../common/types.h"
#include "../../common/protocol.h"

/*
 * Check if a given role has permission to execute a command.
 * Returns 1 if allowed, 0 if denied.
 */
int rbac_check(user_role_t role, command_type_t cmd);

/*
 * Get the human-readable name for a command type.
 */
const char *rbac_command_name(command_type_t cmd);

/*
 * Parse a command string (e.g., "LIST_EVENTS") into a command_type_t.
 * Case-insensitive. Returns CMD_UNKNOWN if not recognized.
 */
command_type_t rbac_parse_command_type(const char *cmd_str);

/*
 * Get a human-readable description of why a command was denied.
 */
const char *rbac_denial_reason(user_role_t role, command_type_t cmd);

#endif /* ETP_RBAC_H */
