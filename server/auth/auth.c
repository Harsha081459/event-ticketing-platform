/*
 * ============================================================================
 * Event Ticketing Platform — Authentication Manager (Implementation)
 * ============================================================================
 * Session pool + credential verification against the users table.
 * Thread-safe via mutex — multiple clients can login/register concurrently.
 * ============================================================================
 */

#include "auth.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Lifecycle
 * ================================================================ */

auth_manager_t *auth_create(table_t *users_table) {
    if (!users_table) {
        etp_log(LOG_ERROR, "auth_create: NULL users_table");
        return NULL;
    }

    auth_manager_t *auth = calloc(1, sizeof(auth_manager_t));
    if (!auth) return NULL;

    auth->users_table  = users_table;
    auth->session_count = 0;
    pthread_mutex_init(&auth->mutex, NULL);

    /* Mark all session slots as inactive */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        auth->sessions[i].fd = -1;
        auth->sessions[i].authenticated = 0;
    }

    etp_log(LOG_INFO, "auth: manager created (capacity=%d sessions)", MAX_CONNECTIONS);
    return auth;
}

void auth_destroy(auth_manager_t *auth) {
    if (!auth) return;
    pthread_mutex_destroy(&auth->mutex);
    free(auth);
    etp_log(LOG_INFO, "auth: manager destroyed");
}

/* ================================================================
 * Session Management
 * ================================================================ */

session_t *auth_create_session(auth_manager_t *auth, int client_fd) {
    if (!auth) return NULL;

    pthread_mutex_lock(&auth->mutex);

    /* Find a free session slot */
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (auth->sessions[i].fd == -1) {
            session_init(&auth->sessions[i], client_fd);
            auth->sessions[i].last_active = etp_get_timestamp();
            auth->session_count++;

            etp_log(LOG_DEBUG, "auth: session created for fd=%d (slot=%d, total=%d)",
                    client_fd, i, auth->session_count);

            pthread_mutex_unlock(&auth->mutex);
            return &auth->sessions[i];
        }
    }

    pthread_mutex_unlock(&auth->mutex);
    etp_log(LOG_WARN, "auth: no free session slots for fd=%d", client_fd);
    return NULL;
}

session_t *auth_get_session(auth_manager_t *auth, int client_fd) {
    if (!auth) return NULL;

    pthread_mutex_lock(&auth->mutex);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (auth->sessions[i].fd == client_fd) {
            auth->sessions[i].last_active = etp_get_timestamp();
            pthread_mutex_unlock(&auth->mutex);
            return &auth->sessions[i];
        }
    }

    pthread_mutex_unlock(&auth->mutex);
    return NULL;
}

void auth_remove_session(auth_manager_t *auth, int client_fd) {
    if (!auth) return;

    pthread_mutex_lock(&auth->mutex);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (auth->sessions[i].fd == client_fd) {
            etp_log(LOG_DEBUG, "auth: session removed for fd=%d (user='%s')",
                    client_fd, auth->sessions[i].username);

            auth->sessions[i].fd = -1;
            auth->sessions[i].authenticated = 0;
            auth->sessions[i].user_id = 0;
            auth->sessions[i].role = ROLE_GUEST;
            auth->sessions[i].username[0] = '\0';
            auth->session_count--;
            break;
        }
    }

    pthread_mutex_unlock(&auth->mutex);
}

/* ================================================================
 * Internal: Find user by username (table scan with filter)
 * ================================================================ */

typedef struct {
    const char *target_username;
} username_filter_ctx_t;

static int filter_by_username(const void *record, void *context) {
    const user_record_t *user = (const user_record_t *)record;
    username_filter_ctx_t *ctx = (username_filter_ctx_t *)context;
    return (strcmp(user->username, ctx->target_username) == 0);
}

/* ================================================================
 * Authentication Operations
 * ================================================================ */

