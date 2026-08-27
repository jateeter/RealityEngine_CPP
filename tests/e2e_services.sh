#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

REALITY_ENGINE_E2E_PORT="${REALITY_ENGINE_E2E_PORT:-5401}"
PERCEPTION_ENGINE_E2E_PORT="${PERCEPTION_ENGINE_E2E_PORT:-5400}"
OPENAI_STUB_E2E_PORT="${OPENAI_STUB_E2E_PORT:-5412}"
# Second PE instance, booted with no source-bootstrap flag, used by the
# declaration checks at the end of this script (RealityEngine_CPP#40).
DECLARATION_PE_E2E_PORT="${DECLARATION_PE_E2E_PORT:-5413}"
VECTOR_DIMENSION="${VECTOR_DIMENSION:-7680}"
MACHINES_DIR="${MACHINES_DIR:-../RealityEngine_Machines/machines}"
LOCAL_AI_API_URL="${LOCAL_AI_API_URL:-http://localhost:4000}"
LOCAL_AI_MACHINES_DIR="${LOCAL_AI_MACHINES_DIR:-../localAIStack/data/machines}"

REALITY_PID=""
PERCEPTION_PID=""
DECLARATION_PE_PID=""
OPENAI_STUB_PID=""

cleanup() {
  if [ -n "$DECLARATION_PE_PID" ] && kill -0 "$DECLARATION_PE_PID" >/dev/null 2>&1; then
    kill "$DECLARATION_PE_PID" >/dev/null 2>&1 || true
    wait "$DECLARATION_PE_PID" >/dev/null 2>&1 || true
  fi
  if [ -n "$OPENAI_STUB_PID" ] && kill -0 "$OPENAI_STUB_PID" >/dev/null 2>&1; then
    kill "$OPENAI_STUB_PID" >/dev/null 2>&1 || true
    wait "$OPENAI_STUB_PID" >/dev/null 2>&1 || true
  fi
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
for path in root.rglob("*.json"):
    data = json.loads(path.read_text())
    machine = data.get("machine", {})
    mapping = machine.get("perceptualMapping", {})
    seqs = machine.get("inputSequences", [])
    if not isinstance(seqs, list) or not seqs:
        seqs = machine.get("sequences", [])
    if (
        isinstance(mapping, dict)
        and isinstance(mapping.get("input"), dict)
        and isinstance(seqs, list)
        and any(isinstance(seq.get("vectors"), list) and seq.get("vectors") for seq in seqs)
    ):
        count += 1
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
    metadata = source.get("metadata")
    if not isinstance(metadata, dict):
        missing.append(f"{source.get('id')}:metadata")
    elif not isinstance(metadata.get("segments"), list) or not metadata.get("segments"):
        missing.append(f"{source.get('id')}:metadata.segments")
if missing:
    raise SystemExit("test sources missing full representation: " + ", ".join(missing[:20]))
' "$expected"
}

count_test_sources() {
  python3 -c '
import json, sys
data = json.load(sys.stdin)
print(len([s for s in data.get("sources", []) if s.get("type") == "test"]))
'
}

assert_test_source_count() {
  local payload="$1"
  local expected="$2"
  local context="$3"
  local actual
  actual="$(printf "%s" "$payload" | count_test_sources)"
  if [ "$actual" != "$expected" ]; then
    echo "${context}: expected ${expected} test source(s), got ${actual}" >&2
    exit 1
  fi
}

assert_completion_success() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("success") is not True:
    raise SystemExit(f"completion did not report success: {data!r}")
completion = data.get("completion", {})
if completion.get("provider") != "e2e" or completion.get("agent") != "e2e":
    raise SystemExit(f"unexpected completion metadata: {completion!r}")
if completion.get("sourceMappingId") != "agent-completion-risk":
    raise SystemExit(f"completion did not resolve configured source mapping: {completion!r}")
source = data.get("signal", {}).get("source", {})
if source.get("sensorId") != "agent.e2e.completion":
    raise SystemExit(f"completion did not commit to expected sensor source: {source!r}")
