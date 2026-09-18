/*
 * ============================================================================
 * Event Ticketing Platform — Event Manager (Header)
 * ============================================================================
 * CRUD operations for events: create (with auto seat generation), list,
 * view, and delete.
 * ============================================================================
 */

#ifndef ETP_EVENT_MGR_H
#define ETP_EVENT_MGR_H

#include "../../common/types.h"
#include "../../common/protocol.h"
#include "../../server/storage/table.h"

/* Event Manager context — holds table references */
typedef struct {
    table_t *events_table;
    table_t *seats_table;
} event_manager_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
event_manager_t *event_mgr_create(table_t *events, table_t *seats);
void             event_mgr_destroy(event_manager_t *em);

/* ================================================================
 * Operations
 * ================================================================ */

/*
 * Create a new event and auto-generate seat records.
 * Generates (total_rows × seats_per_row) seat records.
 * Returns ETP_OK on success, event_id written to *out_event_id.
 */
etp_result_t event_mgr_create_event(event_manager_t *em,
                                     const char *name, const char *venue,
                                     const char *date, const char *time_str,
                                     uint32_t organizer_id,
                                     uint16_t total_rows, uint16_t seats_per_row,
                                     float price,
                                     uint32_t *out_event_id);

/*
 * Get event by ID.
 */
etp_result_t event_mgr_get_event(event_manager_t *em, uint32_t event_id,
                                  event_record_t *out);

/*
 * List all active events. Returns count found.
 */
int event_mgr_list_events(event_manager_t *em, event_record_t *results,
                           int max_results);

/*
 * Get all seats for an event. Returns count found.
 */
int event_mgr_get_seats(event_manager_t *em, uint32_t event_id,
                         seat_record_t *results, int max_results);

/*
 * Delete (soft-delete) an event.
 * Only the organizer who created it (or admin) should call this.
 */
etp_result_t event_mgr_delete_event(event_manager_t *em, uint32_t event_id);

#endif /* ETP_EVENT_MGR_H */
