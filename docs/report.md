# Event Ticketing Platform — Project Report

## 1. Introduction

The **Event Ticketing Platform** is a multi-threaded client-server application built in C that simulates a real-world ticketing system (like Ticketmaster or BookMyShow). It was designed from the ground up to demonstrate deep knowledge of **Operating Systems** and **Database Management Systems (DBMS)** concepts.

## 2. Core Architecture

The system is built using a layered architecture:

1. **Storage Layer (Custom DBMS)**: 
   - A custom database engine implementing fixed-size 4KB pages.
   - An LRU Buffer Pool for caching pages in memory.
   - B+ Tree indexing for $O(\log N)$ point queries.
   - A Write-Ahead Log (WAL) to ensure data durability and recovery (ACID compliance).
2. **Transaction Layer**: 
   - A Lock Manager implementing Two-Phase Locking (2PL) to serialize concurrent transactions and prevent double-booking.
3. **Network Layer**: 
   - A multithreaded TCP Server using POSIX sockets (`bind`, `listen`, `accept`).
   - A custom Thread Pool to multiplex incoming client connections efficiently.
4. **IPC Layer (Inter-Process Communication)**:
   - Uses pipes for asynchronous logging.
   - Uses System V Message Queues for notifications.
   - Uses Shared Memory for real-time system statistics.
5. **Business Logic Layer**:
   - Manages Users (Authentication, RBAC), Events, and Bookings.
   - Role-Based Access Control limits what `GUEST`, `CUSTOMER`, `ORGANIZER`, and `ADMIN` users can do.

## 3. Operating System Concepts Applied

- **POSIX Sockets**: Used for the client-server communication over TCP/IP.
- **Multithreading & Synchronization**: The thread pool uses `pthread_create`, `pthread_mutex`, and `pthread_cond` to manage worker threads.
- **Inter-Process Communication (IPC)**:
  - **Pipes (`pipe`)**: A dedicated background thread writes logs asynchronously to prevent blocking worker threads.
  - **Shared Memory (`shmget`, `shmat`)**: Global statistics (active connections, total requests) are tracked in a shared memory segment.
  - **Message Queues (`msgget`, `msgsnd`, `msgrcv`)**: Used to decouple the booking engine from the notification system.
- **I/O Multiplexing (`select`)**: The client uses `select` to listen to both `stdin` and the server socket simultaneously, preventing blocking on either end.
- **Signal Handling (`signal`)**: The server gracefully shuts down on `SIGINT` (flushing dirty pages and closing the WAL). The client ignores `SIGPIPE` to prevent crashes when the server disconnects.

## 4. Database Management Systems Concepts Applied

- **Custom Storage Engine**: Data is not simply dumped into a text file. It is serialized into fixed 4KB binary pages, mimicking real engines like InnoDB or Postgres.
- **B+ Tree Indexing**: Secondary lookup structures ensure that finding a record by its Primary Key is extremely fast, minimizing disk I/O.
- **Buffer Pool (LRU Cache)**: Memory management for database pages. Pages are pinned during use and unpinned after, with a background eviction policy based on Least Recently Used.
- **Write-Ahead Logging (WAL)**: All modifications (inserts/updates) are appended to a WAL file *before* being written to the data file, guaranteeing atomicity and durability (the "A" and "D" in ACID).
- **Two-Phase Locking (2PL)**: 
  - *Growing phase*: The booking engine requests locks for all selected seats.
  - *Critical section*: If all locks are acquired, the transaction creates the booking.
  - *Shrinking phase*: All locks are released together upon commit or abort.
  - This ensures Isolation and prevents the classic "double-booking" anomaly under heavy concurrency.

## 5. Security & Authentication

- **Role-Based Access Control (RBAC)**: Enforces permissions based on user roles. Customers cannot create events; organizers cannot view system-wide stats.
- **Password Hashing**: Passwords are hashed using the DJB2 algorithm (a lightweight placeholder) before being stored in the database. 
- **Session Management**: Thread-safe active session tracking links a socket file descriptor to an authenticated user ID.

## 6. Testing & Validation

- **Automated Storage Tests**: `make test_storage` runs 10 integration tests against the DBMS engine, testing B+ Tree operations, page I/O, Buffer Pool pinning, and WAL recovery. All tests pass with 100% success.
- **Concurrency Stress Test**: `scripts/stress_test.sh` spawns 5 concurrent background clients that all try to book the exact same seats simultaneously. The lock manager correctly serializes these requests, ensuring exactly 1 client succeeds and the others are rejected, proving the 2PL implementation works.
- **Automated Demo**: `scripts/demo.sh` runs through a full end-to-end user scenario.

## 7. Conclusion

The Event Ticketing Platform successfully implements a highly complex, performant, and safe backend system in C from scratch. By writing our own storage engine and concurrency control mechanisms, it deeply demonstrates the critical intersections between Operating Systems and Database Management Systems.
