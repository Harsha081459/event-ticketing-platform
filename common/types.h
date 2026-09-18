/*
 * ============================================================================
 * Event Ticketing Platform — Common Type Definitions
 * ============================================================================
 * All shared data types, record structures, enumerations, and result codes
 * used across the entire project. This is the single source of truth for
 * data layout on disk and in memory.
 *
 * Record structs are __attribute__((packed)) and padded to power-of-2 sizes
 * for efficient page-based storage. Every record includes an `is_deleted`
 * flag for soft-delete semantics.
 * ============================================================================
 */

#ifndef ETP_TYPES_H
#define ETP_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Basic Type Aliases
 * ================================================================ */
typedef uint32_t page_id_t;
typedef uint32_t record_id_t;
typedef uint32_t txn_id_t;
typedef uint64_t lsn_t;

#define INVALID_PAGE_ID     ((page_id_t)0xFFFFFFFF)
#define INVALID_RECORD_ID   ((record_id_t)0xFFFFFFFF)
#define INVALID_TXN_ID      ((txn_id_t)0)
#define INVALID_LSN         ((lsn_t)0)

/* ================================================================
 * Record Pointer — locates a record on disk (page + slot)
 * ================================================================ */
typedef struct {
    page_id_t   page_id;
    uint16_t    slot_id;
} record_ptr_t;

static const record_ptr_t INVALID_RECORD_PTR = {0xFFFFFFFF, 0xFFFF};

/* ================================================================
 * Enumerations
 * ================================================================ */

/* User Roles — ordered by privilege level */
typedef enum {
    ROLE_GUEST      = 0,    /* Browse only, no booking */
    ROLE_CUSTOMER   = 1,    /* Browse + book + view own bookings */
    ROLE_ORGANIZER  = 2,    /* Create/manage events + view own event stats */
    ROLE_ADMIN      = 3     /* Full platform control */
} user_role_t;

/* Seat Status */
typedef enum {
    SEAT_AVAILABLE  = 0,
    SEAT_LOCKED     = 1,    /* Temporarily locked during booking transaction */
    SEAT_BOOKED     = 2
} seat_status_t;

/* Event Status */
typedef enum {
    EVENT_ACTIVE    = 0,
    EVENT_CANCELLED = 1
} event_status_t;

/* Booking Status */
typedef enum {
    BOOKING_CONFIRMED = 0,
    BOOKING_CANCELLED = 1
} booking_status_t;

/* Table Identifiers */
typedef enum {
    TABLE_USERS         = 0,
    TABLE_EVENTS        = 1,
    TABLE_SEATS         = 2,
    TABLE_BOOKINGS      = 3,
    TABLE_BOOKING_SEATS = 4,
    TABLE_COUNT         = 5
} table_id_t;

/* WAL Operation Types */
typedef enum {
    WAL_INSERT      = 1,
    WAL_UPDATE      = 2,
    WAL_DELETE      = 3,
    WAL_COMMIT      = 4,
    WAL_ABORT       = 5,
    WAL_CHECKPOINT  = 6
} wal_op_type_t;

/* Lock Types */
typedef enum {
    LOCK_SHARED     = 0,    /* Read lock — multiple holders allowed */
    LOCK_EXCLUSIVE  = 1     /* Write lock — single holder only */
} lock_type_t;

/* ================================================================
 * Result Codes
 * ================================================================ */
typedef enum {
    ETP_OK                  =   0,
    ETP_ERR_GENERIC         =  -1,
    ETP_ERR_NOT_FOUND       =  -2,
    ETP_ERR_DUPLICATE       =  -3,
    ETP_ERR_FULL            =  -4,
    ETP_ERR_IO              =  -5,
    ETP_ERR_LOCK_CONFLICT   =  -6,
    ETP_ERR_AUTH            =  -7,
    ETP_ERR_PERMISSION      =  -8,
    ETP_ERR_INVALID_ARG     =  -9,
    ETP_ERR_TXN_ABORT       = -10,
    ETP_ERR_NO_MEMORY       = -11,
    ETP_ERR_CORRUPTION      = -12,
    ETP_ERR_SEAT_UNAVAIL    = -13,
    ETP_ERR_ALREADY_EXISTS  = -14
} etp_result_t;

/* ================================================================
 * Table Record Structures
 *
 * All records are packed and padded to power-of-2 sizes for
 * aligned page storage. The `is_deleted` field enables soft
 * deletes — records are marked deleted but space is not reclaimed
 * until a compaction pass (future enhancement).
 * ================================================================ */

/*
 * User Record — 128 bytes
 * Fields: 4 + 32 + 65 + 1 + 8 + 1 = 111 bytes + 17 padding
 */
typedef struct __attribute__((packed)) {
    uint32_t    user_id;            /* Primary Key (auto-increment)        */
    char        username[32];       /* Unique username                     */
    char        password_hash[65];  /* Hash digest + null terminator       */
    uint8_t     role;               /* user_role_t                         */
    int64_t     created_at;         /* Unix timestamp                      */
    uint8_t     is_deleted;         /* Soft delete flag                    */
    char        _pad[17];           /* Pad to 128 bytes                    */
} user_record_t;

