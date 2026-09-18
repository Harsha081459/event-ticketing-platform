/*
 * ============================================================================
 * Event Ticketing Platform — Authentication Manager (Header)
 * ============================================================================
 * Manages user registration, login/logout, and session tracking.
 * Sessions map client file descriptors to authenticated user state.
 * ============================================================================
 */

#ifndef ETP_AUTH_H
#define ETP_AUTH_H

#include "../../common/types.h"
#include "../../common/config.h"
#include "../../common/protocol.h"
#include "../../server/storage/table.h"

#include <pthread.h>

/* ================================================================
 * Auth Manager
 * ================================================================ */
typedef struct {
    session_t       sessions[MAX_CONNECTIONS];   /* Session pool             */
    int             session_count;               /* Active sessions          */
    pthread_mutex_t mutex;                       /* Thread-safe access       */
    table_t        *users_table;                 /* Users table handle       */
} auth_manager_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create auth manager linked to a users table */
auth_manager_t *auth_create(table_t *users_table);

/* Destroy auth manager */
void auth_destroy(auth_manager_t *auth);

/* ================================================================
 * Session Management
 * ================================================================ */

/* Create a new guest session for a client connection */
session_t *auth_create_session(auth_manager_t *auth, int client_fd);

/* Get the session for a client fd (NULL if not found) */
session_t *auth_get_session(auth_manager_t *auth, int client_fd);

/* Remove session when client disconnects */
void auth_remove_session(auth_manager_t *auth, int client_fd);

/* ================================================================
 * Authentication Operations
 * ================================================================ */

/*
 * Register a new user.
 * Returns ETP_OK on success, ETP_ERR_DUPLICATE if username taken.
 */
etp_result_t auth_register(auth_manager_t *auth, const char *username,
                            const char *password, user_role_t role,
                            uint32_t *out_user_id);

/*
 * Login — verify credentials, update session to authenticated state.
 * Returns ETP_OK on success, ETP_ERR_AUTH if credentials invalid.
 */
etp_result_t auth_login(auth_manager_t *auth, int client_fd,
                         const char *username, const char *password);

/*
 * Logout — revert session to guest state.
 */
etp_result_t auth_logout(auth_manager_t *auth, int client_fd);

/* ================================================================
 * Admin: Bootstrap
 * ================================================================ */

/* Create default admin user if no users exist (first-run setup) */
void auth_bootstrap_admin(auth_manager_t *auth);

#endif /* ETP_AUTH_H */