if source.get("lastValue") != [1, 0, 0.75, 0]:
    raise SystemExit(f"completion source lastValue mismatch: {source!r}")
' "$payload"
}

assert_healthkit_ingest_success() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("success") is not True:
    raise SystemExit(f"HealthKit ingest failed: {data!r}")
results = data.get("results", [])
if results:
    raise SystemExit(f"HealthKit returned legacy results shape: {data!r}")
resolved = data.get("resolved", [])
if len(resolved) != 1 or resolved[0].get("resolved") is not True:
    raise SystemExit(f"HealthKit result mismatch: {data!r}")
if resolved[0].get("sourceMappingId") != "healthkit-activity":
    raise SystemExit(f"HealthKit source mapping mismatch: {resolved[0]!r}")
if resolved[0].get("sensorId") != "healthkit.step-count":
    raise SystemExit(f"HealthKit sensor id mismatch: {resolved[0]!r}")
source = resolved[0].get("source", {})
if source.get("lastValue") != [1, 0, 0.9, 0]:
    raise SystemExit(f"HealthKit lastValue mismatch: {source!r}")
' "$payload"
}

assert_carekit_ingest_success() {
  python3 - "$1" <<'PY'
import json, sys
data = json.loads(sys.argv[1])
if not data.get("success"):
    raise SystemExit(f"CareKit ingest failed: {data!r}")
results = data.get("results", [])
if len(results) != 1:
    raise SystemExit(f"CareKit result mismatch: {data!r}")
if results[0].get("sourceMappingId") != "carekit-task":
    raise SystemExit(f"CareKit source mapping mismatch: {results[0]!r}")
if results[0].get("sensorId") != "carekit.task-adherence":
    raise SystemExit(f"CareKit sensor id mismatch: {results[0]!r}")
source = results[0].get("result", {}).get("source", {})
if source.get("lastValue") != [1, 0, 0.8, 0.95]:
    raise SystemExit(f"CareKit lastValue mismatch: {source!r}")
PY
}

assert_integration_registry_loaded() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("loaded") is not True:
    raise SystemExit(f"integration registry did not load: {data!r}")
ids = {m.get("id") for m in data.get("sourceMappings", [])}
if "agent-completion-risk" not in ids:
    raise SystemExit(f"expected agent-completion-risk source mapping: {data!r}")
' "$payload"
}

first_dispatch_record_id() {
  python3 -c '
import json, sys
data = json.load(sys.stdin)
records = data.get("records", [])
if not records:
    raise SystemExit(f"expected at least one dispatch record: {data!r}")
print(records[0]["id"])
'
}

assert_dispatch_update_success() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("success") is not True:
    raise SystemExit(f"dispatch update failed: {data!r}")
record = data.get("record", {})
if record.get("status") != "delivered":
    raise SystemExit(f"dispatch status was not updated: {record!r}")
if record.get("attempts") != 1:
    raise SystemExit(f"dispatch attempts was not incremented: {record!r}")
receipt = record.get("providerReceipt", {})
if receipt.get("adapter") != "e2e" or receipt.get("externalRunId") != "run-e2e":
    raise SystemExit(f"dispatch provider receipt mismatch: {receipt!r}")
' "$payload"
}

assert_openai_dispatch_success() {
  local payload="$1"
  python3 -c '
import json, sys
data = json.loads(sys.argv[1])
if data.get("success") is not True:
    raise SystemExit(f"OpenAI dispatch failed: {data!r}")
if data.get("provider") != "openai":
    raise SystemExit(f"unexpected provider: {data!r}")
receipt = data.get("receipt", {})
if receipt.get("status") != "sent" or receipt.get("adapter") != "openai":
    raise SystemExit(f"receipt mismatch: {receipt!r}")
fmt = data.get("response", {}).get("receivedTextFormat", {})
if fmt.get("type") != "json_schema" or fmt.get("strict") is not True:
    raise SystemExit(f"OpenAI payload was not schema-strict: {fmt!r}")
required = fmt.get("schema", {}).get("required", [])
if required != ["completed", "failed", "confidence", "actionClass"]:
    raise SystemExit(f"OpenAI schema required fields mismatch: {required!r}")
signal = data.get("completion", {}).get("signal", {}).get("source", {})
if signal.get("lastValue") != [1, 0, 0.83, 0]:
    raise SystemExit(f"OpenAI completion values mismatch: {signal!r}")
' "$payload"
}

