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

# Accept --instance=<id> to stop a specific named instance
INSTANCE_ID="${INSTANCE_ID:-}"
for _arg in "$@"; do
  case "$_arg" in
    --instance=*) INSTANCE_ID="${_arg#--instance=}" ;;
  esac
done
_INST="${INSTANCE_ID:+-${INSTANCE_ID}}"

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
stop_pid_file "Perception Engine" "$RUN_DIR/perception_engine${_INST}.pid"
stop_pid_file "Reality Engine"    "$RUN_DIR/reality_engine${_INST}.pid"

# The ports this repo's engines listen on, resolved the same way start.sh
# resolves them so an override reaches both.
REALITY_ENGINE_PORT="${REALITY_ENGINE_PORT:-5301}"
PERCEPTION_ENGINE_PORT="${PERCEPTION_ENGINE_PORT:-5300}"

# Report anything still holding a port after the PID-file stops.
#
# stop.sh only knows what start.sh started — it stops PIDs from run/*.pid. An
# engine spawned by RealityEngine_CI/startUniverse.sh writes no PID file here,
# so this script said "not running", exited 0, and left the process serving.
# start.sh then refused to bind ("Port N is already in use") and the old binary
# kept answering, healthy, on the port under test.
#
# That makes a rebuild-and-retest loop measure the previous build while every
# signal reports success — which cost a correct fix being reported as
# ineffective twice (#59). Reporting success while the port is still held is
# the specific lie; this ends it.
still_bound=0
check_port_released() {
  local label="$1"
  local port="$2"
  command -v lsof >/dev/null 2>&1 || return 0
  local holder
  holder="$(lsof -nP -iTCP:"$port" -sTCP:LISTEN 2>/dev/null | awk 'NR>1 {print $2; exit}')"
  [ -n "$holder" ] || return 0
  still_bound=1
  warn "$label port $port is still held by PID $holder after shutdown"
  echo "    $(ps -o command= -p "$holder" 2>/dev/null | cut -c1-100)"
  echo "    This process was not started by this repo's start.sh — most likely by"
  echo "    RealityEngine_CI/startUniverse.sh. stop.sh only stops what it started."
  echo "    Stop it there, or: kill $holder"
}

check_port_released "Reality Engine"    "$REALITY_ENGINE_PORT"
check_port_released "Perception Engine" "$PERCEPTION_ENGINE_PORT"

if [ "$still_bound" -ne 0 ]; then
  echo ""
  warn "Shutdown incomplete — a port this repo uses is still serving."
  warn "Do not treat a rebuild as deployed until it is released."
  exit 1
fi

echo ""
echo "Persistent shared data preserved:"
echo "  - Qdrant is owned by localAIStack and was not stopped."
echo "  - Qdrant data repository remains ../localAIStack/volumes/qdrant"
echo ""
echo "To also stop Qdrant and localAIStack:"
echo "  cd ../localAIStack && ./scripts/stop.sh"

