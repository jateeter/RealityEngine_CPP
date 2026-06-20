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
die() { warn "$1"; exit 1; }

if [ -f .env ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|\#*) continue ;;
    esac
    key="${line%%=*}"
    value="${line#*=}"
    case "$key" in
      ''|*[!A-Za-z0-9_]*) continue ;;
    esac
    if [ -z "${!key+x}" ]; then
      export "$key=$value"
    fi
  done < .env
fi

REALITY_ENGINE_PORT="${REALITY_ENGINE_PORT:-5301}"
PERCEPTION_ENGINE_PORT="${PERCEPTION_ENGINE_PORT:-5300}"
VECTOR_DIMENSION="${VECTOR_DIMENSION:-7680}"
MACHINES_DIR="${MACHINES_DIR:-../RealityEngine_Machines/machines}"
RE_LOAD_MACHINES="${RE_LOAD_MACHINES:-1}"
# INSTANCE_ID — when set, PID/log files are suffixed so multiple CPP
# instances can run from the same repo directory simultaneously.
INSTANCE_ID="${INSTANCE_ID:-}"
_INST="${INSTANCE_ID:+-${INSTANCE_ID}}"   # "" or "-<id>"
QDRANT_URL="${QDRANT_URL:-http://localhost:4333}"
QDRANT_GRPC_URL="${QDRANT_GRPC_URL:-http://localhost:4334}"
QDRANT_STORAGE_DIR="${QDRANT_STORAGE_DIR:-../localAIStack/volumes/qdrant}"
QDRANT_LOCALAI_COLLECTION="${QDRANT_LOCALAI_COLLECTION:-localai_docs}"
QDRANT_REALITY_COLLECTION="${QDRANT_REALITY_COLLECTION:-reality-vectors}"
LOCAL_AI_API_URL="${LOCAL_AI_API_URL:-http://localhost:4000}"
LOCAL_AI_MACHINES_DIR="${LOCAL_AI_MACHINES_DIR:-../localAIStack/data/machines}"
LOCAL_AI_BOOTSTRAP="${LOCAL_AI_BOOTSTRAP:-false}"

# ── MQTT bridge env passthrough ────────────────────────────────────────────
# When MQTT_BROKER_HOST is set, the PE will boot the MQTT bridge from
# MQTT_MAPPINGS_FILE (or MQTT_MAPPINGS_JSON / MQTT_TOPIC_MAP).  Defaults
# point at the yuma-agriculture demo mapping file shipped in config/;
# operators override by exporting the env vars before invoking start.sh.
MQTT_BROKER_HOST="${MQTT_BROKER_HOST:-}"
MQTT_BROKER_PORT="${MQTT_BROKER_PORT:-1883}"
MQTT_CLIENT_ID="${MQTT_CLIENT_ID:-reality-engine-pe}"
MQTT_KEEPALIVE="${MQTT_KEEPALIVE:-60}"
MQTT_MAPPINGS_FILE="${MQTT_MAPPINGS_FILE:-}"
MQTT_ALLOW_REGION_OVERLAP="${MQTT_ALLOW_REGION_OVERLAP:-0}"

# PE integration registry — HealthKit / CareKit / completion adapters.
# When set, the PE loads the JSON file at startup and routes inbound
# /api/integrations/healthkit/ingest and /api/integrations/carekit/ingest
# payloads to the declared sensor regions.
# localAIStack config: ../localAIStack/config/pe-integrations.json
_DEFAULT_INTEGRATIONS_CONFIG="../RealityEngine_CI/config/integrations.json"
if [ -z "${INTEGRATIONS_CONFIG:-}" ] && [ -f "$_DEFAULT_INTEGRATIONS_CONFIG" ]; then
  INTEGRATIONS_CONFIG="$_DEFAULT_INTEGRATIONS_CONFIG"
else
  INTEGRATIONS_CONFIG="${INTEGRATIONS_CONFIG:-}"
fi

# OpenClaw ACP defaults shared with CI and the other engine runtimes.
ACP_ENABLED="${ACP_ENABLED:-true}"
ACP_PLATFORM="${ACP_PLATFORM:-OpenClaw}"
ACP_SURFACE="${ACP_SURFACE:-xACP}"
ACP_GATEWAY_URL="${ACP_GATEWAY_URL:-${OPENCLAW_GATEWAY_URL:-ws://127.0.0.1:18789}}"
OPENCLAW_GATEWAY_URL="${OPENCLAW_GATEWAY_URL:-$ACP_GATEWAY_URL}"
ACP_SESSION_KEY="${ACP_SESSION_KEY:-${OPENCLAW_ACP_SESSION:-agent:main:main}}"
OPENCLAW_ACP_SESSION="${OPENCLAW_ACP_SESSION:-$ACP_SESSION_KEY}"
ACP_TARGET_AGENT="${ACP_TARGET_AGENT:-openclaw}"
ACP_COMPLETION_SOURCE_MAPPING_ID="${ACP_COMPLETION_SOURCE_MAPPING_ID:-acp-openclaw-completion}"

