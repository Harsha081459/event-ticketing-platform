/*
 * ============================================================================
 * Event Ticketing Platform — Storage Engine Integration Test
 * ============================================================================
 * Tests all Phase 1 components: Page I/O, Buffer Pool, B+ Tree, WAL, and
 * the Table abstraction layer. Each test prints PASS/FAIL.
 *
 * Run: make test_storage
 * ============================================================================
 */

#include "common/types.h"
#include "common/config.h"
#include "common/utils.h"
#include "server/storage/page.h"
#include "server/storage/buffer_pool.h"
#include "server/storage/btree.h"
#include "server/storage/wal.h"
#include "server/storage/table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

/* ── Test Counters ────────────────────────────────────────────── */
static int tests_run    = 0;
static int tests_passed = 0;

#define TEST_START(name) \
    do { tests_run++; printf("  %-50s ", name); fflush(stdout); } while(0)

#define TEST_PASS() \
    do { tests_passed++; printf("\033[32mPASS\033[0m\n"); } while(0)

#define TEST_FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m  (%s)\n", msg); } while(0)

#define ASSERT_TEST(cond, msg) \
    do { if (!(cond)) { TEST_FAIL(msg); return; } } while(0)

/* ── Test Data Directory ──────────────────────────────────────── */
#define TEST_DATA_DIR "data/test"

static void ensure_test_dir(void) {
    etp_ensure_dir("data");
    etp_ensure_dir(TEST_DATA_DIR);
}

/* ================================================================
 * Test 1: Page I/O
 * ================================================================ */
static void test_page_basic(void) {
    TEST_START("Page: create, write, read back");

    const char *file = TEST_DATA_DIR "/test_page.dat";
    int fd = page_file_create(file);
    ASSERT_TEST(fd >= 0, "page_file_create failed");

    /* Allocate a page for user records */
    page_id_t pid = page_alloc(fd, USER_RECORD_SIZE);
    ASSERT_TEST(pid != INVALID_PAGE_ID, "page_alloc failed");

    /* Read the page into a buffer */
    char buf[PAGE_SIZE];
    ASSERT_TEST(page_read(fd, pid, buf) == 0, "page_read failed");

    /* Insert a user record */
    user_record_t user;
    memset(&user, 0, sizeof(user));
    user.user_id = 1;
    etp_strlcpy(user.username, "alice", sizeof(user.username));
    etp_strlcpy(user.password_hash, "abc123hash", sizeof(user.password_hash));
    user.role = ROLE_CUSTOMER;
    user.created_at = etp_get_timestamp();
    user.is_deleted = 0;

    int slot = page_insert_record(buf, &user);
    ASSERT_TEST(slot >= 0, "page_insert_record failed");

    /* Read it back */
    void *rec = page_get_record(buf, (uint16_t)slot);
    ASSERT_TEST(rec != NULL, "page_get_record returned NULL");

    user_record_t *read_back = (user_record_t *)rec;
    ASSERT_TEST(read_back->user_id == 1, "user_id mismatch");
    ASSERT_TEST(strcmp(read_back->username, "alice") == 0, "username mismatch");

    /* Write page back to disk and re-read */
    ASSERT_TEST(page_write(fd, pid, buf) == 0, "page_write failed");

    char buf2[PAGE_SIZE];
    ASSERT_TEST(page_read(fd, pid, buf2) == 0, "page_read after write failed");

    user_record_t *read_back2 = (user_record_t *)page_get_record(buf2, (uint16_t)slot);
    ASSERT_TEST(read_back2->user_id == 1, "re-read user_id mismatch");

    page_file_close(fd);
    TEST_PASS();
}

static void test_page_multiple_records(void) {
    TEST_START("Page: insert multiple records, verify count");

    const char *file = TEST_DATA_DIR "/test_page_multi.dat";
    int fd = page_file_create(file);
    ASSERT_TEST(fd >= 0, "create failed");

    page_id_t pid = page_alloc(fd, SEAT_RECORD_SIZE);
    char buf[PAGE_SIZE];
    page_read(fd, pid, buf);

    /* Insert several seat records */
    uint16_t max = page_get_max_records(SEAT_RECORD_SIZE);
    int inserted = 0;
    for (uint16_t i = 0; i < max && i < 20; i++) {
        seat_record_t seat;
        memset(&seat, 0, sizeof(seat));
        seat.seat_id = i + 1;
        seat.event_id = 100;
        seat.row_label = 'A' + (i / 5);
        seat.seat_number = (i % 5) + 1;
        seat.status = SEAT_AVAILABLE;
        seat.booked_by = 0;
        seat.is_deleted = 0;

        int slot = page_insert_record(buf, &seat);
        if (slot >= 0) inserted++;
    }

    ASSERT_TEST(inserted == 20, "expected 20 inserts");
    ASSERT_TEST(page_get_record_count(buf) == 20, "record count mismatch");

    page_file_close(fd);
    TEST_PASS();
}

