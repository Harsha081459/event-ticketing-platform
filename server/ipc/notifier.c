/*
 * ============================================================================
 * Event Ticketing Platform — Message Queue Notifier (Implementation)
 * ============================================================================
 * Uses System V Message Queues for asynchronous notification delivery.
 *
 * Architecture:
 *
 *   Booking Engine ──msgsnd()──→ Message Queue ──msgrcv()──→ Consumer Thread
 *   Event Manager  ──msgsnd()──→        ↑                        │
 *                                  (kernel-managed)         [process notification]
 *
 * The message queue is identified by a key (ETP_MSG_KEY from config.h).
 * Multiple producers can send messages concurrently (kernel handles sync).
 * The consumer thread reads messages by type and processes them.
 *
 * OS concepts:
 *   - msgget(key, IPC_CREAT)  — create or open a message queue
 *   - msgsnd(qid, msg, size)  — send a typed message
 *   - msgrcv(qid, msg, size, type) — receive by type (0 = any)
 *   - msgctl(qid, IPC_RMID)  — remove queue on shutdown
 * ============================================================================
 */

#include "notifier.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

/* ================================================================
 * Notifier Structure
 * ================================================================ */
struct notifier {
    int             msg_queue_id;   /* System V message queue ID         */
    pthread_t       consumer;       /* Consumer thread                   */
    volatile int    running;        /* 1 while active                    */
};

/* ================================================================
 * Consumer Thread — reads notifications from the queue
 *
 * Uses msgrcv() with type=0 to receive ANY message type.
 * Processes each notification (in production, this would trigger
 * email/SMS/push notifications; here we log them).
 * ================================================================ */
static void *consumer_thread(void *arg) {
    notifier_t *n = (notifier_t *)arg;
    notification_msg_t msg;

    etp_log(LOG_DEBUG, "notifier: consumer thread started");

    while (n->running) {
        /*
         * msgrcv() blocks until a message is available.
         * type=0 means "receive any message type".
         * IPC_NOWAIT is NOT set — this is blocking I/O.
         *
         * On shutdown, we send a special SYSTEM_ALERT to wake up
         * the consumer, or use msgctl(IPC_RMID) which causes
         * msgrcv to return with EIDRM.
         */
        ssize_t rc = msgrcv(n->msg_queue_id, &msg,
                            sizeof(msg.payload), 0, 0);

        if (rc < 0) {
            if (errno == EIDRM || errno == EINTR) {
                break;  /* Queue removed or interrupted — shutdown */
            }
            etp_log(LOG_ERROR, "notifier: msgrcv failed: %s", strerror(errno));
            continue;
        }

        /* Process the notification */
        const char *type_str;
        switch (msg.mtype) {
            case NOTIFY_BOOKING_CONFIRMED:  type_str = "BOOKING_CONFIRMED"; break;
            case NOTIFY_BOOKING_CANCELLED:  type_str = "BOOKING_CANCELLED"; break;
            case NOTIFY_EVENT_CREATED:      type_str = "EVENT_CREATED";     break;
            case NOTIFY_SYSTEM_ALERT:       type_str = "SYSTEM_ALERT";      break;
            default:                        type_str = "UNKNOWN";           break;
        }

        etp_log(LOG_INFO, "notifier: [%s] %s", type_str, msg.payload);

        /*
         * In a production system, this is where you'd dispatch to:
         *   - Email service (SMTP)
         *   - SMS gateway
         *   - Push notification service
         *   - WebSocket broadcast
         * For this project, logging demonstrates the IPC mechanism.
         */
    }

    etp_log(LOG_DEBUG, "notifier: consumer thread exiting");
    return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

notifier_t *notifier_create(void) {
    notifier_t *n = calloc(1, sizeof(notifier_t));
    if (!n) return NULL;

    /*
     * Create (or open) the System V message queue.
     * IPC_CREAT: create if doesn't exist
     * 0666: read/write for owner, group, others
     */
    n->msg_queue_id = msgget(ETP_MSG_KEY, IPC_CREAT | 0666);
    if (n->msg_queue_id < 0) {
        etp_log(LOG_ERROR, "notifier: msgget() failed: %s", strerror(errno));
        free(n);
        return NULL;
    }

    n->running = 1;

    /* Spawn consumer thread */
    if (pthread_create(&n->consumer, NULL, consumer_thread, n) != 0) {
        etp_log(LOG_ERROR, "notifier: pthread_create failed");
        msgctl(n->msg_queue_id, IPC_RMID, NULL);
        free(n);
        return NULL;
    }

    etp_log(LOG_INFO, "notifier: created (queue_id=%d, key=0x%X)",
            n->msg_queue_id, ETP_MSG_KEY);
    return n;
}

void notifier_destroy(notifier_t *n) {
    if (!n) return;

    n->running = 0;

    /* Remove the message queue — this wakes up msgrcv() with EIDRM */
    if (n->msg_queue_id >= 0) {
        msgctl(n->msg_queue_id, IPC_RMID, NULL);
    }

    pthread_join(n->consumer, NULL);
    free(n);

    etp_log(LOG_INFO, "notifier: destroyed");
}

/* ================================================================
 * Internal: Send a message to the queue
 * ================================================================ */
static void send_notification(notifier_t *n, long mtype, const char *text) {
    if (!n || !n->running) return;

    notification_msg_t msg;
    msg.mtype = mtype;
    etp_strlcpy(msg.payload, text, sizeof(msg.payload));

    /*
     * msgsnd() adds the message to the queue.
     * IPC_NOWAIT: don't block if queue is full (drop the message).
     * In production, you might want to block or retry.
     */
    if (msgsnd(n->msg_queue_id, &msg, sizeof(msg.payload), IPC_NOWAIT) < 0) {
        etp_log(LOG_WARN, "notifier: msgsnd failed: %s", strerror(errno));
    }
}

/* ================================================================
 * Notification Helpers
 * ================================================================ */

void notify_booking_confirmed(notifier_t *n, uint32_t booking_id,
                               uint32_t user_id, uint32_t event_id,
                               int num_seats) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Booking #%u confirmed: user %u booked %d seats for event %u",
             booking_id, user_id, num_seats, event_id);
    send_notification(n, NOTIFY_BOOKING_CONFIRMED, buf);
}

void notify_booking_cancelled(notifier_t *n, uint32_t booking_id,
                                uint32_t user_id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Booking #%u cancelled by user %u", booking_id, user_id);
    send_notification(n, NOTIFY_BOOKING_CANCELLED, buf);
}

void notify_event_created(notifier_t *n, uint32_t event_id,
                           const char *event_name) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "New event created: '%s' (ID: %u)", event_name, event_id);
    send_notification(n, NOTIFY_EVENT_CREATED, buf);
}

void notify_system_alert(notifier_t *n, const char *message) {
    send_notification(n, NOTIFY_SYSTEM_ALERT, message);
}
