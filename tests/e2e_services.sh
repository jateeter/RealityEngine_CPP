#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

REALITY_ENGINE_E2E_PORT="${REALITY_ENGINE_E2E_PORT:-3299}"
PERCEPTION_ENGINE_E2E_PORT="${PERCEPTION_ENGINE_E2E_PORT:-3301}"
VECTOR_DIMENSION="${VECTOR_DIMENSION:-5120}"
MACHINES_DIR="${MACHINES_DIR:-../RealityEngine_AI/examples/machines}"
LOCAL_AI_API_URL="${LOCAL_AI_API_URL:-http://localhost:4000}"
LOCAL_AI_MACHINES_DIR="${LOCAL_AI_MACHINES_DIR:-../localAIStack/data/machines}"

REALITY_PID=""
PERCEPTION_PID=""

cleanup() {
  if [ -n "$PERCEPTION_PID" ] && kill -0 "$PERCEPTION_PID" >/dev/null 2>&1; then
    kill "$PERCEPTION_PID" >/dev/null 2>&1 || true
    wait "$PERCEPTION_PID" >/dev/null 2>&1 || true
  fi
  if [ -n "$REALITY_PID" ] && kill -0 "$REALITY_PID" >/dev/null 2>&1; then
    kill "$REALITY_PID" >/dev/null 2>&1 || true
    wait "$REALITY_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

wait_for_http() {
  local url="$1"
  local name="$2"
  local i
  for i in $(seq 1 40); do
    if curl -sf "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  echo "$name did not become ready at $url" >&2
  return 1
}

assert_machine_count_gt_zero() {
  python3 -c '
import json, sys
data = json.load(sys.stdin)
count = len(data.get("machines", []))
if count <= 0:
    raise SystemExit(f"expected at least one loaded machine, got {count}")
'
}

assert_push_success() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("success") is not True:
    raise SystemExit(f"push did not report success: {data!r}")
if data.get("globalStep", 0) <= 0:
    raise SystemExit(f"push did not advance globalStep: {data!r}")
step = data.get("step")
if not isinstance(step, dict) or not step.get("mergeBatch"):
    raise SystemExit(f"push response did not include a non-empty mergeBatch: {data!r}")
' "$payload"
}

assert_result_count() {
  local payload="$1"
  local expected="$2"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
expected = int(sys.argv[2])
count = len(data.get("results", []))
if count != expected:
    raise SystemExit(f"expected {expected} result(s), got {count}: {data!r}")
' "$payload" "$expected"
}

assert_merge_region() {
  local payload="$1"
  local offset="$2"
  local length="$3"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
offset = int(sys.argv[2])
length = int(sys.argv[3])
step = data.get("step", {})
for merge in step.get("mergeBatch", []):
    region = merge.get("region", {})
    if region.get("offset") == offset and region.get("length") == length:
        break
else:
    raise SystemExit(f"expected merge region offset={offset} length={length}: {data!r}")
space = step.get("perceptualSpace", [])
if len(space) < offset + length or space[offset:offset + length] != [1, 0]:
    raise SystemExit(f"expected perceptualSpace[{offset}:{offset + length}] == [1, 0]: {space[offset:offset + length]!r}")
' "$payload" "$offset" "$length"
}

expected_machine_test_source_count() {
  python3 -c '
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
count = 0
for path in root.glob("*.json"):
    data = json.loads(path.read_text())
    seqs = data.get("machine", {}).get("inputSequences", [])
    if isinstance(seqs, list):
        count += len(seqs)
print(count)
' "$MACHINES_DIR"
}

assert_machine_test_sources() {
  local payload="$1"
  local expected="$2"
  printf "%s" "$payload" | python3 -c '
import json, sys
data = json.load(sys.stdin)
expected = int(sys.argv[1])
sources = data.get("sources", [])
test_sources = [s for s in sources if s.get("type") == "test"]
if len(test_sources) != expected:
    raise SystemExit(f"expected {expected} machine test sources, got {len(test_sources)}")
missing = []
for source in test_sources:
    if not source.get("inputs"):
        missing.append(f"{source.get('id')}:inputs")
    if not isinstance(source.get("metadata"), dict):
        missing.append(f"{source.get('id')}:metadata")
    sequence = source.get("sequence")
    if not isinstance(sequence, dict) or not isinstance(sequence.get("vectors"), list):
        missing.append(f"{source.get('id')}:sequence")
if missing:
    raise SystemExit("test sources missing full representation: " + ", ".join(missing[:20]))
' "$expected"
}

post_machine() {
  local payload="$1"
  curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/machines" \
    -H "Content-Type: application/json" \
    -d "$payload" >/dev/null
}

[ -x bin/reality_engine_server ] || { echo "bin/reality_engine_server missing; run make first" >&2; exit 1; }
[ -x bin/perception_engine_server ] || { echo "bin/perception_engine_server missing; run make first" >&2; exit 1; }
[ -d "$MACHINES_DIR" ] || { echo "Machine directory not found: $MACHINES_DIR" >&2; exit 1; }

echo "E2E service boundary test"
echo "  Reality Engine port:    $REALITY_ENGINE_E2E_PORT"
echo "  Perception Engine port: $PERCEPTION_ENGINE_E2E_PORT"