/* ================================================================
 * Test 2: B+ Tree
 * ================================================================ */
static void test_btree_basic(void) {
    TEST_START("B+Tree: insert, search, delete");

    const char *file = TEST_DATA_DIR "/test_btree.idx";
    btree_t *tree = btree_create(file, 4);  /* Small order for testing splits */
    ASSERT_TEST(tree != NULL, "btree_create failed");

    /* Insert keys 1..20 */
    for (uint32_t i = 1; i <= 20; i++) {
        record_ptr_t ptr = { .page_id = i * 10, .slot_id = (uint16_t)(i % 5) };
        int rc = btree_insert(tree, i, ptr);
        ASSERT_TEST(rc == 0, "insert failed");
    }
    ASSERT_TEST(btree_size(tree) == 20, "size should be 20");

    /* Search for key 15 */
    record_ptr_t result;
    int rc = btree_search(tree, 15, &result);
    ASSERT_TEST(rc == 0, "search for 15 failed");
    ASSERT_TEST(result.page_id == 150, "page_id mismatch for key 15");

    /* Delete key 10 */
    rc = btree_delete(tree, 10);
    ASSERT_TEST(rc == 0, "delete 10 failed");
    ASSERT_TEST(btree_size(tree) == 19, "size should be 19 after delete");

    /* Search for deleted key should fail */
    rc = btree_search(tree, 10, &result);
    ASSERT_TEST(rc != 0, "search for deleted key should fail");

    /* Duplicate insert should fail */
    record_ptr_t dup = { .page_id = 999, .slot_id = 0 };
    rc = btree_insert(tree, 5, dup);
    ASSERT_TEST(rc != 0, "duplicate insert should fail");

    btree_close(tree);
    TEST_PASS();
}

static void test_btree_persistence(void) {
    TEST_START("B+Tree: save and reload from disk");

    const char *file = TEST_DATA_DIR "/test_btree_persist.idx";

    /* Create and populate */
    btree_t *tree = btree_create(file, 4);
    for (uint32_t i = 1; i <= 50; i++) {
        record_ptr_t ptr = { .page_id = i, .slot_id = 0 };
        btree_insert(tree, i, ptr);
    }
    btree_close(tree);  /* Saves to disk */

    /* Reopen and verify */
    tree = btree_open(file);
    ASSERT_TEST(tree != NULL, "btree_open failed");
    ASSERT_TEST(btree_size(tree) == 50, "size should be 50 after reload");

    record_ptr_t result;
    int rc = btree_search(tree, 25, &result);
    ASSERT_TEST(rc == 0, "search after reload failed");
    ASSERT_TEST(result.page_id == 25, "value mismatch after reload");

    btree_close(tree);
    TEST_PASS();
}

static void test_btree_range_scan(void) {
    TEST_START("B+Tree: range scan [10, 20]");

    const char *file = TEST_DATA_DIR "/test_btree_range.idx";
    btree_t *tree = btree_create(file, 4);

    for (uint32_t i = 1; i <= 30; i++) {
        record_ptr_t ptr = { .page_id = i * 100, .slot_id = 0 };
        btree_insert(tree, i, ptr);
    }

    record_ptr_t results[20];
    int count = btree_range_scan(tree, 10, 20, results, 20);
    ASSERT_TEST(count == 11, "range [10,20] should return 11 keys");

    btree_close(tree);
    TEST_PASS();
}

/* ================================================================
 * Test 3: Buffer Pool
 * ================================================================ */
static void test_buffer_pool(void) {
    TEST_START("BufferPool: fetch, pin/unpin, dirty flush");

    buffer_pool_t *pool = buffer_pool_create(8);  /* Small pool for testing */
    ASSERT_TEST(pool != NULL, "buffer_pool_create failed");

    /* Create a page file */
    const char *file = TEST_DATA_DIR "/test_bp.dat";
    int fd = page_file_create(file);
    ASSERT_TEST(fd >= 0, "page_file_create failed");

    page_id_t pid = page_alloc(fd, USER_RECORD_SIZE);

    /* Fetch page through buffer pool */
    void *page = buffer_pool_fetch(pool, fd, pid);
    ASSERT_TEST(page != NULL, "buffer_pool_fetch failed");

    /* Insert a record through the buffer */
    user_record_t user;
    memset(&user, 0, sizeof(user));
    user.user_id = 42;
    etp_strlcpy(user.username, "bob", sizeof(user.username));

    int slot = page_insert_record(page, &user);
    ASSERT_TEST(slot >= 0, "insert into buffered page failed");

    buffer_pool_mark_dirty(pool, fd, pid);
    buffer_pool_unpin(pool, fd, pid);

    /* Flush and verify */
    buffer_pool_flush_all(pool);

    /* Re-read directly from disk to verify */
    char raw[PAGE_SIZE];
    page_read(fd, pid, raw);
    user_record_t *check = (user_record_t *)page_get_record(raw, (uint16_t)slot);
    ASSERT_TEST(check != NULL, "direct read failed");
    ASSERT_TEST(check->user_id == 42, "flushed data mismatch");

    page_file_close(fd);
    buffer_pool_destroy(pool);
    TEST_PASS();
}

