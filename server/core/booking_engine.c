/*
 * ============================================================================
 * Event Ticketing Platform — Booking Engine (Implementation)
 * ============================================================================
 * Transactional booking with Two-Phase Locking:
 *
 *   BEGIN_TXN
 *     1. Lock each seat exclusively (growing phase)
 *     2. Verify all seats are AVAILABLE
 *     3. Create booking record
 *     4. For each seat: create booking_seat + update seat status
 *   COMMIT_TXN (releases all locks — shrinking phase)
 *
 * If any step fails, ABORT_TXN releases locks and rolls back.
 * ============================================================================
 */

#include "booking_engine.h"
#include "../../common/config.h"
#include "../../common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Internal: Filter for booking_seat records by booking_id
 * (Defined at file scope — C doesn't support nested functions in
 *  standard mode, only as a GCC extension)
 * ================================================================ */
typedef struct { uint32_t target_booking_id; } bs_filter_ctx_t;

static int filter_booking_seats(const void *record, void *ctx) {
    const booking_seat_record_t *bs = (const booking_seat_record_t *)record;
    bs_filter_ctx_t *fc = (bs_filter_ctx_t *)ctx;
    return (bs->booking_id == fc->target_booking_id);
}

/* ================================================================
 * Lifecycle
 * ================================================================ */

booking_engine_t *booking_engine_create(table_t *bookings, table_t *booking_seats,
                                         table_t *seats, table_t *events,
                                         txn_manager_t *txn_mgr) {
    if (!bookings || !booking_seats || !seats || !events || !txn_mgr) return NULL;

    booking_engine_t *be = calloc(1, sizeof(booking_engine_t));
    if (!be) return NULL;

    be->bookings_table      = bookings;
    be->booking_seats_table = booking_seats;
    be->seats_table         = seats;
    be->events_table        = events;
    be->txn_mgr             = txn_mgr;

    etp_log(LOG_INFO, "booking_engine: created");
    return be;
}

void booking_engine_destroy(booking_engine_t *be) {
    if (!be) return;
    free(be);
}

/* ================================================================
 * Book Seats — The Critical Transaction
 *
 * This is the heart of the system. Multiple clients may try to
 * book the same seats concurrently. The 2PL protocol ensures:
 *   - No two bookings can claim the same seat
 *   - All-or-nothing: either ALL seats are booked, or NONE
 * ================================================================ */