assert_openai_dispatch_failure() {
  local payload="$1"
  python3 -c '
import json, sys
raw = sys.argv[1]
body, status = raw.rsplit("\n", 1)
data = json.loads(body)
if status != "502":
    raise SystemExit(f"expected HTTP 502 for invalid provider output, got {status}: {data!r}")
if data.get("success") is not False:
    raise SystemExit(f"expected failed dispatch body: {data!r}")
if "missing required JSON pointer: /actionClass" not in data.get("error", ""):
    raise SystemExit(f"unexpected validation error: {data!r}")
receipt = data.get("receipt", {})
if receipt.get("status") != "failed" or receipt.get("adapter") != "openai":
    raise SystemExit(f"failed receipt mismatch: {receipt!r}")
' "$payload"
}

post_machine() {
  local payload="$1"
  curl -sf -X POST "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/machines" \
    -H "Content-Type: application/json" \
    -d "$payload" >/dev/null
}

[ -x bin/reality_engine_server ] || { echo "bin/reality_engine_server missing; run make first" >&2; exit 1; }
[ -x bin/perception_engine_server ] || { echo "bin/perception_engine_server missing; run make first" >&2; exit 1; }
[ -x bin/reality_engine_cli ] || { echo "bin/reality_engine_cli missing; run make first" >&2; exit 1; }
[ -d "$MACHINES_DIR" ] || { echo "Machine directory not found: $MACHINES_DIR" >&2; exit 1; }

echo "E2E service boundary test"
echo "  Reality Engine port:    $REALITY_ENGINE_E2E_PORT"
echo "  Perception Engine port: $PERCEPTION_ENGINE_E2E_PORT"

bin/reality_engine_server "$REALITY_ENGINE_E2E_PORT" "$MACHINES_DIR" "$VECTOR_DIMENSION" >/tmp/reality_engine_e2e.log 2>&1 &
REALITY_PID="$!"
wait_for_http "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/health" "Reality Engine"

python3 tests/openai_stub_server.py "$OPENAI_STUB_E2E_PORT" >/tmp/openai_stub_e2e.log 2>&1 &
OPENAI_STUB_PID="$!"
wait_for_http "http://localhost:${OPENAI_STUB_E2E_PORT}/v1/models" "OpenAI stub"

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

# PE_SOURCE_BOOTSTRAP=auto registers the corpus test integration at boot, which
# is what makes the source assertion below meaningful: the set is declared by a
# registration event, not materialised by the read that follows it
# (RealityEngine_CPP#40). The declaration checks at the end of this script boot
# a second PE without the flag and assert the complementary case.
INTEGRATIONS_CONFIG="config/integrations.example.json" \
  TRIGGERS_ENABLED=true \
  PE_SOURCE_BOOTSTRAP=auto \
  OPENAI_BASE_URL="http://localhost:${OPENAI_STUB_E2E_PORT}/v1" \
  OPENAI_API_KEY="sk-e2e-stub" \
  bin/perception_engine_server "$PERCEPTION_ENGINE_E2E_PORT" "http://localhost:${REALITY_ENGINE_E2E_PORT}" "$LOCAL_AI_API_URL" "$LOCAL_AI_MACHINES_DIR" "$VECTOR_DIMENSION" >/tmp/perception_engine_e2e.log 2>&1 &
PERCEPTION_PID="$!"
wait_for_http "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/health" "Perception Engine"

