# Event Ticketing Platform (Mini BookMyShow)

![CI](https://github.com/Harsha081459/event-ticketing-platform/actions/workflows/ci.yml/badge.svg)

A **multi-client event ticketing system** built from scratch in C, showcasing **Operating Systems** and **DBMS** concepts through a real-world application.

*Project period: built Jun–Jul 2025 as an Operating Systems / DBMS course project; published to GitHub Sep 2026.*

## Architecture

```
┌──────────────┐  TCP/IP  ┌────────────────────────────────────────────────────────────┐
│  CLI Client  │ ◄──────► │                    ETP Server                              │
│  (client.c)  │  Socket  │  ┌──────────┐  ┌──────────┐  ┌──────────────────────────┐  │
│              │          │  │ TCP Srv  │──│ Thread   │──│   Command Router         │  │
│  - select()  │          │  │          │  │ Pool     │  │                          │  │
│  - I/O mux   │          │  └──────────┘  └──────────┘  │  ┌──────┐ ┌──────────┐  │  │
└──────────────┘          │                              │  │ Auth │ │  RBAC    │  │  │
                          │  ┌───────────────────────┐   │  └──────┘ └──────────┘  │  │
                          │  │   Business Logic      │   │                          │  │
                          │  │ ┌────────┐ ┌────────┐ │   │  ┌──────────────────┐   │  │
                          │  │ │Event   │ │Booking │ │   │  │     Reports      │   │  │
                          │  │ │Manager │ │Engine  │ │   │  │                  │   │  │
                          │  │ └────────┘ └────────┘ │   │  └──────────────────┘   │  │
                          │  └───────────────────────┘   └──────────────────────────┘  │
                          │  ┌───────────────────────┐   ┌──────────────────────────┐  │
                          │  │  Transaction Layer    │   │     IPC Layer (OS)       │  │
                          │  │ ┌────────┐ ┌────────┐ │   │ ┌──────┐ ┌────┐ ┌─────┐ │  │
                          │  │ │Lock    │ │Txn     │ │   │ │Pipe  │ │Msg │ │SHM  │ │  │
                          │  │ │Manager │ │Manager │ │   │ │Logger│ │ Q  │ │Stats│ │  │
                          │  │ │ (2PL)  │ │ (WAL)  │ │   │ └──────┘ └────┘ └─────┘ │  │
                          │  │ └────────┘ └────────┘ │   └──────────────────────────┘  │
                          │  └───────────────────────┘                                 │
                          │  ┌─────────────────────────────────────────────────────────┐│
                          │  │                  Storage Engine (DBMS)                  ││
                          │  │  ┌──────────┐  ┌────────────┐  ┌───────┐  ┌──────────┐ ││
                          │  │  │  Table   │──│ Buffer Pool│──│Page   │──│   WAL    │ ││
                          │  │  │  Layer   │  │  (LRU)     │  │ I/O   │  │(Append)  │ ││
                          │  │  └──────────┘  └────────────┘  └───────┘  └──────────┘ ││
                          │  │       │                                                 ││
                          │  │  ┌────────────┐                                        ││
                          │  │  │ B+ Tree    │                                        ││
                          │  │  │  Index     │                                        ││
                          │  │  └────────────┘                                        ││
                          │  └─────────────────────────────────────────────────────────┘│
                          └────────────────────────────────────────────────────────────┘
```

## OS Concepts Demonstrated

| Concept | Implementation | File(s) |
|---|---|---|
| **Socket Programming** | TCP server with `socket()`, `bind()`, `listen()`, `accept()` | `tcp_server.c` |
| **Thread Pool** | Bounded producer-consumer with `pthread_mutex` + `pthread_cond` | `thread_pool.c` |
| **Record-Level Locking** | Hash-bucketed mutexes with shared/exclusive modes | `lock_manager.c` |
| **Two-Phase Locking (2PL)** | Growing phase (acquire) → Shrinking phase (release on commit) | `txn_manager.c` |
| **Semaphores** | Connection limiting via `sem_wait`/`sem_post` | `tcp_server.c` |
| **Pipes** | Async logging through `pipe()` + dedicated writer thread | `logger.c` |
| **Message Queues** | Notification delivery via System V `msgget`/`msgsnd`/`msgrcv` | `notifier.c` |
| **Shared Memory** | Real-time server stats via `shmget`/`shmat` | `stats.c` |
| **Atomic Operations** | Lock-free stat counters with `__atomic` builtins | `stats.c` |
| **Signal Handling** | Graceful shutdown via `SIGINT`/`SIGTERM` handler | `main.c` |
| **I/O Multiplexing** | Client uses `select()` for stdin + socket | `client.c` |
| **File Locking** | Advisory locks via `flock()` for page files | `page.c` |

## DBMS Concepts Demonstrated

| Concept | Implementation | File(s) |
|---|---|---|
| **Page-Based Storage** | Fixed 4KB pages with header + packed records | `page.c` |
| **Buffer Pool Manager** | LRU eviction, pin/unpin, dirty page tracking | `buffer_pool.c` |
| **B+ Tree Index** | In-memory with disk persistence, range scan support | `btree.c` |
| **Write-Ahead Logging** | All mutations logged before data modification | `wal.c` |
| **Lock-managed bookings** | Seat locking plus commit/abort records; full undo/crash recovery is not implemented | `txn_manager.c` |
| **Soft Deletes** | `is_deleted` flag, no immediate space reclamation | `table.c` |
| **Table Abstraction** | Unified CRUD with callback-based scan filters | `table.c` |
| **Role-Based Access Control** | Permission matrix: 4 roles × 17 commands | `rbac.c` |

## Project Structure

```
event-ticketing-platform/
├── common/                 # Shared types, config, utilities
│   ├── types.h             # Record structs, enums, result codes
│   ├── config.h            # All tunable parameters
│   ├── protocol.h          # Command types, session struct
│   ├── utils.h / utils.c   # Logging, hashing, timestamps
│
├── server/
│   ├── main.c              # Server entry point + command router
│   ├── storage/            # DBMS core
│   │   ├── page.h/c        # Page-based file I/O
│   │   ├── buffer_pool.h/c # LRU buffer cache
│   │   ├── btree.h/c       # B+ Tree index
│   │   ├── wal.h/c         # Write-Ahead Log
│   │   └── table.h/c       # Table abstraction layer
│   ├── network/            # OS networking
│   │   ├── tcp_server.h/c  # Multi-client TCP server
│   │   └── thread_pool.h/c # Bounded task queue
│   ├── auth/               # Authentication
│   │   ├── auth.h/c        # Session & credential management
│   │   └── rbac.h/c        # Role-Based Access Control
│   ├── protocol/
│   │   └── parser.h/c      # Command tokenizer
│   ├── core/               # Business logic
│   │   ├── event_mgr.h/c   # Event CRUD + seat generation
│   │   ├── booking_engine.h/c # Transactional seat booking
│   │   └── reports.h/c     # Revenue & occupancy reports
│   ├── txn/                # Transaction control
│   │   ├── txn_manager.h/c # Begin/commit/abort lifecycle
│   │   └── lock_manager.h/c# Record-level 2PL
│   └── ipc/                # OS IPC showcase
│       ├── logger.h/c      # Pipe-based async logger
│       ├── notifier.h/c    # System V message queue
│       └── stats.h/c       # Shared memory stats
│
├── client/
│   └── client.c            # Interactive CLI client
│
├── tests/
│   └── test_storage.c      # Storage engine integration tests
│
├── scripts/
│   ├── demo.sh             # Automated end-to-end demo
│   └── stress_test.sh      # Concurrent booking stress test (2PL proof)
│
├── docs/
│   └── report.md           # Project report (OS + DBMS concepts mapped)
│
└── Makefile                # Build system
```

## Build & Run

### Prerequisites
- GCC with pthreads support
- Linux or WSL (uses POSIX APIs)

### Build
```bash
make            # Build server + client
make test_storage  # Run storage engine tests
make clean      # Remove build artifacts
```

### Run Server
```bash
./bin/etp_server              # Default: port 9090, 16 threads
./bin/etp_server -p 8080      # Custom port
./bin/etp_server -t 8 -v      # 8 threads, verbose logging
```

### Run Client
```bash
./bin/etp_client              # Connect to localhost:9090
./bin/etp_client -p 8080      # Custom port
./bin/etp_client -h 10.0.0.5 -p 8080  # Custom host and port
```

## Usage Example

```
$ ./bin/etp_client

  ╔═══════════════════════════════════════════════════╗
  ║     Welcome to Event Ticketing Platform           ║
  ║     Type HELP for available commands              ║
  ╚═══════════════════════════════════════════════════╝

etp> LOGIN admin admin123
OK Welcome, admin! (Role: ADMIN)

etp> REGISTER organizer1 pass123 organizer
OK Registered user 'organizer1' (ID: 2, Role: ORGANIZER)

etp> LOGIN organizer1 pass123
ERROR Already logged in as 'admin'. LOGOUT first.

etp> LOGOUT
OK Logged out successfully

etp> LOGIN organizer1 pass123
OK Welcome, organizer1! (Role: ORGANIZER)

etp> CREATE_EVENT ConcertNight MainHall 2026-08-15 19:00 5 10 500
OK Event 'ConcertNight' created (ID: 1, 50 seats)

etp> LIST_EVENTS
OK 1 active event(s):
  ID    Name                 Venue           Date        Time   Seats   Rs.
  ───── ──────────────────── ─────────────── ─────────── ─────  ──────  ────
  1     ConcertNight         MainHall        2026-08-15  19:00  5×10    500

etp> VIEW_SEATS 1
OK Seats for event 1 (50 total):
  SeatID Seat  Status
  ────── ───── ────────
  1      A1    AVAIL
  2      A2    AVAIL
  ...

etp> BOOK 1 1 2 3
OK Booking confirmed! (Booking ID: 1, 3 seats)

etp> MY_BOOKINGS
OK Your bookings (1):
  BookID   EventID  Seats  Amount
  ──────── ──────── ────── ──────────
  1        1        3      Rs.1500.00

etp> REVENUE 1
OK
Event: ConcertNight (ID: 1)
  Seats:     3 booked / 50 total
  Occupancy: 6.0%
  Revenue:   Rs. 1500.00

etp> QUIT
OK Goodbye!
```

## Available Commands

| Command | Role Required | Description |
|---|---|---|
| `REGISTER <user> <pass> [role]` | Any for customer; admin for organizer | Create new account |
| `LOGIN <user> <pass>` | Any | Authenticate |
| `LOGOUT` | Logged in | End session |
| `LIST_EVENTS` | Any | Show all active events |
| `VIEW_EVENT <id>` | Any | Event details |
| `VIEW_SEATS <id>` | Any | Seat map with availability |
| `CREATE_EVENT <...>` | Organizer+ | Create event with seats |
| `DELETE_EVENT <id>` | Owner organizer or admin | Remove an event |
| `BOOK <event_id> <seats...>` | Customer+ | Book seats (transactional) |
| `CANCEL <booking_id>` | Customer+ | Cancel and release seats |
| `MY_BOOKINGS` | Customer+ | View your bookings |
| `REVENUE [event_id]` | Organizer+ | Revenue reports |
| `LIST_USERS` | Admin | All registered users |
| `SYSTEM_STATS` | Admin | Server metrics (from SHM) |
| `HELP` | Any | Available commands |
| `QUIT` | Any | Disconnect |

## Technical Highlights

### Concurrent Booking (Deadlock Prevention)
The booking engine sorts seat IDs before acquiring locks, preventing deadlocks when two clients try to book overlapping sets:
```
Client A: BOOK 1 3 5 7    → locks [3, 5, 7] in order
Client B: BOOK 1 7 5 3    → sorted to [3, 5, 7], waits on 3
```

### Buffer Pool Hit Ratio
The LRU buffer pool caches frequently accessed pages. With 64 frames (256KB), it achieves high hit ratios for typical workloads:
```
Total: 64 frames × 4KB = 256KB memory budget
Eviction: LRU (Least Recently Used)
Pin/Unpin: Prevents eviction during active use
```

### Write-Ahead Logging (recovery not yet integrated)
Every mutation is logged to the WAL **before** modifying data pages:
```
1. wal_log_insert(wal, txn, table, key, data)    ← Logged first
2. page_insert_record(page_buf, data)              ← Then applied
3. buffer_pool_mark_dirty(pool, fd, page_id)       ← Marked dirty
```

## Verified local-demo workflow

On Linux or WSL with GCC, Make and Python 3:

```bash
make
make test_storage
python3 tests/test_workflows.py
```

The workflow tests create their own temporary data directory and launch the real TCP server. They cover booking/cancellation/rebooking, eight clients contending for seats, disjoint concurrent bookings, duplicate seat rejection, simultaneous cancellation, fragmented/coalesced commands, oversized-line handling, organizer permissions, and orderly shutdown/restart with persistent records. They do not touch an existing `data/` directory. CI runs both the C storage tests and these socket-level tests.

For the interactive demo start `./bin/etp_server`, then `./bin/etp_client` in another terminal. Log in as `admin` with the documented demo password, create an organizer, create an event, then register a customer and book the seat IDs printed by `VIEW_SEATS`. Only an admin can register organizers; customer registration remains open. Keep the service on a trusted local machine because the default protocol has no transport encryption.

## Limitations

- **Not a complete ACID engine.** `txn_abort()` releases locks but does not undo table mutations; table WAL entries still use transaction ID 0, so they cannot be correlated with booking transaction commits for recovery. Mid-operation I/O failures can leave partial writes. Do not claim crash-safe atomic bookings or use this for real payments.
- **Storage operations use a shared mutex** to protect mutable page/index state; seat-level locks coordinate bookings, but this is not a high-throughput database benchmark. Reads across multiple tables are not snapshot-isolated.

- **WAL recovery is not wired into startup.** Every mutation is logged before
  being applied and `wal_recover()` exists (`server/storage/wal.c:379`), but
  `main.c` never calls it — the WAL demonstrates the log-first discipline, not
  an automatic crash-replay on boot.
- **Password hashing is non-cryptographic.** `etp_hash_password` in
  `common/utils.c` uses two seeded djb2 passes (no salt, no KDF) — adequate for
  a demo, not for real credential storage. A default `admin`/`admin123`
  account is bootstrapped on first run (`server/auth/auth.c:279`).
- **Plaintext TCP.** Commands and responses travel unencrypted — no TLS. This
  is a teaching build for OS primitives, not a deployable service.
- **B+ tree indexes are per-table files** serialized to disk on save/close
  (`server/storage/btree.c`); there is no integrated query planner — the table
  layer chooses index vs full scan.

## Author

Harsha Vardhan Doppalapudi — IIIT Bangalore. Built as a Systems Engineering
project demonstrating OS + DBMS concepts.

## License

MIT — see [LICENSE](LICENSE)
