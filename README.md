# Event Ticketing Platform (Mini BookMyShow)

A **multi-client event ticketing system** built from scratch in C, showcasing **Operating Systems** and **DBMS** concepts through a real-world application.

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
| **ACID Transactions** | Atomicity (WAL), Isolation (2PL), Durability (fsync) | `txn_manager.c` |
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
| `REGISTER <user> <pass> [role]` | Any | Create new account |
| `LOGIN <user> <pass>` | Any | Authenticate |
| `LOGOUT` | Logged in | End session |
| `LIST_EVENTS` | Any | Show all active events |
| `VIEW_EVENT <id>` | Any | Event details |
| `VIEW_SEATS <id>` | Any | Seat map with availability |
| `CREATE_EVENT <...>` | Organizer+ | Create event with seats |
| `DELETE_EVENT <id>` | Organizer+ | Remove an event |
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

### WAL for Crash Recovery
Every mutation is logged to the WAL **before** modifying data pages:
```
1. wal_log_insert(wal, txn, table, key, data)    ← Logged first
2. page_insert_record(page_buf, data)              ← Then applied
3. buffer_pool_mark_dirty(pool, fd, page_id)       ← Marked dirty
```

## Author

Built as a Systems Engineering project demonstrating OS + DBMS concepts.

## License

MIT