etp_result_t booking_book_seats(booking_engine_t *be, uint32_t user_id,
                                 uint32_t event_id,
                                 uint32_t *seat_ids, int num_seats,
                                 uint32_t *out_booking_id) {
    if (!be || !seat_ids || num_seats <= 0) return ETP_ERR_INVALID_ARG;
    if (num_seats > MAX_SEATS_PER_BOOKING) {
        etp_log(LOG_WARN, "booking: too many seats (%d > %d)",
                num_seats, MAX_SEATS_PER_BOOKING);
        return ETP_ERR_INVALID_ARG;
    }

    /* Verify event exists and is active */
    event_record_t evt;
    etp_result_t rc = table_find_by_id(be->events_table, event_id, &evt);
    if (rc != ETP_OK) {
        etp_log(LOG_WARN, "booking: event %u not found", event_id);
        return ETP_ERR_NOT_FOUND;
    }
    if (evt.status != EVENT_ACTIVE) {
        return ETP_ERR_GENERIC;
    }

    /* ── BEGIN TRANSACTION ── */
    txn_id_t txn_id = txn_begin(be->txn_mgr);
    if (txn_id == INVALID_TXN_ID) {
        return ETP_ERR_GENERIC;
    }

    /* Step 1: Lock all seats exclusively (2PL growing phase)
     * Sort seat_ids to prevent deadlocks (consistent lock ordering) */
    for (int i = 0; i < num_seats - 1; i++) {
        for (int j = i + 1; j < num_seats; j++) {
            if (seat_ids[i] > seat_ids[j]) {
                uint32_t tmp = seat_ids[i];
                seat_ids[i] = seat_ids[j];
                seat_ids[j] = tmp;
            }
        }
    }

    for (int i = 0; i < num_seats; i++) {
        if (txn_lock(be->txn_mgr, txn_id, TABLE_SEATS,
                     seat_ids[i], LOCK_EXCLUSIVE) != 0) {
            etp_log(LOG_ERROR, "booking: failed to lock seat %u", seat_ids[i]);
            txn_abort(be->txn_mgr, txn_id);
            return ETP_ERR_GENERIC;
        }
    }

    /* Step 2: Verify ALL seats are AVAILABLE */
    seat_record_t seat_buf;
    for (int i = 0; i < num_seats; i++) {
        rc = table_find_by_id(be->seats_table, seat_ids[i], &seat_buf);
        if (rc != ETP_OK) {
            etp_log(LOG_WARN, "booking: seat %u not found", seat_ids[i]);
            txn_abort(be->txn_mgr, txn_id);
            return ETP_ERR_NOT_FOUND;
        }
        if (seat_buf.event_id != event_id) {
            etp_log(LOG_WARN, "booking: seat %u belongs to event %u, not %u",
                    seat_ids[i], seat_buf.event_id, event_id);
            txn_abort(be->txn_mgr, txn_id);
            return ETP_ERR_INVALID_ARG;
        }
        if (seat_buf.status != SEAT_AVAILABLE) {
            etp_log(LOG_WARN, "booking: seat %u not available (status=%d)",
                    seat_ids[i], seat_buf.status);
            txn_abort(be->txn_mgr, txn_id);
            return ETP_ERR_SEAT_UNAVAIL;
        }
    }

    /* Step 3: Create booking record */
    booking_record_t booking;
    memset(&booking, 0, sizeof(booking));
    booking.user_id     = user_id;
    booking.event_id    = event_id;
    booking.seat_count  = (uint16_t)num_seats;
    booking.total_amount = evt.price * num_seats;
    booking.status      = BOOKING_CONFIRMED;
    booking.booked_at   = etp_get_timestamp();
    booking.is_deleted  = 0;

    uint32_t booking_id = 0;
    rc = table_insert(be->bookings_table, &booking, &booking_id);
    if (rc != ETP_OK) {
        etp_log(LOG_ERROR, "booking: failed to create booking record");
        txn_abort(be->txn_mgr, txn_id);
        return rc;
    }

    /* Step 4: For each seat — create booking_seat and update seat status */
    for (int i = 0; i < num_seats; i++) {
        /* Create booking_seat junction record */
        booking_seat_record_t bs;
        memset(&bs, 0, sizeof(bs));
        bs.booking_id = booking_id;
        bs.seat_id    = seat_ids[i];
        bs.is_deleted = 0;

        uint32_t bs_id = 0;
        rc = table_insert(be->booking_seats_table, &bs, &bs_id);
        if (rc != ETP_OK) {
            etp_log(LOG_ERROR, "booking: failed to create booking_seat for seat %u",
                    seat_ids[i]);
            txn_abort(be->txn_mgr, txn_id);
            return rc;
        }

        /* Update seat status to BOOKED */
        rc = table_find_by_id(be->seats_table, seat_ids[i], &seat_buf);
        if (rc == ETP_OK) {
            seat_buf.status    = SEAT_BOOKED;
            seat_buf.booked_by = user_id;
            table_update(be->seats_table, seat_ids[i], &seat_buf);
        }
    }

    /* ── COMMIT TRANSACTION ── (releases all locks) */
    if (txn_commit(be->txn_mgr, txn_id) != 0) {
        etp_log(LOG_ERROR, "booking: transaction commit failed");
        return ETP_ERR_IO;
    }

    if (out_booking_id) *out_booking_id = booking_id;

    etp_log(LOG_INFO, "booking: user %u booked %d seats for event %u (booking=%u, total=%.2f)",
            user_id, num_seats, event_id, booking_id, booking.total_amount);
    return ETP_OK;
}

/* ================================================================
 * Cancel Booking
 *
 * Steps:
 *   1. Find booking, verify ownership
 *   2. Release each booked seat back to AVAILABLE
 *   3. Mark booking as CANCELLED
 * ================================================================ */
