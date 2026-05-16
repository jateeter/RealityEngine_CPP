#!/usr/bin/env bash
# stopUniverse.sh (_CPP) — delegates to the AI orchestrator, falling back
# to the native ./stop.sh when the AI shim isn't available.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AI_ORCHESTRATOR="$SCRIPT_DIR/../RealityEngine_AI/stopUniverse.sh"

if [ -x "$AI_ORCHESTRATOR" ]; then
  exec "$AI_ORCHESTRATOR" --re-engine=cpp --pe-engine=cpp "$@"
fi

echo "_AI orchestrator not found at $AI_ORCHESTRATOR — falling back to native stop"
exec "$SCRIPT_DIR/stop.sh" "$@"