/*
 * Event Record — 192 bytes
 * Fields: 4 + 64 + 64 + 11 + 6 + 4 + 2 + 2 + 4 + 1 + 1 = 163 bytes + 29 padding
 */
typedef struct __attribute__((packed)) {
    uint32_t    event_id;           /* Primary Key                         */
    char        name[64];           /* Event name                          */
    char        venue[64];          /* Venue name                          */
    char        event_date[11];     /* "YYYY-MM-DD"                        */
    char        event_time[6];      /* "HH:MM"                             */
    uint32_t    organizer_id;       /* FK → users.user_id                  */
    uint16_t    total_rows;         /* Number of seat rows (A-Z)           */
    uint16_t    seats_per_row;      /* Seats per row                       */
    float       price;              /* Price per seat                      */
    uint8_t     status;             /* event_status_t                      */
    uint8_t     is_deleted;         /* Soft delete flag                    */
    char        _pad[29];           /* Pad to 192 bytes                    */
} event_record_t;

/*
 * Seat Record — 32 bytes
 * Fields: 4 + 4 + 1 + 2 + 1 + 4 + 1 = 17 bytes + 15 padding
 */
typedef struct __attribute__((packed)) {
    uint32_t    seat_id;            /* Primary Key                         */
    uint32_t    event_id;           /* FK → events.event_id                */
    char        row_label;          /* 'A', 'B', 'C', ...                  */
    uint16_t    seat_number;        /* 1, 2, 3, ...                        */
    uint8_t     status;             /* seat_status_t                       */
    uint32_t    booked_by;          /* FK → users.user_id (0 = none)       */
    uint8_t     is_deleted;         /* Soft delete flag                    */
    char        _pad[15];           /* Pad to 32 bytes                     */
} seat_record_t;

/*
 * Booking Record — 64 bytes
 * Fields: 4 + 4 + 4 + 2 + 4 + 1 + 8 + 1 = 28 bytes + 36 padding
 */
typedef struct __attribute__((packed)) {
    uint32_t    booking_id;         /* Primary Key                         */
    uint32_t    user_id;            /* FK → users.user_id                  */
    uint32_t    event_id;           /* FK → events.event_id                */
    uint16_t    seat_count;         /* Number of seats in this booking     */
    float       total_amount;       /* price × seat_count                  */
    uint8_t     status;             /* booking_status_t                    */
    int64_t     booked_at;          /* Unix timestamp                      */
    uint8_t     is_deleted;         /* Soft delete flag                    */
    char        _pad[36];           /* Pad to 64 bytes                     */
} booking_record_t;

/*
 * Booking-Seat Junction Record — 16 bytes
 * Fields: 4 + 4 + 1 = 9 bytes + 7 padding
 */
typedef struct __attribute__((packed)) {
    uint32_t    booking_id;         /* FK → bookings.booking_id            */
    uint32_t    seat_id;            /* FK → seats.seat_id                  */
    uint8_t     is_deleted;         /* Soft delete flag                    */
    char        _pad[7];            /* Pad to 16 bytes                     */
} booking_seat_record_t;

/* ================================================================
 * Record Size Constants (compile-time assertions below)
 * ================================================================ */
#define USER_RECORD_SIZE            128
#define EVENT_RECORD_SIZE           192
#define SEAT_RECORD_SIZE             32
#define BOOKING_RECORD_SIZE          64
#define BOOKING_SEAT_RECORD_SIZE     16

/* Compile-time size checks */
_Static_assert(sizeof(user_record_t)         == USER_RECORD_SIZE,
               "user_record_t must be 128 bytes");
_Static_assert(sizeof(event_record_t)        == EVENT_RECORD_SIZE,
               "event_record_t must be 192 bytes");
_Static_assert(sizeof(seat_record_t)         == SEAT_RECORD_SIZE,
               "seat_record_t must be 32 bytes");
_Static_assert(sizeof(booking_record_t)      == BOOKING_RECORD_SIZE,
               "booking_record_t must be 64 bytes");
_Static_assert(sizeof(booking_seat_record_t) == BOOKING_SEAT_RECORD_SIZE,
               "booking_seat_record_t must be 16 bytes");

/* ================================================================
 * Helper: Get record size for a given table
 * ================================================================ */
static inline uint16_t etp_record_size(table_id_t table) {
    static const uint16_t sizes[TABLE_COUNT] = {
        USER_RECORD_SIZE,
        EVENT_RECORD_SIZE,
        SEAT_RECORD_SIZE,
        BOOKING_RECORD_SIZE,
        BOOKING_SEAT_RECORD_SIZE
    };
    return (table < TABLE_COUNT) ? sizes[table] : 0;
}

/* ================================================================
 * Helper: Table name strings (for logging / debug)
 * ================================================================ */
static inline const char *etp_table_name(table_id_t table) {
    static const char *names[TABLE_COUNT] = {
        "users", "events", "seats", "bookings", "booking_seats"
    };
    return (table < TABLE_COUNT) ? names[table] : "unknown";
}

#endif /* ETP_TYPES_H */