etp_result_t auth_register(auth_manager_t *auth, const char *username,
                            const char *password, user_role_t role,
                            uint32_t *out_user_id) {
    if (!auth || !username || !password) return ETP_ERR_INVALID_ARG;

    /* Validate input */
    if (strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return ETP_ERR_INVALID_ARG;
    }
    if (strlen(password) == 0 || strlen(password) > MAX_PASSWORD_LEN) {
        return ETP_ERR_INVALID_ARG;
    }

    /* Check if username already exists */
    username_filter_ctx_t ctx = { .target_username = username };
    user_record_t existing;
    int found = table_scan(auth->users_table, filter_by_username, &ctx,
                           &existing, 1);
    if (found > 0) {
        etp_log(LOG_WARN, "auth: registration failed — username '%s' already taken", username);
        return ETP_ERR_DUPLICATE;
    }

    /* Create new user record */
    user_record_t new_user;
    memset(&new_user, 0, sizeof(new_user));
    etp_strlcpy(new_user.username, username, sizeof(new_user.username));
    etp_hash_password(password, new_user.password_hash, sizeof(new_user.password_hash));
    new_user.role = (uint8_t)role;
    new_user.created_at = etp_get_timestamp();
    new_user.is_deleted = 0;

    uint32_t user_id = 0;
    etp_result_t rc = table_insert(auth->users_table, &new_user, &user_id);
    if (rc != ETP_OK) {
        etp_log(LOG_ERROR, "auth: failed to insert user '%s': %s", username, etp_result_str(rc));
        return rc;
    }

    if (out_user_id) *out_user_id = user_id;

    etp_log(LOG_INFO, "auth: registered user '%s' (id=%u, role=%d)", username, user_id, role);
    return ETP_OK;
}

etp_result_t auth_login(auth_manager_t *auth, int client_fd,
                         const char *username, const char *password) {
    if (!auth || !username || !password) return ETP_ERR_INVALID_ARG;

    /* Find user by username */
    username_filter_ctx_t ctx = { .target_username = username };
    user_record_t user;
    int found = table_scan(auth->users_table, filter_by_username, &ctx,
                           &user, 1);
    if (found == 0) {
        etp_log(LOG_WARN, "auth: login failed — user '%s' not found", username);
        return ETP_ERR_AUTH;
    }

    /* Verify password */
    if (!etp_verify_password(password, user.password_hash)) {
        etp_log(LOG_WARN, "auth: login failed — wrong password for '%s'", username);
        return ETP_ERR_AUTH;
    }

    /* Update session — hold the lock across the entire find+modify
     * to prevent a TOCTOU race where another thread could invalidate
     * the session between our lookup and our modification. */
    pthread_mutex_lock(&auth->mutex);
    session_t *session = NULL;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (auth->sessions[i].fd == client_fd) {
            session = &auth->sessions[i];
            break;
        }
    }
    if (!session) {
        pthread_mutex_unlock(&auth->mutex);
        etp_log(LOG_ERROR, "auth: no session for fd=%d during login", client_fd);
        return ETP_ERR_GENERIC;
    }

    session->user_id       = user.user_id;
    session->role          = user.role;
    session->authenticated = 1;
    session->last_active   = etp_get_timestamp();
    etp_strlcpy(session->username, user.username, sizeof(session->username));
    pthread_mutex_unlock(&auth->mutex);

    etp_log(LOG_INFO, "auth: user '%s' logged in (fd=%d, role=%d)",
            username, client_fd, user.role);
    return ETP_OK;
}

etp_result_t auth_logout(auth_manager_t *auth, int client_fd) {
    if (!auth) return ETP_ERR_INVALID_ARG;

    /* Hold mutex across lookup + modification (same TOCTOU prevention as login) */
    pthread_mutex_lock(&auth->mutex);
    session_t *session = NULL;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (auth->sessions[i].fd == client_fd) {
            session = &auth->sessions[i];
            break;
        }
    }
    if (!session || !session->authenticated) {
        pthread_mutex_unlock(&auth->mutex);
        return ETP_ERR_AUTH;
    }

    etp_log(LOG_INFO, "auth: user '%s' logged out (fd=%d)", session->username, client_fd);

    session->user_id       = 0;
    session->role          = ROLE_GUEST;
    session->authenticated = 0;
    session->username[0]   = '\0';
    pthread_mutex_unlock(&auth->mutex);

    return ETP_OK;
}

/* ================================================================
 * Admin Bootstrap
 * ================================================================ */

void auth_bootstrap_admin(auth_manager_t *auth) {
    if (!auth) return;

    /* Check if any users exist */
    int count = table_count(auth->users_table);
    if (count > 0) {
        etp_log(LOG_DEBUG, "auth: %d users exist, skipping admin bootstrap", count);
        return;
    }

    /* No users — create default admin */
    uint32_t admin_id = 0;
    etp_result_t rc = auth_register(auth, "admin", "admin123", ROLE_ADMIN, &admin_id);
    if (rc == ETP_OK) {
        etp_log(LOG_INFO, "auth: bootstrapped default admin (id=%u, password='admin123')", admin_id);
    } else {
        etp_log(LOG_ERROR, "auth: failed to bootstrap admin: %s", etp_result_str(rc));
    }
}
