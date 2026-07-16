#!/bin/bash
# ============================================================================
# Event Ticketing Platform — Stress Test
# ============================================================================
# Simulates concurrent booking by spawning multiple background clients that
# all attempt to book the same seats at the same time.
# Proves that the 2PL and lock manager correctly serialize transactions and
# prevent double-booking.
# ============================================================================

# NOTE: Intentionally NOT using `set -e` because grep returning no match
# would silently abort the script.

CLIENT_BIN="./bin/etp_client"
HOST="127.0.0.1"
PORT=9090

# Ensure we're in the project root
if [ ! -f "$CLIENT_BIN" ]; then
    echo "Error: Run this from the project root (where ./bin/etp_client exists)"
    exit 1
fi

echo "============================================================"
echo " Starting Concurrent Booking Stress Test"
echo "============================================================"

# Helper function to send a sequence of commands
run_client() {
    local username=$1
    local password=$2
    local event_id=$3
    local seats=$4
    local delay=$5
    local id=$6

    # Sleep for a tiny bit to stagger client starts slightly
    sleep $delay

    # We pipe commands into the client. The client will disconnect when EOF is reached.
    (
        echo "LOGIN $username $password"
        sleep 0.1
        echo "BOOK $event_id $seats"
        sleep 0.1
        echo "QUIT"
    ) | $CLIENT_BIN -h $HOST -p $PORT > /dev/null 2>&1
    
    echo "Client $id ($username) finished attempting to book."
}

# 1. Setup: Create a test event and a bunch of users
echo "[Setup] Preparing event and users..."
# Admin logs in, creates event
(
    echo "LOGIN admin admin123"
    echo "CREATE_EVENT StressConcert Arena 2026-10-10 20:00 2 10 500" # 20 seats
    echo "REGISTER testuser1 pass customer"
    echo "REGISTER testuser2 pass customer"
    echo "REGISTER testuser3 pass customer"
    echo "REGISTER testuser4 pass customer"
    echo "REGISTER testuser5 pass customer"
    sleep 1
    echo "QUIT"
) | $CLIENT_BIN -h $HOST -p $PORT > /dev/null 2>&1

echo "[Setup] Ready."
echo ""

# 2. The race: 5 clients all try to book seats 1, 2, 3, 4, 5 simultaneously
echo "[Test] 5 clients racing to book the same seats..."
run_client "testuser1" "pass" 1 "1 2 3 4 5" 0.01 1 &
run_client "testuser2" "pass" 1 "5 4 3 2 1" 0.01 2 &
run_client "testuser3" "pass" 1 "1 3 5 2 4" 0.01 3 &
run_client "testuser4" "pass" 1 "2 1 4 3 5" 0.01 4 &
run_client "testuser5" "pass" 1 "3 2 1 5 4" 0.01 5 &

wait
echo ""
echo "[Test] All clients finished."

# 3. Verify: Check how many bookings actually succeeded (should be exactly 1)
echo "[Verify] Checking event revenue and occupancy..."
(
    echo "LOGIN admin admin123"
    echo "REVENUE 1"
    sleep 1
    echo "QUIT"
) | $CLIENT_BIN -h $HOST -p $PORT | grep -A 4 "Event: StressConcert"

echo ""
echo "Conclusion: If occupancy is 25% (5 seats booked out of 20), the lock manager"
echo "            successfully serialized the concurrent requests and prevented"
echo "            double-booking!"
echo "============================================================"