etp_result_t booking_cancel(booking_engine_t *be, uint32_t booking_id,
                             uint32_t user_id) {
    if (!be) return ETP_ERR_INVALID_ARG;

    /* ── BEGIN TRANSACTION ──
     * We start the transaction BEFORE reading the booking to prevent
     * a TOCTOU race: another thread could cancel the same booking
     * between our read and our update if we read outside the txn. */
    txn_id_t txn_id = txn_begin(be->txn_mgr);
    if (txn_id == INVALID_TXN_ID) return ETP_ERR_GENERIC;

    /* Find the booking */
    booking_record_t booking;
    etp_result_t rc = table_find_by_id(be->bookings_table, booking_id, &booking);
    if (rc != ETP_OK) {
        txn_abort(be->txn_mgr, txn_id);
        return ETP_ERR_NOT_FOUND;
    }

    /* Verify ownership */
    if (booking.user_id != user_id) {
        etp_log(LOG_WARN, "booking: user %u tried to cancel booking %u (owned by %u)",
                user_id, booking_id, booking.user_id);
        txn_abort(be->txn_mgr, txn_id);
        return ETP_ERR_AUTH;
    }

    if (booking.status != BOOKING_CONFIRMED) {
        txn_abort(be->txn_mgr, txn_id);
        return ETP_ERR_GENERIC;
    }

    /* Find all booking_seats for this booking */
    bs_filter_ctx_t bctx = { .target_booking_id = booking_id };
    booking_seat_record_t bs_results[MAX_SEATS_PER_BOOKING];
    int count = table_scan(be->booking_seats_table, filter_booking_seats, &bctx,
                           bs_results, MAX_SEATS_PER_BOOKING);

    /* Sort seats by seat_id before locking — prevents deadlocks.
     * Same strategy as booking_book_seats (consistent lock ordering). */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (bs_results[i].seat_id > bs_results[j].seat_id) {
                booking_seat_record_t tmp = bs_results[i];
                bs_results[i] = bs_results[j];
                bs_results[j] = tmp;
            }
        }
    }

    /* Lock and release each seat */
    for (int i = 0; i < count; i++) {
        if (txn_lock(be->txn_mgr, txn_id, TABLE_SEATS,
                     bs_results[i].seat_id, LOCK_EXCLUSIVE) != 0) {
            etp_log(LOG_ERROR, "booking: cancel failed to lock seat %u",
                    bs_results[i].seat_id);
            txn_abort(be->txn_mgr, txn_id);
            return ETP_ERR_GENERIC;
        }

        seat_record_t seat;
        rc = table_find_by_id(be->seats_table, bs_results[i].seat_id, &seat);
        if (rc == ETP_OK) {
            seat.status    = SEAT_AVAILABLE;
            seat.booked_by = 0;
            if (table_update(be->seats_table, bs_results[i].seat_id, &seat) != ETP_OK) {
                etp_log(LOG_ERROR, "booking: cancel failed to update seat %u",
                        bs_results[i].seat_id);
                txn_abort(be->txn_mgr, txn_id);
                return ETP_ERR_IO;
            }
        }
    }

    /* Mark booking as cancelled */
    booking.status = BOOKING_CANCELLED;
    if (table_update(be->bookings_table, booking_id, &booking) != ETP_OK) {
        etp_log(LOG_ERROR, "booking: cancel failed to update booking %u", booking_id);
        txn_abort(be->txn_mgr, txn_id);
        return ETP_ERR_IO;
    }

    /* ── COMMIT TRANSACTION ── */
    if (txn_commit(be->txn_mgr, txn_id) != 0) {
        etp_log(LOG_ERROR, "booking: cancel commit failed for booking %u", booking_id);
        return ETP_ERR_IO;
    }

    etp_log(LOG_INFO, "booking: cancelled booking %u (%d seats released)",
            booking_id, count);
    return ETP_OK;
}

/* ================================================================
 * Get Booking
 * ================================================================ */
etp_result_t booking_get(booking_engine_t *be, uint32_t booking_id,
                          booking_record_t *out) {
    if (!be || !out) return ETP_ERR_INVALID_ARG;
    return table_find_by_id(be->bookings_table, booking_id, out);
}

/* ================================================================
 * List Bookings by User
 * ================================================================ */
typedef struct { uint32_t target_user_id; } user_filter_ctx_t;

static int filter_by_user(const void *record, void *ctx) {
    const booking_record_t *b = (const booking_record_t *)record;
    user_filter_ctx_t *fc = (user_filter_ctx_t *)ctx;
    return (b->user_id == fc->target_user_id &&
            b->status == BOOKING_CONFIRMED);
}

int booking_list_by_user(booking_engine_t *be, uint32_t user_id,
                          booking_record_t *results, int max_results) {
    if (!be || !results) return 0;
    user_filter_ctx_t ctx = { .target_user_id = user_id };
    return table_scan(be->bookings_table, filter_by_user, &ctx,
                      results, max_results);
}
