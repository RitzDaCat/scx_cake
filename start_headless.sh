#!/bin/bash
# start_headless.sh
# Starts scx_cake in the background, fully detached.
# Logs are written to /tmp/scx_cake.log

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/target/release/scx_cake"
PID_FILE="/tmp/scx_cake.pid"
LOG_FILE="/tmp/scx_cake.log"

# 1. Check Root
if [ "$EUID" -ne 0 ]; then
    echo "Error: scx_cake requires root privileges."
    echo "Please run: sudo $0 $@"
    exit 1
fi

# 2. Check Binary
if [ ! -f "$BINARY" ]; then
    echo "Error: Binary not found at $BINARY"
    echo "Please run ./build.sh first"
    exit 1
fi

# 3. Check if already running
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if ps -p "$OLD_PID" > /dev/null 2>&1; then
        echo "Error: scx_cake is already running (PID: $OLD_PID)."
        echo "Stop it first with ./stop_headless.sh"
        exit 1
    else
        # Stale PID file
        rm "$PID_FILE"
    fi
fi

echo "Starting scx_cake in headless mode..."
echo "Logs: $LOG_FILE"

# 4. Start Background Process
# nohup: ignores SIGHUP (terminal close)
# > $LOG_FILE: redirects output
# &: puts in background
nohup "$BINARY" "$@" > "$LOG_FILE" 2>&1 &

# 5. Save PID
PID=$!
echo "$PID" > "$PID_FILE"
echo "Success. Scheduler running with PID: $PID"
