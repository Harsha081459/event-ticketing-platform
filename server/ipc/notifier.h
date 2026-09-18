/*
 * ============================================================================
 * Event Ticketing Platform — Message Queue Notifier (Header)
 * ============================================================================
 * Demonstrates System V Message Queues for inter-process communication.
 * Sends booking notifications (confirmations, cancellations) through a
 * message queue. A dedicated consumer thread processes notifications.
 *
 * OS concepts:
 *   - msgget()    — create/access a message queue
 *   - msgsnd()    — send a message to the queue
 *   - msgrcv()    — receive a message from the queue
 *   - msgctl()    — remove the queue on shutdown
 * ============================================================================
 */

#ifndef ETP_NOTIFIER_H
#define ETP_NOTIFIER_H

#include <stdint.h>

/* Message types for the queue */
#define NOTIFY_BOOKING_CONFIRMED    1L
#define NOTIFY_BOOKING_CANCELLED    2L
#define NOTIFY_EVENT_CREATED        3L
#define NOTIFY_SYSTEM_ALERT         4L

/* Notification message payload */
typedef struct {
    long    mtype;              /* Message type (required by System V)   */
    char    payload[256];       /* Human-readable notification text      */
} notification_msg_t;

/* Opaque notifier handle */
typedef struct notifier notifier_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create the notifier — initializes message queue + consumer thread */
notifier_t *notifier_create(void);

/* Destroy the notifier — removes message queue, joins consumer */
void notifier_destroy(notifier_t *notifier);

/* ================================================================
 * Send Notifications
 * ================================================================ */

/* Send a booking confirmation notification */
void notify_booking_confirmed(notifier_t *n, uint32_t booking_id,
                               uint32_t user_id, uint32_t event_id,
                               int num_seats);

/* Send a booking cancellation notification */
void notify_booking_cancelled(notifier_t *n, uint32_t booking_id,
                                uint32_t user_id);

/* Send an event creation notification */
void notify_event_created(notifier_t *n, uint32_t event_id,
                           const char *event_name);

/* Send a generic system alert */
void notify_system_alert(notifier_t *n, const char *message);

#endif /* ETP_NOTIFIER_H */
