#!/usr/bin/env bash
# =============================================================================
# startUniverse.sh (_CPP) — thin shim that delegates to RealityEngine_CI's
# contract-owned orchestrator with --re-engine=cpp --pe-engine=cpp pre-selected.
#
# Operators sitting in the _CPP repo can run ./startUniverse.sh and get the
# same engine-selectable universe as if they'd run it from _CI.  All flags
# accepted by RealityEngine_CI/startUniverse.sh are forwarded unchanged.
#
# When the CI orchestrator isn't available, falls back to launching the
# native CPP binaries via ./start.sh directly.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CI_ORCHESTRATOR="$SCRIPT_DIR/../RealityEngine_CI/startUniverse.sh"

if [ -x "$CI_ORCHESTRATOR" ]; then
  exec "$CI_ORCHESTRATOR" --re-engine=cpp --pe-engine=cpp "$@"
fi

echo "_CI orchestrator not found at $CI_ORCHESTRATOR — falling back to native start"
exec "$SCRIPT_DIR/start.sh" "$@"