RUN_DIR="$ROOT_DIR/run"
LOG_DIR="$ROOT_DIR/logs"
mkdir -p "$RUN_DIR" "$LOG_DIR"

# Per-instance PID and log file paths (suffixed when INSTANCE_ID is set)
RE_PID_FILE="$RUN_DIR/reality_engine${_INST}.pid"
PE_PID_FILE="$RUN_DIR/perception_engine${_INST}.pid"
RE_LOG_FILE="$LOG_DIR/reality_engine${_INST}.log"
PE_LOG_FILE="$LOG_DIR/perception_engine${_INST}.log"

usage() {
  cat <<USAGE
Usage: ./start.sh [--no-build] [--allow-missing-qdrant]

Starts RealityEngine_CPP native services:
  - Reality Engine API    : http://localhost:${REALITY_ENGINE_PORT}
  - Perception Engine API : http://localhost:${PERCEPTION_ENGINE_PORT}
  - Vector dimension      : ${VECTOR_DIMENSION}

Qdrant is shared with localAIStack and is verified but not managed here:
  - REST    : ${QDRANT_URL}
  - Storage : ${QDRANT_STORAGE_DIR}

Optional local AI integration:
  - API       : ${LOCAL_AI_API_URL}
  - Machines  : ${LOCAL_AI_MACHINES_DIR}
  - Bootstrap : ${LOCAL_AI_BOOTSTRAP}
USAGE
}

NO_BUILD=false
ALLOW_MISSING_QDRANT=false
for arg in "$@"; do
  case "$arg" in
    --no-build) NO_BUILD=true ;;
    --allow-missing-qdrant) ALLOW_MISSING_QDRANT=true ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown argument: $arg" ;;
  esac
done

check_port_free() {
  local port="$1"
  local pid_file="$2"
  if [ -f "$pid_file" ]; then
    local pid
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -n "$pid" ] && ps -p "$pid" >/dev/null 2>&1; then
      die "Service already running from $pid_file (PID $pid). Use ./stop.sh first."
    fi
    rm -f "$pid_file"
  fi

  if lsof -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    lsof -iTCP:"$port" -sTCP:LISTEN
    die "Port $port is already in use."
  fi
}

wait_for_http() {
  local url="$1"
  local name="$2"
  local i
  for i in $(seq 1 30); do
    if curl -sf "$url" >/dev/null 2>&1; then
      ok "$name ready at $url"
      return 0
    fi
    sleep 0.3
  done
  return 1
}

machine_count_from_api() {
  local url="$1"
  curl -sf "$url/api/machines" 2>/dev/null | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
    print(len(data.get("machines", [])))
except Exception:
    print(0)
'
}

echo "=================================================="
echo "RealityEngine_CPP - Starting Native Services"
echo "=================================================="
echo ""

if [ "$NO_BUILD" = false ]; then
  info "Building C++ services..."
  make >/dev/null
  ok "Build complete"
else
  [ -x bin/reality_engine_server ] || die "bin/reality_engine_server missing. Run make or omit --no-build."
  [ -x bin/perception_engine_server ] || die "bin/perception_engine_server missing. Run make or omit --no-build."
fi

# When RE_LOAD_MACHINES=0 the RE starts with no corpus (CI will seed separately).
# Use a temp empty directory so the binary still receives a valid path argument.
if [ "${RE_LOAD_MACHINES}" = "0" ]; then
    _machines_load_dir=$(mktemp -d)
    trap 'rm -rf "$_machines_load_dir"' EXIT
    ok "Machine load disabled (RE_LOAD_MACHINES=0) — RE will start empty"
else
    [ -d "$MACHINES_DIR" ] || die "Machine directory not found: $MACHINES_DIR"
    _machines_load_dir="$MACHINES_DIR"
    ok "Machine repository: $MACHINES_DIR"
fi

info "Checking unified Qdrant at $QDRANT_URL..."
if curl -sf "$QDRANT_URL/collections" >/dev/null 2>&1 || curl -sf "$QDRANT_URL/healthz" >/dev/null 2>&1; then
  ok "Unified Qdrant reachable"
