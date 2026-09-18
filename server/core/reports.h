/*
 * ============================================================================
 * Event Ticketing Platform — Reports Engine (Header)
 * ============================================================================
 * Analytics and reporting: revenue per event, occupancy rates, system stats.
 * ============================================================================
 */

#ifndef ETP_REPORTS_H
#define ETP_REPORTS_H

#include "../../common/types.h"
#include "../../server/storage/table.h"

/* Reports context */
typedef struct {
    table_t *events_table;
    table_t *seats_table;
    table_t *bookings_table;
} reports_engine_t;

/* Revenue report for a single event */
typedef struct {
    uint32_t    event_id;
    char        event_name[64];
    int         total_seats;
    int         booked_seats;
    float       occupancy_pct;
    float       total_revenue;
} event_revenue_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
reports_engine_t *reports_create(table_t *events, table_t *seats, table_t *bookings);
void              reports_destroy(reports_engine_t *re);

/* ================================================================
 * Reports
 * ================================================================ */

/*
 * Get revenue report for a specific event.
 */
etp_result_t reports_event_revenue(reports_engine_t *re, uint32_t event_id,
                                    event_revenue_t *out);

/*
 * Get revenue reports for all events by an organizer. Returns count.
 */
int reports_organizer_revenue(reports_engine_t *re, uint32_t organizer_id,
                               event_revenue_t *results, int max_results);

/*
 * Format a revenue report as a human-readable string.
 */
int reports_format_revenue(event_revenue_t *report, char *buf, size_t buf_size);

#endif /* ETP_REPORTS_H */