expected_test_sources="$(expected_machine_test_source_count)"
startup_sources="$(curl -sf "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/sources")"
assert_machine_test_sources "$startup_sources" "$expected_test_sources"

PE_URL="http://localhost:${PERCEPTION_ENGINE_E2E_PORT}"
integration_status="$(bin/reality_engine_cli pe integrations-status --pe-url "$PE_URL")"
assert_integration_registry_loaded "$integration_status"
bin/reality_engine_cli pe ollama-status --pe-url "$PE_URL" >/dev/null
bin/reality_engine_cli pe openai-status --pe-url "$PE_URL" >/dev/null
bin/reality_engine_cli pe healthkit-status --pe-url "$PE_URL" >/dev/null
bin/reality_engine_cli pe carekit-status --pe-url "$PE_URL" >/dev/null

healthkit_payload="$(bin/reality_engine_cli pe healthkit-ingest --pe-url "$PE_URL" --sample-type step-count --source-mapping-id healthkit-activity --unit count --values 1,0,0.9,0)"
assert_healthkit_ingest_success "$healthkit_payload"
carekit_payload="$(bin/reality_engine_cli pe carekit-ingest --pe-url "$PE_URL" --sample-type task-adherence --source-mapping-id carekit-task --task-id morning-medication --care-plan-id care-plan-a --values 1,0,0.8,0.95)"
assert_carekit_ingest_success "$carekit_payload"

curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/sources" \
  -H "Content-Type: application/json" \
  -d '{"id":"e2e-dispatch-source","type":"test","name":"E2E Dispatch Source","active":true,"region":{"offset":492,"length":4},"inputs":[[0,1,0,1]],"loop":false}' >/dev/null

push_payload="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_push_success "$push_payload"
dispatch_id="$(bin/reality_engine_cli pe dispatch-ledger --pe-url "$PE_URL" | first_dispatch_record_id)"
bin/reality_engine_cli pe dispatch-read "$dispatch_id" --pe-url "$PE_URL" >/dev/null
dispatch_update="$(bin/reality_engine_cli pe dispatch-update "$dispatch_id" --pe-url "$PE_URL" --status delivered --adapter e2e --external-run-id run-e2e --increment-attempts)"
assert_dispatch_update_success "$dispatch_update"
openai_failure="$(curl -sS -X POST "${PE_URL}/api/integrations/openai/dispatch" -H "Content-Type: application/json" -d '{"id":"'"$dispatch_id"'","prompt":"MISSING_ACTION_CLASS","triggerPush":false}' -w '\n%{http_code}')"
assert_openai_dispatch_failure "$openai_failure"
openai_success="$(curl -sf -X POST "${PE_URL}/api/integrations/openai/dispatch" -H "Content-Type: application/json" -d '{"id":"'"$dispatch_id"'","triggerPush":false}')"
assert_openai_dispatch_success "$openai_success"

completion_downstream='{"version":"1.0.0","machine":{"name":"E2E Async Completion Consumer","description":"Consumes async agent completion source after dispatch record delivery","arbiterRule":"PASSTHROUGH","perceptualMapping":{"input":{"offset":4200,"length":4},"output":{"offset":4710,"length":2}},"sequences":[{"id":"e2e-async-completion-seq","name":"completion source drives downstream transition","vectors":[{"id":"e2e-async-completion-ready","elements":[{"value":1,"comparatorType":"equals"},{"value":0,"comparatorType":"equals"},{"value":0.75,"comparatorType":"equals"},{"value":0,"comparatorType":"equals"}],"isInitial":true,"outputVectors":[{"id":"e2e-async-completion-out","vector":[1,0],"metadata":{"boundary":"agent completion consumed"}}]}]}]}}'
post_machine "$completion_downstream"

completion_payload="$(bin/reality_engine_cli pe completion --pe-url "$PE_URL" --provider e2e --agent e2e --source-mapping-id agent-completion-risk --correlation-id e2e-correlation --values 1,0,0.75,0)"
assert_completion_success "$completion_payload"
completion_step="$(curl -sf -X POST "http://localhost:${PERCEPTION_ENGINE_E2E_PORT}/api/push" -H "Content-Type: application/json" -d '{"compact":true}')"
assert_merge_region "$completion_step" 4710 2
echo "RealityEngine_CPP async completion e2e tests passed"

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

