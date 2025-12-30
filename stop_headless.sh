#!/bin/bash
# stop_headless.sh
# Stops the scx_cake instance started by start_headless.sh

set -e

PID_FILE="/tmp/scx_cake.pid"

# 1. Check Root
if [ "$EUID" -ne 0 ]; then
    echo "Error: scx_cake requires root privileges."
    echo "Please run: sudo $0"
    exit 1
fi

# 2. Check PID file
if [ ! -f "$PID_FILE" ]; then
    echo "Error: No active PID file found at $PID_FILE."
    echo "Is the scheduler running headless?"
    # Fallback: Check scxctl? No, just warn.
    exit 1
fi

PID=$(cat "$PID_FILE")

# 3. Stop Process
if ps -p "$PID" > /dev/null 2>&1; then
    echo "Stopping scx_cake (PID: $PID)..."
    kill "$PID"
    
    # Wait for it to exit
    TIMEOUT=5
    while ps -p "$PID" > /dev/null 2>&1; do
        if [ "$TIMEOUT" -le 0 ]; then
            echo "Timed out waiting for exit. Sending SIGKILL..."
            kill -9 "$PID"
            break
        fi
        sleep 1
        TIMEOUT=$((TIMEOUT-1))
    done
    
    echo "Scheduler stopped."
else
    echo "Process $PID not found. Cleaning up stale PID file."
fi

# 4. Cleanup
rm -f "$PID_FILE"