else
  if [ "$ALLOW_MISSING_QDRANT" = true ]; then
    warn "Qdrant not reachable; continuing because --allow-missing-qdrant was provided"
  else
    cat <<EOF

RealityEngine_CPP uses the same unified Qdrant instance as RealityEngine_AI and localAIStack.
Start localAIStack first, then retry:

  cd ../localAIStack && ./scripts/start.sh

EOF
    exit 1
  fi
fi

if [ -d "$QDRANT_STORAGE_DIR" ]; then
  ok "Qdrant storage repository: $QDRANT_STORAGE_DIR"
else
  warn "Qdrant storage directory not found yet: $QDRANT_STORAGE_DIR"
fi

info "Checking optional local AI API at $LOCAL_AI_API_URL..."
if curl -sf "$LOCAL_AI_API_URL/health" >/dev/null 2>&1; then
  ok "local AI API reachable"
else
  warn "local AI API not reachable; perception external-signal endpoints still available"
fi

if [ "$LOCAL_AI_BOOTSTRAP" = "true" ] || [ "$LOCAL_AI_BOOTSTRAP" = "1" ] || [ "$LOCAL_AI_BOOTSTRAP" = "yes" ]; then
  [ -d "$LOCAL_AI_MACHINES_DIR" ] || warn "local AI machine directory not found: $LOCAL_AI_MACHINES_DIR"
  info "local AI bootstrap enabled; Perception Engine will register localAIStack sensors and import bridge machines"
fi

if command -v python3 >/dev/null 2>&1 && curl -sf "$QDRANT_URL/collections" >/tmp/re_cpp_qdrant_collections.json 2>/dev/null; then
  python3 - /tmp/re_cpp_qdrant_collections.json "$QDRANT_LOCALAI_COLLECTION" "$QDRANT_REALITY_COLLECTION" <<'PY' || true
import json, sys
path = sys.argv[1]
expected = set(sys.argv[2:])
try:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    names = {c.get("name") for c in data.get("result", {}).get("collections", [])}
except Exception:
    names = set()
missing = sorted(expected - names)
if missing:
    print("! Qdrant collections not present yet: " + ", ".join(missing))
else:
    print("✓ Qdrant collections present: " + ", ".join(sorted(expected)))
PY
  rm -f /tmp/re_cpp_qdrant_collections.json
fi

check_port_free "$REALITY_ENGINE_PORT" "$RE_PID_FILE"
check_port_free "$PERCEPTION_ENGINE_PORT" "$PE_PID_FILE"

export QDRANT_URL QDRANT_GRPC_URL QDRANT_STORAGE_DIR QDRANT_LOCALAI_COLLECTION QDRANT_REALITY_COLLECTION
export LOCAL_AI_API_URL LOCAL_AI_MACHINES_DIR LOCAL_AI_BOOTSTRAP
export VECTOR_DIMENSION
# MQTT bridge: env vars consumed by perception_engine_server when present.
export MQTT_BROKER_HOST MQTT_BROKER_PORT MQTT_CLIENT_ID MQTT_KEEPALIVE
export MQTT_MAPPINGS_FILE MQTT_ALLOW_REGION_OVERLAP
# Integration registry: consumed by perception_engine_server to route
# HealthKit / CareKit ingest payloads to PE sensor regions.
export INTEGRATIONS_CONFIG
export ACP_ENABLED ACP_PLATFORM ACP_SURFACE ACP_GATEWAY_URL OPENCLAW_GATEWAY_URL
export ACP_SESSION_KEY OPENCLAW_ACP_SESSION ACP_TARGET_AGENT ACP_COMPLETION_SOURCE_MAPPING_ID

info "Starting Reality Engine on port $REALITY_ENGINE_PORT${INSTANCE_ID:+ [instance: $INSTANCE_ID]}..."
nohup "$ROOT_DIR/bin/reality_engine_server" "$REALITY_ENGINE_PORT" "$_machines_load_dir" "$VECTOR_DIMENSION" \
  > "$RE_LOG_FILE" 2>&1 &
echo $! > "$RE_PID_FILE"
sleep 0.2
if ! ps -p "$(cat "$RE_PID_FILE")" >/dev/null 2>&1; then
  tail -40 "$RE_LOG_FILE" || true
  bash "$ROOT_DIR/stop.sh" "${INSTANCE_ID:+--instance=$INSTANCE_ID}" >/dev/null 2>&1 || true
  die "Reality Engine exited before becoming healthy"
fi