# ── CORS header checks ────────────────────────────────────────────────────────
cors_origin="$(curl -si -H 'Origin: http://127.0.0.1:8088' \
  "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/engine/stats" \
  | tr -d '\r' | grep -i '^access-control-allow-origin:' | head -1)"
if [[ -z "$cors_origin" ]]; then
  echo "CORS: GET /api/engine/stats missing Access-Control-Allow-Origin" >&2; exit 1
fi

options_status="$(curl -si -X OPTIONS \
  -H 'Origin: http://127.0.0.1:8088' \
  -H 'Access-Control-Request-Method: GET' \
  -H 'Access-Control-Request-Headers: accept' \
  "http://localhost:${REALITY_ENGINE_E2E_PORT}/api/engine/stats" \
  | tr -d '\r' | head -1)"
if [[ "$options_status" != *" 204 "* && "$options_status" != *" 200 "* ]]; then
  echo "CORS: OPTIONS /api/engine/stats returned unexpected status: ${options_status}" >&2; exit 1
fi
echo "RealityEngine_CPP CORS e2e tests passed"

# ── source declaration & reset validation ─────────────────────────────────────
#
# RealityEngine_CPP#40 and #41, against the settled contract in
# RealityEngine_CI#163. A second PE is booted with no PE_SOURCE_BOOTSTRAP, i.e.
# with the corpus test integration unregistered, so the declared set starts
# empty. It runs last and on its own port because it pushes and resets, and
# neither should be able to disturb the assertions above.
DECL_PE_URL="http://localhost:${DECLARATION_PE_E2E_PORT}"
bin/perception_engine_server "$DECLARATION_PE_E2E_PORT" "http://localhost:${REALITY_ENGINE_E2E_PORT}" "$LOCAL_AI_API_URL" "$LOCAL_AI_MACHINES_DIR" "$VECTOR_DIMENSION" >/tmp/perception_engine_declaration_e2e.log 2>&1 &
DECLARATION_PE_PID="$!"
wait_for_http "${DECL_PE_URL}/api/health" "Perception Engine (declaration)"

# Nothing registered, so nothing declared. This used to report one test source
# per RE machine, because the read itself materialised them.
assert_test_source_count "$(curl -sf "${DECL_PE_URL}/api/sources")" 0 "boot with bootstrap off"

# Reads report state; they do not declare. Repeating them must not move the
# count off zero — the four-run drift in #40 (1, 809, 639, 544 sources needing
# activation across identical operations) was read timing, not state.
curl -sf "${DECL_PE_URL}/api/state" >/dev/null
curl -sf "${DECL_PE_URL}/api/machines" >/dev/null
curl -sf "${DECL_PE_URL}/api/sources" >/dev/null
assert_test_source_count "$(curl -sf "${DECL_PE_URL}/api/sources")" 0 "after reads"

# Nor does a push. The corpus sync used to run on every push, which is what
# #35's definition-change guard was added to contain; the guard stays, but the
# push path no longer touches membership at all.
curl -sf -X POST "${DECL_PE_URL}/api/push" -H "Content-Type: application/json" -d '{"compact":true}' >/dev/null
assert_test_source_count "$(curl -sf "${DECL_PE_URL}/api/sources")" 0 "after push"

# Registering the integration dynamically declares the set, immediately and
# completely.
curl -sf -X POST "${DECL_PE_URL}/api/sources/bootstrap-from-machines" >/dev/null
declared_sources="$(curl -sf "${DECL_PE_URL}/api/sources")"
assert_machine_test_sources "$declared_sources" "$expected_test_sources"
echo "RealityEngine_CPP source declaration e2e tests passed"

echo "RealityEngine_CPP service e2e tests passed"
