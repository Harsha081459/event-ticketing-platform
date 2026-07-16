# ============================================================================
# Event Ticketing Platform — Makefile
# ============================================================================
# Build system for the ETP project. Compiles the server, client, and tests.
#
# Usage:
#   make              — Build server and client
#   make test_storage — Build and run storage engine tests
#   make clean        — Remove all build artifacts
# ============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -g -O0 -I.
LDFLAGS = -pthread -lrt

# ── Directories ──────────────────────────────────────────────────────────────
BINDIR  = bin
DATADIR = data

# ── Source Files ─────────────────────────────────────────────────────────────

COMMON_SRCS = common/utils.c

STORAGE_SRCS = server/storage/page.c        \
               server/storage/buffer_pool.c \
               server/storage/btree.c       \
               server/storage/wal.c         \
               server/storage/table.c

NETWORK_SRCS = server/network/tcp_server.c  \
               server/network/thread_pool.c

AUTH_SRCS    = server/auth/auth.c           \
               server/auth/rbac.c

PROTOCOL_SRCS = server/protocol/parser.c

CORE_SRCS    = server/core/event_mgr.c      \
               server/core/booking_engine.c \
               server/core/reports.c

TXN_SRCS     = server/txn/txn_manager.c     \
               server/txn/lock_manager.c

IPC_SRCS     = server/ipc/logger.c          \
               server/ipc/notifier.c        \
               server/ipc/stats.c

SERVER_SRCS  = server/main.c                \
               $(COMMON_SRCS)               \
               $(STORAGE_SRCS)              \
               $(NETWORK_SRCS)              \
               $(AUTH_SRCS)                  \
               $(PROTOCOL_SRCS)             \
               $(CORE_SRCS)                 \
               $(TXN_SRCS)                  \
               $(IPC_SRCS)

CLIENT_SRCS  = client/client.c              \
               $(COMMON_SRCS)

# ── Binaries ─────────────────────────────────────────────────────────────────
SERVER_BIN       = $(BINDIR)/etp_server
CLIENT_BIN       = $(BINDIR)/etp_client
STORAGE_TEST_BIN = $(BINDIR)/test_storage

# ── Targets ──────────────────────────────────────────────────────────────────

.PHONY: all clean dirs test_storage

all: dirs $(SERVER_BIN) $(CLIENT_BIN)
	@echo ""
	@echo "  Build complete!"
	@echo "  Server: $(SERVER_BIN)"
	@echo "  Client: $(CLIENT_BIN)"
	@echo ""

dirs:
	@mkdir -p $(BINDIR) $(DATADIR)

$(SERVER_BIN): $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Phase 1 Test ─────────────────────────────────────────────────────────────
# Build and run the storage engine integration test.

test_storage: dirs
	$(CC) $(CFLAGS) -o $(STORAGE_TEST_BIN) \
		tests/test_storage.c               \
		$(STORAGE_SRCS)                    \
		$(COMMON_SRCS)                     \
		$(LDFLAGS)
	@echo ""
	@echo "  Running storage tests..."
	@echo "  ─────────────────────────"
	@./$(STORAGE_TEST_BIN)

# ── Phase 2 Compile Check ────────────────────────────────────────────────────
# Compiles all Phase 1 + Phase 2 modules into a test binary to verify no errors.

test_phase2: dirs
	$(CC) $(CFLAGS) -o $(BINDIR)/test_phase2 \
		tests/test_storage.c               \
		$(STORAGE_SRCS)                    \
		$(NETWORK_SRCS)                    \
		$(AUTH_SRCS)                        \
		$(PROTOCOL_SRCS)                   \
		$(COMMON_SRCS)                     \
		$(LDFLAGS)
	@echo ""
	@echo "  Phase 2 compile check passed!"
	@echo "  Running storage tests as validation..."
	@./$(BINDIR)/test_phase2

# ── Cleanup ──────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BINDIR) $(DATADIR)
	@echo "  Cleaned."
