#!/bin/bash
# ============================================================================
# Event Ticketing Platform — Automated Demo Walkthrough
# ============================================================================
# Connects to the server and runs through a full scenario:
# 1. Admin login & creating an organizer
# 2. Organizer creating an event
# 3. Customer registering & booking tickets
# 4. Organizer checking revenue
# 5. Admin checking system stats
# ============================================================================

CLIENT_BIN="./bin/etp_client"
HOST="127.0.0.1"
PORT=9090

if [ ! -f "$CLIENT_BIN" ]; then
    echo "Error: Run this from the project root (where ./bin/etp_client exists)"
    exit 1
fi

echo "============================================================"
echo " Starting Automated Demo Walkthrough"
echo " Make sure the server is running on port $PORT"
echo "============================================================"

# Send commands with small delays to avoid pipe coalescing.
# Note: Multi-word args use single words (parser tokenizes by whitespace).
(
    echo "LOGIN admin admin123"
    sleep 0.2
    echo "REGISTER org1 pass organizer"
    sleep 0.2
    echo "LOGOUT"
    sleep 0.2
    echo "LOGIN org1 pass"
    sleep 0.2
    echo "CREATE_EVENT SummerFest CentralPark 2026-07-20 14:00 5 10 1500"
    sleep 0.2
    echo "LIST_EVENTS"
    sleep 0.2
    echo "LOGOUT"
    sleep 0.2
    echo "REGISTER alice pass1 customer"
    sleep 0.2
    echo "LOGIN alice pass1"
    sleep 0.2
    echo "VIEW_SEATS 1"
    sleep 0.2
    echo "BOOK 1 1 2 3"
    sleep 0.2
    echo "MY_BOOKINGS"
    sleep 0.2
    echo "LOGOUT"
    sleep 0.2
    echo "LOGIN org1 pass"
    sleep 0.2
    echo "REVENUE 1"
    sleep 0.2
    echo "LOGOUT"
    sleep 0.2
    echo "LOGIN admin admin123"
    sleep 0.2
    echo "LIST_USERS"
    sleep 0.2
    echo "SYSTEM_STATS"
    sleep 0.2
    echo "QUIT"
    sleep 0.5
) | $CLIENT_BIN -h $HOST -p $PORT

echo ""
echo "============================================================"
echo " Demo completed!"
echo "============================================================"