/* ================================================================
 * Test 4: WAL
 * ================================================================ */
static void test_wal_basic(void) {
    TEST_START("WAL: log insert/commit, recover");

    const char *file = TEST_DATA_DIR "/test_wal.log";
    wal_t *wal = wal_create(file);
    ASSERT_TEST(wal != NULL, "wal_create failed");

    /* Log an insert */
    user_record_t user;
    memset(&user, 0, sizeof(user));
    user.user_id = 1;
    etp_strlcpy(user.username, "charlie", sizeof(user.username));

    lsn_t lsn1 = wal_log_insert(wal, 1, TABLE_USERS, 1, &user, sizeof(user));
    ASSERT_TEST(lsn1 != INVALID_LSN, "wal_log_insert failed");

    lsn_t lsn2 = wal_log_commit(wal, 1);
    ASSERT_TEST(lsn2 > lsn1, "commit LSN should be > insert LSN");

    wal_flush(wal);
    wal_close(wal);

    /* Recover — count records */
    static int recover_count;
    recover_count = 0;

    void count_callback(const wal_record_header_t *hdr,
                        const void *old_data, const void *new_data,
                        void *ctx) {
        (void)old_data; (void)new_data; (void)ctx;
        (void)hdr;
        recover_count++;
    }

    wal = wal_open(file);
    ASSERT_TEST(wal != NULL, "wal_open failed");

    wal_recover(wal, count_callback, NULL);
    ASSERT_TEST(recover_count == 2, "expected 2 WAL records (insert + commit)");

    wal_close(wal);
    TEST_PASS();
}

/* ================================================================
 * Test 5: Table — Full Integration
 * ================================================================ */
static void test_table_insert_find(void) {
    TEST_START("Table: insert user, find by ID");

    buffer_pool_t *pool = buffer_pool_create(BUFFER_POOL_FRAMES);
    wal_t *wal = wal_create(TEST_DATA_DIR "/test_table_wal.log");
    ASSERT_TEST(pool && wal, "pool/wal creation failed");

    table_t *users = table_create(TABLE_USERS,
                                   TEST_DATA_DIR "/test_users.dat",
                                   TEST_DATA_DIR "/test_users_pk.idx",
                                   pool, wal);
    ASSERT_TEST(users != NULL, "table_create failed");

    /* Insert a user */
    user_record_t user;
    memset(&user, 0, sizeof(user));
    etp_strlcpy(user.username, "diana", sizeof(user.username));
    etp_hash_password("secret", user.password_hash, sizeof(user.password_hash));
    user.role = ROLE_ORGANIZER;
    user.created_at = etp_get_timestamp();

    uint32_t out_id = 0;
    etp_result_t rc = table_insert(users, &user, &out_id);
    ASSERT_TEST(rc == ETP_OK, "table_insert failed");
    ASSERT_TEST(out_id > 0, "out_id should be > 0");

    /* Find by ID */
    user_record_t found;
    rc = table_find_by_id(users, out_id, &found);
    ASSERT_TEST(rc == ETP_OK, "table_find_by_id failed");
    ASSERT_TEST(found.user_id == out_id, "user_id mismatch");
    ASSERT_TEST(strcmp(found.username, "diana") == 0, "username mismatch");
    ASSERT_TEST(found.role == ROLE_ORGANIZER, "role mismatch");

    table_close(users);
    wal_close(wal);
    buffer_pool_destroy(pool);
    TEST_PASS();
}