if ! wait_for_http "http://localhost:${REALITY_ENGINE_PORT}/api/health" "Reality Engine"; then
  tail -40 "$RE_LOG_FILE" || true
  bash "$ROOT_DIR/stop.sh" "${INSTANCE_ID:+--instance=$INSTANCE_ID}" >/dev/null 2>&1 || true
  die "Reality Engine failed to become healthy"
fi

if [ "${RE_LOAD_MACHINES}" != "0" ]; then
  info "Verifying RealityEngine_Machines corpus loaded..."
  if ! command -v python3 >/dev/null 2>&1; then
    ./stop.sh >/dev/null 2>&1 || true
    die "python3 is required to verify startup machine loading"
  fi
  MACHINE_COUNT="$(machine_count_from_api "http://localhost:${REALITY_ENGINE_PORT}")"
  if [ "${MACHINE_COUNT:-0}" -le 0 ]; then
    tail -60 "$RE_LOG_FILE" || true
    ./stop.sh >/dev/null 2>&1 || true
    die "Reality Engine started but loaded 0 machines from $MACHINES_DIR"
  fi
  ok "Loaded $MACHINE_COUNT machine(s) from $MACHINES_DIR"
else
  ok "RE started empty (RE_LOAD_MACHINES=0) — corpus will be seeded by CI"
fi

info "Starting Perception Engine on port $PERCEPTION_ENGINE_PORT${INSTANCE_ID:+ [instance: $INSTANCE_ID]}..."
nohup "$ROOT_DIR/bin/perception_engine_server" "$PERCEPTION_ENGINE_PORT" "http://localhost:${REALITY_ENGINE_PORT}" "$LOCAL_AI_API_URL" "$LOCAL_AI_MACHINES_DIR" "$VECTOR_DIMENSION" \
  > "$PE_LOG_FILE" 2>&1 &
echo $! > "$PE_PID_FILE"
sleep 0.2
if ! ps -p "$(cat "$PE_PID_FILE")" >/dev/null 2>&1; then
  tail -40 "$PE_LOG_FILE" || true
  bash "$ROOT_DIR/stop.sh" "${INSTANCE_ID:+--instance=$INSTANCE_ID}" >/dev/null 2>&1 || true
  die "Perception Engine exited before becoming healthy"
fi

if ! wait_for_http "http://localhost:${PERCEPTION_ENGINE_PORT}/api/health" "Perception Engine"; then
  tail -40 "$PE_LOG_FILE" || true
  bash "$ROOT_DIR/stop.sh" "${INSTANCE_ID:+--instance=$INSTANCE_ID}" >/dev/null 2>&1 || true
  die "Perception Engine failed to become healthy"
fi

echo ""
echo "=================================================="
echo "RealityEngine_CPP Running"
echo "=================================================="
printf "  %-24s %s\n" "Reality Engine" "http://localhost:${REALITY_ENGINE_PORT}"
printf "  %-24s %s\n" "Perception Engine" "http://localhost:${PERCEPTION_ENGINE_PORT}"
printf "  %-24s %s\n" "Vector dimension" "${VECTOR_DIMENSION}"
printf "  %-24s %s\n" "local AI API" "${LOCAL_AI_API_URL}"
printf "  %-24s %s\n" "Qdrant shared store" "${QDRANT_URL}"
printf "  %-24s %s\n" "Qdrant data repo" "${QDRANT_STORAGE_DIR}"
# Show the resolved MQTT bridge intent — the PE itself will log "MQTT
# bridge enabled" or remain quiet when no broker is configured.
if [ -n "$MQTT_BROKER_HOST" ]; then
  printf "  %-24s %s\n" "MQTT broker" "${MQTT_BROKER_HOST}:${MQTT_BROKER_PORT}"
  [ -n "$MQTT_MAPPINGS_FILE" ] && printf "  %-24s %s\n" "MQTT mappings" "${MQTT_MAPPINGS_FILE}"
fi
[ -n "$INTEGRATIONS_CONFIG" ] && printf "  %-24s %s\n" "Integrations config" "${INTEGRATIONS_CONFIG}"
printf "  %-24s %s\n" "OpenClaw ACP" "enabled=${ACP_ENABLED} gateway=${ACP_GATEWAY_URL}"
printf "  %-24s %s\n" "OpenClaw mapping" "${ACP_COMPLETION_SOURCE_MAPPING_ID}"
echo ""
echo "Logs:"
echo "  $RE_LOG_FILE"
echo "  $PE_LOG_FILE"
echo ""
echo "Stop with: ./stop.sh"