bin/reality_engine_server "$REALITY_ENGINE_E2E_PORT" "$MACHINES_DIR" "$VECTOR_DIMENSION" >/tmp/reality_engine_e2e.log 2>&1 &
REALITY_PID="$!"
wait_for_http "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/health" "Reality Engine"

curl -sf "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/machines" | assert_machine_count_gt_zero

curl -sf "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/demo/multi-step" >/dev/null
curl -sf "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/demo/data-center" >/dev/null
curl -sf "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/demo/kleene-star" >/dev/null

curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/vectors" \
  -H "Content-Type: application/json" \
  -d '{"id":"e2e-search-vector","elements":[{"value":1},{"value":0}]}' >/dev/null
search_hit="$(curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/vectors/search" -H "Content-Type: application/json" -d '{"vector":[1,0],"threshold":0.99,"limit":10}')"
assert_result_count "$search_hit" 1
search_miss="$(curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/vectors/search" -H "Content-Type: application/json" -d '{"vector":[0,1],"threshold":0.99,"limit":10}')"
assert_result_count "$search_miss" 0

perceive_payload="$(python3 -c 'import json, sys; dimension = int(sys.argv[1]); print(json.dumps({"vector":[0] * dimension,"matchAlgorithm":"equals","compact":True}))' "$VECTOR_DIMENSION")"
curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/perceive" \
  -H "Content-Type: application/json" \
  -d "$perceive_payload" >/dev/null

bin/perception_engine_server "$PERCEPTION_ENGINE_E2E_PORT" "http://localhost:${REALITY_ENGINE_E2E_PORT}" "$LOCAL_AI_API_URL" "$LOCAL_AI_MACHINES_DIR" "$VECTOR_DIMENSION" >/tmp/perception_engine_e2e.log 2>&1 &
PERCEPTION_PID="$!"
wait_for_http "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/health" "Perception Engine"

expected_test_sources="$(expected_machine_test_source_count)"
startup_sources="$(curl -sf "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/sources")"
assert_machine_test_sources "$startup_sources" "$expected_test_sources"

push_payload="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_push_success "$push_payload"

chain_a='{"version":"1.0.0","machine":{"name":"E2E Chain A","description":"First machine in PE-to-RE stream propagation test","arbiterRule":"PASSTHROUGH","perceptualMapping":{"input":{"offset":4600,"length":2},"output":{"offset":4602,"length":2}},"sequences":[{"id":"e2e-chain-a-seq","name":"A input stream emits B input","vectors":[{"id":"e2e-chain-a-start","elements":[{"value":1,"threshold":0.5},{"value":0,"threshold":0.5}],"isInitial":true,"nextVectorIds":["e2e-chain-a-output"]},{"id":"e2e-chain-a-output","elements":[{"value":0,"threshold":0.5},{"value":1,"threshold":0.5}],"isInitial":false,"outputVectors":[{"id":"e2e-chain-a-out","vector":[1,0],"metadata":{"boundary":"A->B"}}]}]}]}}'
chain_b='{"version":"1.0.0","machine":{"name":"E2E Chain B","description":"Second machine in PE-to-RE stream propagation test","arbiterRule":"PASSTHROUGH","perceptualMapping":{"input":{"offset":4602,"length":2},"output":{"offset":4604,"length":2}},"sequences":[{"id":"e2e-chain-b-seq","name":"B consumes A output and emits C input","vectors":[{"id":"e2e-chain-b-output","elements":[{"value":1,"threshold":0.5},{"value":0,"threshold":0.5}],"isInitial":true,"outputVectors":[{"id":"e2e-chain-b-out","vector":[1,0],"metadata":{"boundary":"B->C"}}]}]}]}}'
chain_c='{"version":"1.0.0","machine":{"name":"E2E Chain C","description":"Third machine in PE-to-RE stream propagation test","arbiterRule":"PASSTHROUGH","perceptualMapping":{"input":{"offset":4604,"length":2},"output":{"offset":4606,"length":2}},"sequences":[{"id":"e2e-chain-c-seq","name":"C consumes B output and emits terminal output","vectors":[{"id":"e2e-chain-c-output","elements":[{"value":1,"threshold":0.5},{"value":0,"threshold":0.5}],"isInitial":true,"outputVectors":[{"id":"e2e-chain-c-out","vector":[1,0],"metadata":{"boundary":"C terminal"}}]}]}]}}'

post_machine "$chain_a"
post_machine "$chain_b"
post_machine "$chain_c"

curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/sources" \
  -H "Content-Type: application/json" \
  -d '{"id":"e2e-chain-source","type":"test","name":"E2E Chain Source","active":true,"region":{"offset":4600,"length":2},"inputs":[[1,0],[0,1]],"loop":false}' >/dev/null

curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}' >/dev/null
chain_step_2="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_merge_region "$chain_step_2" 4602 2

chain_step_3="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_merge_region "$chain_step_3" 4604 2

chain_step_4="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_merge_region "$chain_step_4" 4606 2

echo "RealityEngine_CPP chained stream e2e tests passed"
echo "RealityEngine_CPP service e2e tests passed"
