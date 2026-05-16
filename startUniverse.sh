#!/usr/bin/env bash
# =============================================================================
# startUniverse.sh (_CPP) — thin shim that delegates to RealityEngine_AI's
# unified orchestrator with --re-engine=cpp --pe-engine=cpp pre-selected.
#
# Operators sitting in the _CPP repo can run ./startUniverse.sh and get the
# same engine-selectable universe as if they'd run it from _AI.  All flags
# accepted by RealityEngine_AI/startUniverse.sh are forwarded unchanged.
#
# When the AI orchestrator isn't available, falls back to launching the
# native CPP binaries via ./start.sh directly.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AI_ORCHESTRATOR="$SCRIPT_DIR/../RealityEngine_AI/startUniverse.sh"

if [ -x "$AI_ORCHESTRATOR" ]; then
  exec "$AI_ORCHESTRATOR" --re-engine=cpp --pe-engine=cpp "$@"
fi

echo "_AI orchestrator not found at $AI_ORCHESTRATOR — falling back to native start"
exec "$SCRIPT_DIR/start.sh" "$@"