static void test_table_update_delete(void) {
    TEST_START("Table: update and soft-delete");

    buffer_pool_t *pool = buffer_pool_create(BUFFER_POOL_FRAMES);
    wal_t *wal = wal_create(TEST_DATA_DIR "/test_table2_wal.log");
    table_t *events = table_create(TABLE_EVENTS,
                                    TEST_DATA_DIR "/test_events.dat",
                                    TEST_DATA_DIR "/test_events_pk.idx",
                                    pool, wal);
    ASSERT_TEST(events != NULL, "table_create failed");

    /* Insert an event */
    event_record_t evt;
    memset(&evt, 0, sizeof(evt));
    etp_strlcpy(evt.name, "Coldplay Mumbai", sizeof(evt.name));
    etp_strlcpy(evt.venue, "DY Patil Stadium", sizeof(evt.venue));
    etp_strlcpy(evt.event_date, "2026-08-15", sizeof(evt.event_date));
    etp_strlcpy(evt.event_time, "19:00", sizeof(evt.event_time));
    evt.organizer_id = 1;
    evt.total_rows = 5;
    evt.seats_per_row = 10;
    evt.price = 2500.0f;
    evt.status = EVENT_ACTIVE;

    uint32_t evt_id = 0;
    etp_result_t rc = table_insert(events, &evt, &evt_id);
    ASSERT_TEST(rc == ETP_OK, "insert event failed");

    /* Update: change price */
    event_record_t updated;
    rc = table_find_by_id(events, evt_id, &updated);
    ASSERT_TEST(rc == ETP_OK, "find for update failed");

    updated.price = 3000.0f;
    rc = table_update(events, evt_id, &updated);
    ASSERT_TEST(rc == ETP_OK, "update failed");

    /* Verify update */
    event_record_t check;
    table_find_by_id(events, evt_id, &check);
    ASSERT_TEST(check.price == 3000.0f, "price not updated");

    /* Delete */
    rc = table_delete(events, evt_id);
    ASSERT_TEST(rc == ETP_OK, "delete failed");

    /* Find should fail after delete */
    rc = table_find_by_id(events, evt_id, &check);
    ASSERT_TEST(rc == ETP_ERR_NOT_FOUND, "find after delete should fail");

    table_close(events);
    wal_close(wal);
    buffer_pool_destroy(pool);
    TEST_PASS();
}

static void test_table_scan(void) {
    TEST_START("Table: scan with filter");

    buffer_pool_t *pool = buffer_pool_create(BUFFER_POOL_FRAMES);
    wal_t *wal = wal_create(TEST_DATA_DIR "/test_table3_wal.log");
    table_t *seats = table_create(TABLE_SEATS,
                                   TEST_DATA_DIR "/test_seats.dat",
                                   TEST_DATA_DIR "/test_seats_pk.idx",
                                   pool, wal);
    ASSERT_TEST(seats != NULL, "table_create failed");

    /* Insert 10 seats — 5 available, 5 booked */
    for (int i = 0; i < 10; i++) {
        seat_record_t seat;
        memset(&seat, 0, sizeof(seat));
        seat.event_id = 1;
        seat.row_label = 'A';
        seat.seat_number = i + 1;
        seat.status = (i < 5) ? SEAT_AVAILABLE : SEAT_BOOKED;
        seat.booked_by = (i < 5) ? 0 : 42;

        uint32_t id;
        table_insert(seats, &seat, &id);
    }

    /* Scan for available seats only */
    int filter_available(const void *record, void *ctx) {
        (void)ctx;
        const seat_record_t *s = (const seat_record_t *)record;
        return s->status == SEAT_AVAILABLE;
    }

    seat_record_t results[20];
    int count = table_scan(seats, filter_available, NULL, results, 20);
    ASSERT_TEST(count == 5, "expected 5 available seats");

    /* Verify they are all AVAILABLE */
    for (int i = 0; i < count; i++) {
        ASSERT_TEST(results[i].status == SEAT_AVAILABLE, "non-available seat in results");
    }

    table_close(seats);
    wal_close(wal);
    buffer_pool_destroy(pool);
    TEST_PASS();
}

/* ================================================================
 * Main — Run All Tests
 * ================================================================ */
int main(void) {
    etp_set_log_level(LOG_WARN);  /* Suppress debug/info during tests */
    ensure_test_dir();

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       Event Ticketing Platform — Storage Tests          ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Phase 1: Page I/O, Buffer Pool, B+Tree, WAL, Table    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\n");

    /* Page tests */
    printf("── Page I/O ────────────────────────────────────────────\n");
    test_page_basic();
    test_page_multiple_records();

    /* B+ Tree tests */
    printf("\n── B+ Tree Index ───────────────────────────────────────\n");
    test_btree_basic();
    test_btree_persistence();
    test_btree_range_scan();

    /* Buffer Pool tests */
    printf("\n── Buffer Pool ─────────────────────────────────────────\n");
    test_buffer_pool();

    /* WAL tests */
    printf("\n── Write-Ahead Log ─────────────────────────────────────\n");
    test_wal_basic();

    /* Table integration tests */
    printf("\n── Table Layer (Full Integration) ──────────────────────\n");
    test_table_insert_find();
    test_table_update_delete();
    test_table_scan();

    /* Summary */
    printf("\n════════════════════════════════════════════════════════\n");
    if (tests_passed == tests_run) {
        printf("  \033[32m✓ All %d tests passed!\033[0m\n", tests_run);
    } else {
        printf("  \033[31m✗ %d/%d tests passed (%d failed)\033[0m\n",
               tests_passed, tests_run, tests_run - tests_passed);
    }
    printf("════════════════════════════════════════════════════════\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
