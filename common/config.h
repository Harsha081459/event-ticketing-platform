/*
 * ============================================================================
 * Event Ticketing Platform — Configuration Constants
 * ============================================================================
 * All tunable parameters for the system in one place. Adjust these values
 * to trade off between memory usage, performance, and capacity.
 * ============================================================================
 */

#ifndef ETP_CONFIG_H
#define ETP_CONFIG_H

#include <stddef.h>  /* NULL */

/* ================================================================
 * Storage Engine
 * ================================================================ */
#define PAGE_SIZE               4096    /* Disk page size in bytes           */
#define PAGE_HEADER_SIZE        16      /* Reserved bytes at start of page   */
#define BUFFER_POOL_FRAMES      64      /* Number of pages cached in memory  */
#define WAL_SYNC_INTERVAL       10      /* fsync WAL every N writes          */

/* ================================================================
 * B+ Tree Index
 * ================================================================ */
#define BTREE_DEFAULT_ORDER     128     /* Max children per internal node    */

/* ================================================================
 * Network / Server
 * ================================================================ */
#define SERVER_PORT             9090    /* TCP listen port                   */
#define SERVER_BACKLOG          32      /* listen() backlog queue size       */
#define MAX_CONNECTIONS         100     /* Maximum concurrent clients        */
#define THREAD_POOL_SIZE        16      /* Worker threads in the pool        */
#define MAX_CMD_LENGTH          1024    /* Maximum client command length     */
#define MAX_RESPONSE_SIZE       65536   /* Maximum server response (64 KB)   */

/* ================================================================
 * Authentication & Sessions
 * ================================================================ */
#define MAX_USERNAME_LEN        31      /* Max chars in username             */
#define MAX_PASSWORD_LEN        64      /* Max chars in password             */
#define MAX_SESSIONS            MAX_CONNECTIONS
#define SESSION_TIMEOUT_SECS    3600    /* Auto-logout after 1 hour idle     */

/* ================================================================
 * Event & Booking Limits
 * ================================================================ */
#define MAX_EVENT_NAME_LEN      63      /* Max chars in event name           */
#define MAX_VENUE_LEN           63      /* Max chars in venue name           */
#define MAX_SEAT_ROWS           26      /* Rows A through Z                  */
#define MAX_SEATS_PER_ROW       100     /* Max seats in a single row         */
#define MAX_SEATS_PER_BOOKING   10      /* Max seats in one booking          */

/* ================================================================
 * File Paths (relative to working directory)
 * ================================================================ */
#define DATA_DIR                        "data"

/* Table data files */
#define USERS_DATA_FILE                 DATA_DIR "/users.dat"
#define EVENTS_DATA_FILE                DATA_DIR "/events.dat"
#define SEATS_DATA_FILE                 DATA_DIR "/seats.dat"
#define BOOKINGS_DATA_FILE              DATA_DIR "/bookings.dat"
#define BOOKING_SEATS_DATA_FILE         DATA_DIR "/booking_seats.dat"

/* Primary key index files */
#define USERS_PK_INDEX_FILE             DATA_DIR "/users_pk.idx"
#define EVENTS_PK_INDEX_FILE            DATA_DIR "/events_pk.idx"
#define SEATS_PK_INDEX_FILE             DATA_DIR "/seats_pk.idx"
#define BOOKINGS_PK_INDEX_FILE          DATA_DIR "/bookings_pk.idx"
#define BOOKING_SEATS_PK_INDEX_FILE     DATA_DIR "/booking_seats_pk.idx"

/* Secondary index files */
#define SEATS_EVENT_INDEX_FILE          DATA_DIR "/seats_event.idx"

/* WAL and log files */
#define WAL_FILE                        DATA_DIR "/wal.log"
#define SERVER_LOG_FILE                 DATA_DIR "/server.log"

/* Metadata */
#define METADATA_FILE                   DATA_DIR "/metadata.dat"

/* ================================================================
 * IPC Configuration (System V IPC keys)
 * ================================================================ */
#define ETP_SHM_KEY             0x45545001  /* Shared memory key         */
#define ETP_MSG_KEY             0x45545002  /* Message queue key          */

/* ================================================================
 * Helper: Get data file path for a table
 * ================================================================ */
static inline const char *etp_data_file(int table) {
    static const char *files[] = {
        USERS_DATA_FILE,
        EVENTS_DATA_FILE,
        SEATS_DATA_FILE,
        BOOKINGS_DATA_FILE,
        BOOKING_SEATS_DATA_FILE
    };
    return (table >= 0 && table < 5) ? files[table] : NULL;
}

/* ================================================================
 * Helper: Get primary key index file path for a table
 * ================================================================ */
static inline const char *etp_index_file(int table) {
    static const char *files[] = {
        USERS_PK_INDEX_FILE,
        EVENTS_PK_INDEX_FILE,
        SEATS_PK_INDEX_FILE,
        BOOKINGS_PK_INDEX_FILE,
        BOOKING_SEATS_PK_INDEX_FILE
    };
    return (table >= 0 && table < 5) ? files[table] : NULL;
}

#endif /* ETP_CONFIG_H */
