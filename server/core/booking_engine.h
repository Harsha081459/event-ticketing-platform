/*
 * ============================================================================
 * Event Ticketing Platform — Booking Engine (Header)
 * ============================================================================
 * Handles seat booking and cancellation with concurrency control via
 * the lock manager and transaction manager (2PL).
 *
 * Critical section: booking must atomically:
 *   1. Lock all requested seats (exclusive)
 *   2. Verify all are AVAILABLE
 *   3. Create booking + booking_seat records
 *   4. Update seat status to BOOKED
 *   5. Release locks on commit
 * ============================================================================
 */

#ifndef ETP_BOOKING_ENGINE_H
#define ETP_BOOKING_ENGINE_H

#include "../../common/types.h"
#include "../../common/protocol.h"
#include "../../server/storage/table.h"
#include "../../server/txn/txn_manager.h"

/* Booking Engine context */
typedef struct {
    table_t         *bookings_table;
    table_t         *booking_seats_table;
    table_t         *seats_table;
    table_t         *events_table;
    txn_manager_t   *txn_mgr;
} booking_engine_t;

/* ================================================================
 * Lifecycle
 * ================================================================ */
booking_engine_t *booking_engine_create(table_t *bookings, table_t *booking_seats,
                                         table_t *seats, table_t *events,
                                         txn_manager_t *txn_mgr);
void              booking_engine_destroy(booking_engine_t *be);

/* ================================================================
 * Operations
 * ================================================================ */

/*
 * Book seats for a user.
 *   - seat_ids: array of seat_id values to book
 *   - num_seats: number of seats to book
 *   - Uses 2PL: locks all seats exclusively, verifies availability,
 *     creates booking records, updates seat status
 *   - Returns ETP_OK on success, booking_id written to *out_booking_id
 */
etp_result_t booking_book_seats(booking_engine_t *be, uint32_t user_id,
                                 uint32_t event_id,
                                 uint32_t *seat_ids, int num_seats,
                                 uint32_t *out_booking_id);

/*
 * Cancel a booking.
 *   - Marks booking as cancelled
 *   - Releases all booked seats back to AVAILABLE
 */
etp_result_t booking_cancel(booking_engine_t *be, uint32_t booking_id,
                             uint32_t user_id);

/*
 * Get a booking by ID.
 */
etp_result_t booking_get(booking_engine_t *be, uint32_t booking_id,
                          booking_record_t *out);

/*
 * List all bookings for a user. Returns count found.
 */
int booking_list_by_user(booking_engine_t *be, uint32_t user_id,
                          booking_record_t *results, int max_results);

#endif /* ETP_BOOKING_ENGINE_H */
