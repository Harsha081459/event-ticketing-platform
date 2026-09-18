/*
 * ============================================================================
 * Event Ticketing Platform — Event Manager (Implementation)
 * ============================================================================
 */

#include "event_mgr.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 * Lifecycle
 * ================================================================ */

event_manager_t *event_mgr_create(table_t *events, table_t *seats) {
    if (!events || !seats) return NULL;

    event_manager_t *em = calloc(1, sizeof(event_manager_t));
    if (!em) return NULL;

    em->events_table = events;
    em->seats_table  = seats;

    etp_log(LOG_INFO, "event_mgr: created");
    return em;
}

void event_mgr_destroy(event_manager_t *em) {
    if (!em) return;
    free(em);
    etp_log(LOG_INFO, "event_mgr: destroyed");
}

/* ================================================================
 * Create Event + Auto-generate Seats
 *
 * Steps:
 *   1. Build event record
 *   2. Insert into events table
 *   3. Generate (total_rows × seats_per_row) seat records
 *   4. Insert each seat into seats table
 * ================================================================ */
etp_result_t event_mgr_create_event(event_manager_t *em,
                                     const char *name, const char *venue,
                                     const char *date, const char *time_str,
                                     uint32_t organizer_id,
                                     uint16_t total_rows, uint16_t seats_per_row,
                                     float price,
                                     uint32_t *out_event_id) {
    if (!em || !name || !venue || !date || !time_str) return ETP_ERR_INVALID_ARG;

    if (!isfinite(price) || price < 0) return ETP_ERR_INVALID_ARG;

    /* Validate constraints */
    if (total_rows == 0 || total_rows > MAX_SEAT_ROWS) {
        etp_log(LOG_WARN, "event_mgr: invalid total_rows=%d (max=%d)",
                total_rows, MAX_SEAT_ROWS);
        return ETP_ERR_INVALID_ARG;
    }
    if (seats_per_row == 0 || seats_per_row > MAX_SEATS_PER_ROW) {
        return ETP_ERR_INVALID_ARG;
    }

    /* Build event record */
    event_record_t evt;
    memset(&evt, 0, sizeof(evt));
    etp_strlcpy(evt.name, name, sizeof(evt.name));
    etp_strlcpy(evt.venue, venue, sizeof(evt.venue));
    etp_strlcpy(evt.event_date, date, sizeof(evt.event_date));
    etp_strlcpy(evt.event_time, time_str, sizeof(evt.event_time));
    evt.organizer_id  = organizer_id;
    evt.total_rows    = total_rows;
    evt.seats_per_row = seats_per_row;
    evt.price         = price;
    evt.status        = EVENT_ACTIVE;
    evt.is_deleted    = 0;

    /* Insert event */
    uint32_t event_id = 0;
    etp_result_t rc = table_insert(em->events_table, &evt, &event_id);
    if (rc != ETP_OK) {
        etp_log(LOG_ERROR, "event_mgr: failed to insert event '%s'", name);
        return rc;
    }

    /* Generate seat records: rows A-Z, seats 1..seats_per_row */
    int seats_created = 0;
    for (uint16_t row = 0; row < total_rows; row++) {
        for (uint16_t snum = 1; snum <= seats_per_row; snum++) {
            seat_record_t seat;
            memset(&seat, 0, sizeof(seat));
            seat.event_id    = event_id;
            seat.row_label   = 'A' + row;
            seat.seat_number = snum;
            seat.status      = SEAT_AVAILABLE;
            seat.booked_by   = 0;
            seat.is_deleted  = 0;

            uint32_t seat_id = 0;
            if (table_insert(em->seats_table, &seat, &seat_id) == ETP_OK) {
                seats_created++;
            }
        }
    }

    etp_log(LOG_INFO, "event_mgr: created event '%s' (id=%u) with %d seats",
            name, event_id, seats_created);

    if (out_event_id) *out_event_id = event_id;
    return ETP_OK;
}

/* ================================================================
 * Get Event
 * ================================================================ */
etp_result_t event_mgr_get_event(event_manager_t *em, uint32_t event_id,
                                  event_record_t *out) {
    if (!em || !out) return ETP_ERR_INVALID_ARG;
    return table_find_by_id(em->events_table, event_id, out);
}

/* ================================================================
 * List Events — filter for active events only
 * ================================================================ */
static int filter_active_events(const void *record, void *ctx) {
    (void)ctx;
    const event_record_t *evt = (const event_record_t *)record;
    return (evt->status == EVENT_ACTIVE);
}

int event_mgr_list_events(event_manager_t *em, event_record_t *results,
                           int max_results) {
    if (!em || !results) return 0;
    return table_scan(em->events_table, filter_active_events, NULL,
                      results, max_results);
}

/* ================================================================
 * Get Seats for Event — filter by event_id
 * ================================================================ */
typedef struct {
    uint32_t target_event_id;
} event_filter_ctx_t;

static int filter_by_event(const void *record, void *ctx) {
    const seat_record_t *seat = (const seat_record_t *)record;
    event_filter_ctx_t *fctx = (event_filter_ctx_t *)ctx;
    return (seat->event_id == fctx->target_event_id);
}

int event_mgr_get_seats(event_manager_t *em, uint32_t event_id,
                         seat_record_t *results, int max_results) {
    if (!em || !results) return 0;
    event_filter_ctx_t ctx = { .target_event_id = event_id };
    return table_scan(em->seats_table, filter_by_event, &ctx,
                      results, max_results);
}

/* ================================================================
 * Delete Event (soft-delete)
 * ================================================================ */
etp_result_t event_mgr_delete_event(event_manager_t *em, uint32_t event_id) {
    if (!em) return ETP_ERR_INVALID_ARG;

    etp_result_t rc = table_delete(em->events_table, event_id);
    if (rc != ETP_OK) return rc;

    etp_log(LOG_INFO, "event_mgr: deleted event %u", event_id);
    return ETP_OK;
}
