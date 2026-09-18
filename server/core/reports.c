/*
 * ============================================================================
 * Event Ticketing Platform — Reports Engine (Implementation)
 * ============================================================================
 */

#include "reports.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Lifecycle
 * ================================================================ */

reports_engine_t *reports_create(table_t *events, table_t *seats, table_t *bookings) {
    if (!events || !seats || !bookings) return NULL;

    reports_engine_t *re = calloc(1, sizeof(reports_engine_t));
    if (!re) return NULL;

    re->events_table   = events;
    re->seats_table    = seats;
    re->bookings_table = bookings;

    etp_log(LOG_INFO, "reports: engine created");
    return re;
}

void reports_destroy(reports_engine_t *re) {
    if (!re) return;
    free(re);
}

/* ================================================================
 * Event Revenue Report
 *
 * Scans all seats for the event, counts booked vs total,
 * calculates revenue from booking count × price.
 * ================================================================ */

typedef struct { uint32_t target_event_id; } evt_ctx_t;

static int filter_seats_by_event(const void *record, void *ctx) {
    const seat_record_t *s = (const seat_record_t *)record;
    evt_ctx_t *c = (evt_ctx_t *)ctx;
    return (s->event_id == c->target_event_id);
}

etp_result_t reports_event_revenue(reports_engine_t *re, uint32_t event_id,
                                    event_revenue_t *out) {
    if (!re || !out) return ETP_ERR_INVALID_ARG;

    /* Get event details */
    event_record_t evt;
    etp_result_t rc = table_find_by_id(re->events_table, event_id, &evt);
    if (rc != ETP_OK) return rc;

    /* Heap-allocate to avoid stack overflow (2600 × 32 = 83KB is too much for stack) */
    evt_ctx_t ctx = { .target_event_id = event_id };
    int max_seats = 2600;
    seat_record_t *seats = malloc(max_seats * sizeof(seat_record_t));
    if (!seats) return ETP_ERR_GENERIC;

    int total = table_scan(re->seats_table, filter_seats_by_event, &ctx,
                           seats, max_seats);

    int booked = 0;
    for (int i = 0; i < total; i++) {
        if (seats[i].status == SEAT_BOOKED) {
            booked++;
        }
    }
    free(seats);

    /* Fill report */
    memset(out, 0, sizeof(*out));
    out->event_id      = event_id;
    etp_strlcpy(out->event_name, evt.name, sizeof(out->event_name));
    out->total_seats   = total;
    out->booked_seats  = booked;
    out->occupancy_pct = (total > 0) ? ((float)booked / total * 100.0f) : 0.0f;
    out->total_revenue = booked * evt.price;

    return ETP_OK;
}

/* ================================================================
 * Organizer Revenue — revenue for all events by an organizer
 * ================================================================ */

typedef struct { uint32_t target_org_id; } org_ctx_t;

static int filter_by_organizer(const void *record, void *ctx) {
    const event_record_t *e = (const event_record_t *)record;
    org_ctx_t *c = (org_ctx_t *)ctx;
    return (e->organizer_id == c->target_org_id && e->status == EVENT_ACTIVE);
}

int reports_organizer_revenue(reports_engine_t *re, uint32_t organizer_id,
                               event_revenue_t *results, int max_results) {
    if (!re || !results) return 0;

    /* Get all events by this organizer */
    org_ctx_t ctx = { .target_org_id = organizer_id };
    event_record_t events[100];
    int num_events = table_scan(re->events_table, filter_by_organizer, &ctx,
                                events, 100);

    int count = 0;
    for (int i = 0; i < num_events && count < max_results; i++) {
        if (reports_event_revenue(re, events[i].event_id, &results[count]) == ETP_OK) {
            count++;
        }
    }

    return count;
}

/* ================================================================
 * Format Revenue Report — human-readable output
 * ================================================================ */
int reports_format_revenue(event_revenue_t *report, char *buf, size_t buf_size) {
    if (!report || !buf) return -1;

    return snprintf(buf, buf_size,
        "Event: %s (ID: %u)\n"
        "  Seats:     %d booked / %d total\n"
        "  Occupancy: %.1f%%\n"
        "  Revenue:   Rs. %.2f\n",
        report->event_name, report->event_id,
        report->booked_seats, report->total_seats,
        report->occupancy_pct,
        report->total_revenue);
}
