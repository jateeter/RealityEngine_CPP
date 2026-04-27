#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info() { echo -e "${YELLOW}i${NC} $1"; }
ok() { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${RED}!${NC} $1"; }

RUN_DIR="$ROOT_DIR/run"

stop_pid_file() {
  local label="$1"
  local pid_file="$2"

  if [ ! -f "$pid_file" ]; then
    info "$label is not running (no $(basename "$pid_file"))"
    return 0
  fi

  local pid
  pid="$(cat "$pid_file" 2>/dev/null || true)"
  if [ -z "$pid" ]; then
    rm -f "$pid_file"
    info "$label had an empty PID file; removed it"
    return 0
  fi

  if ! ps -p "$pid" >/dev/null 2>&1; then
    rm -f "$pid_file"
    info "$label was not running; removed stale PID file"
    return 0
  fi

  info "Stopping $label (PID $pid)..."
  kill -TERM "$pid" 2>/dev/null || true

  local waited=0
  while ps -p "$pid" >/dev/null 2>&1 && [ "$waited" -lt 15 ]; do
    sleep 1
    waited=$((waited + 1))
  done

  if ps -p "$pid" >/dev/null 2>&1; then
    warn "$label did not exit after ${waited}s; sending SIGKILL"
    kill -KILL "$pid" 2>/dev/null || true
  fi

  rm -f "$pid_file"
  ok "$label stopped"
}

echo "=================================================="
echo "RealityEngine_CPP - Graceful Shutdown"
echo "=================================================="
echo ""

# Stop the pusher first so it cannot send work into a shutting-down Reality Engine.
stop_pid_file "Perception Engine" "$RUN_DIR/perception_engine.pid"
stop_pid_file "Reality Engine" "$RUN_DIR/reality_engine.pid"

echo ""
echo "Persistent shared data preserved:"
echo "  - Qdrant is owned by localAIStack and was not stopped."
echo "  - Qdrant data repository remains ../localAIStack/volumes/qdrant"
echo ""
echo "To also stop Qdrant and localAIStack:"
echo "  cd ../localAIStack && ./scripts/stop.sh"

