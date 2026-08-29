#!/usr/bin/env bash
# stopUniverse.sh (_CPP) — stops this repository's engines.
#
# This used to exec a deprecated prototype's stopUniverse.sh whenever it was
# executable, and only fall back to the native stop when it was missing. That
# made teardown depend on a repository deprecated in June 2026 and frozen since
# — and on a developer machine where the checkout still exists, the delegation
# was the path that actually ran. Multi-engine teardown belongs to
# RealityEngine_CI/stopUniverse.sh; this stops the local engines and nothing
# else.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/stop.sh" "$@"
