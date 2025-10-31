#!/bin/bash
# Local test runner for qtick
# Usage: bash scripts/run_local.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

echo "=== qtick Local Test Runner ==="
echo "Project root: $PROJECT_ROOT"

# Check if built
if [ ! -f "$BUILD_DIR/bridge" ]; then
  echo "Error: binaries not found. Build first:"
  echo "  mkdir -p build && cd build"
  echo "  cmake .. -DCMAKE_BUILD_TYPE=Release"
  echo "  make -j$(nproc)"
  exit 1
fi

# Redis socket path
REDIS_SOCKET="/var/run/redis/redis.sock"
REDIS_CONF="${PROJECT_ROOT}/config/redis.conf"

# kdb+ settings
Q_PORT=5010
Q_HOST="localhost"

# PID file tracking
PIDS=()
cleanup() {
  echo ""
  echo "Cleaning up processes..."
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      echo "Killing $pid"
      kill "$pid" 2>/dev/null || true
    fi
  done
  wait
  echo "Cleanup complete"
}
trap cleanup EXIT INT TERM

# 1. Start Redis
echo "[1/4] Starting Redis..."
if [ -f "$REDIS_CONF" ]; then
  redis-server "$REDIS_CONF" &
  REDIS_PID=$!
  PIDS+=($REDIS_PID)
else
  echo "Starting Redis with default config + unix socket..."
  redis-server --unixsocket "$REDIS_SOCKET" --unixsocketperm 777 \
               --save "" --appendonly no &
  REDIS_PID=$!
  PIDS+=($REDIS_PID)
fi
sleep 1

# Check Redis
if ! redis-cli -s "$REDIS_SOCKET" ping > /dev/null 2>&1; then
  echo "Error: Redis not responding on $REDIS_SOCKET"
  exit 1
fi
echo "Redis ready (PID: $REDIS_PID)"

# 2. Start kdb+ RDB
echo "[2/4] Starting kdb+ RDB on port $Q_PORT..."
cd "$PROJECT_ROOT"
q q/rdb.q -p $Q_PORT &
Q_PID=$!
PIDS+=($Q_PID)
sleep 2

# Check kdb+
if ! ps -p $Q_PID > /dev/null; then
  echo "Error: kdb+ failed to start"
  exit 1
fi
echo "kdb+ RDB ready (PID: $Q_PID)"

# 3. Start bridge
echo "[3/4] Starting ingest bridge..."
cd "$BUILD_DIR"
./bridge "$REDIS_SOCKET" "$Q_HOST" $Q_PORT &
BRIDGE_PID=$!
PIDS+=($BRIDGE_PID)
sleep 1

if ! ps -p $BRIDGE_PID > /dev/null; then
  echo "Error: bridge failed to start"
  exit 1
fi
echo "Bridge ready (PID: $BRIDGE_PID)"

# 4. Start feed simulator
echo "[4/4] Starting feed simulator..."
./feed_sim "$REDIS_SOCKET" &
FEED_PID=$!
PIDS+=($FEED_PID)

echo ""
echo "=== All processes started ==="
echo "Redis:  PID $REDIS_PID (socket: $REDIS_SOCKET)"
echo "kdb+:   PID $Q_PID (port: $Q_PORT)"
echo "Bridge: PID $BRIDGE_PID"
echo "Feed:   PID $FEED_PID"
echo ""
echo "Monitor with:"
echo "  watch -n1 'redis-cli -s $REDIS_SOCKET info stats | grep total_commands'"
echo "  q -p $Q_PORT -e 'getStats[]'"
echo "  tail -f logs/*.log"
echo ""
echo "Press Ctrl+C to stop all processes"
echo ""

# Wait for feed to finish or user interrupt
wait $FEED_PID