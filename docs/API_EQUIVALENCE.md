# API Equivalence

Reference APIs:

- `RealityEngine_AI/scala/src/main/scala/com/realityengine/api/Routes.scala`
- `RealityEngine_AI/scala/perception-engine/src/main/scala/com/realityengine/perception/api/PerceptionRoutes.scala`

## Reality Engine API

Base path: `/api`

OpenAPI: [`docs/openapi/reality-engine.yaml`](openapi/reality-engine.yaml)

| Scala/Akka endpoint | C++ status | Notes |
| --- | --- | --- |
| `GET /health` | Implemented | Same `status`, `timestamp`, `version` shape. |
| `GET /config` | Implemented | Static C++ config for now. |
| `PUT /config/dimension` | Documented gap | Native engine dimension is configured at process startup through `VECTOR_DIMENSION`. |
| `PUT /config/threshold` | Documented gap | Comparators carry thresholds per vector. |
| `POST /vectors`, `GET/DELETE /vectors/:id`, `POST /vectors/search` | Shape-compatible | Uses an in-memory vector store. Search honors `limit` and `threshold`; Qdrant persistence is still outside the native service. |
| `POST/GET/DELETE /sequences...` | Documented gap | Sequence CRUD will be added after machine-level parity. |
| `POST /engine/process` | Implemented | Legacy sequence process shape with output list. |
| `POST /engine/reset` | Implemented | Resets machine sequence state. |
| `GET /engine/stats` | Implemented | Includes machine/vector totals. |
| `GET /engine/active` | Stub-compatible | Returns empty object until sequence CRUD parity lands. |
| `GET /engine/history` | Stub-compatible | Returns empty array; simulator history is implemented separately. |
| `POST /machines` | Implemented | Accepts existing machine JSON schema. |
| `GET /machines` | Implemented | Returns `machines` array. |
| `GET /machines/:id` | Implemented | Returns full machine details. |
| `PUT /machines/:id` | Implemented | Replaces machine from JSON. |
| `DELETE /machines/:id` | Implemented | Removes from engine and simulator. |
| `POST /machines/:id/process` | Implemented | Machine-local input vector processing. |
| `POST /machines/:id/process-universal` | Implemented | Universal-space extraction before machine processing. |
| `POST /machines/process-universal/all` | Implemented | Input-atomic all-machine processing. |
| `POST /machines/:id/whatif` | Implemented | Processes copy of machine. |
| `POST /machines/:id/whatif-universal` | Implemented | Universal-space what-if on machine copy. |
| `POST/GET/DELETE /machines/:id/checkpoints...` | Documented gap | Snapshot persistence not included in first port. |
| `GET /machines/json/list` | Implemented | Lists files in configured machine directory. |
| `GET /machines/json/:name` | Implemented | Loads existing machine JSON into service. |
| `POST /machines/json/import` | Planned | Equivalent to `POST /machines` in current C++ build. |
| `GET /machines/:id/export` | Planned | Domain serialization exists; export route not yet mounted. |
| `GET /demo/multi-step`, `GET /demo/data-center`, `GET /demo/kleene-star` | Implemented | Legacy TypeScript-compatible demo envelopes for loaded example machines. |
| `GET /machine-graph` | Implemented | Nodes and overlap edges. |
| `POST /perceptual-simulation/configure/chunk` | Implemented | Chunk buffer. |
| `POST /perceptual-simulation/configure/commit` | Implemented | Configures simulator from buffer. |
| `POST /perceptual-simulation/start/stop/step/reset` | Implemented | Manual stepping; no background scheduler in first port. |
| `GET /perceptual-simulation/state/history` | Implemented | Same top-level shapes. |
| `POST /preception/diagnostic` | Implemented | Reports nonzero universal values and machine mappings. |
| `POST /perceive` | Implemented | Main Perception Engine push target. Accepts both `matchAlgorithmOverride` and legacy PE `matchAlgorithm`. Supports response projection with `compact`, `includeMachineResults`, and `includePerceptualSpace`. Returns observable `mergeBatch`. |
| `GET/PATCH /runtime/options` | C++ extension | Dynamic history and response projection controls. |

### Startup Loading

The Scala service auto-discovers machine JSON files at startup. The C++ service
does the same through the `MACHINES_DIR` argument passed by `start.sh`, defaulting
to:

```text
../RealityEngine_AI/examples/machines
```

Startup is considered failed if `/api/machines` reports zero loaded machines
after the Reality Engine health endpoint becomes available.

## Perception Engine API

Base path: `/api`

OpenAPI: [`docs/openapi/perception-engine.yaml`](openapi/perception-engine.yaml)

| Scala/Akka endpoint | C++ status | Notes |
| --- | --- | --- |
| `GET /health` | Implemented | Same shape. |
| `GET /state` | Implemented | Sources, assembled vector, global step, auto config. |
| `GET /integrations/localai/status` | C++ extension | Reports configured local AI API health and localAIStack-compatible sensor registration. |
| `GET /integrations/localai/catalog` | C++ extension | Dynamically discovers localAIStack health, graph schema, recent GraphQL trigger events, and allowed invocation endpoints. |
| `POST /integrations/localai/bootstrap` | C++ extension | Registers localAIStack-compatible sensors and imports bridge machines when available. |
| `POST /integrations/localai/invoke` | C++ extension | Guarded dynamic access to allowed localAIStack `GET`/`POST` endpoints such as `/graph/schema`, `/graph/rag`, `/graph/agent`, `/rag/query`, `/chat`, and `/graphql`. |
| `POST /signals` | C++ extension | Generic external-signal write path over sensor sources; optionally triggers `/api/push`. `compactPush: true` requests compact inline push results. |
| `POST /push` | Implemented | Posts to Reality Engine `/api/perceive` with both `matchAlgorithm` and `matchAlgorithmOverride` for cross-version compatibility. Supports sync compatibility and async job mode with `async: true`. |
| `GET /push/:id` | C++ extension | Polls async push job status and result. |
| `POST /auto/start`, `POST /auto/stop` | State-compatible | Tracks auto state; background scheduler is planned. |
| `PATCH /config` | Implemented | Supports `gte` and `equals`. |
| `POST /reset` | Implemented | Resets sources and persistent vector. |
| `GET /sources`, `POST /sources` | Implemented | Test, simulated, and sensor source configs. |
| `PATCH /sources/:id` | Partial | Replaces source with patch body. Merge-patch parity is planned. |
| `DELETE /sources/:id` | Implemented | Removes source. |
| `POST /sensors/:sensorId` | Implemented | Updates matching sensor source. |
| `GET /machines` | Implemented | Proxies Reality Engine `/api/machines`. |
| `GET /ws` | Documented gap | WebSocket broadcast not included in the current Beast HTTP transport. |

## Migration Notes

The C++ service is suitable for native-domain parity and endpoint smoke testing.
Production parity still needs:

- Qdrant-backed vector persistence/search.
- WebSocket broadcast support.
- Background auto-push scheduler.
- Checkpoint persistence and export route.
- More complete JSON patch semantics for perception sources.

The local AI endpoints are C++ extensions around the existing sensor source
model. They preserve Scala/Node Perception Engine behavior while making
localAIStack and custom AI gateways easier to connect.

When the Perception Engine observes machines loaded in the Reality Engine, it
creates one inactive `test` source for each machine `inputSequences` entry. The
source id is deterministic, the source region is the machine input mapping, and
the source inputs are copied from the test-sequence vectors. Each source also
retains the sequence metadata and the full authored sequence object, including
the original vector list and expected-output metadata. These sources are
inactive by default so the authored test corpus is available through
`GET /api/sources` without driving all machine test scenarios during normal
pushes.

`POST /api/push` is single-flight in the C++ service. A second concurrent push
returns `409` with `error: "push already in progress"` and `coalesced: true` so
source advancement, `globalStep`, and persistent-vector carry-forward cannot
race. The worker performs one compact follow-up push after the current push
finishes, which captures any signal updates that arrived during the in-flight
operation without advancing the same source cursors concurrently. The follow-up
is intentionally capped at one compact push per active push cycle so sustained
duplicate callers cannot starve reset or the next normal push request.

Push execution is dispatched through a bounded PE worker queue with capacity
`1`. The route waits for the queued job result to preserve the synchronous API
shape, while the external PE-to-RE HTTP call is isolated from the request
handler path. Queue saturation returns `429` with `error: "push queue is full"`.

`POST /api/perceive` accepts either `matchAlgorithmOverride` or the legacy
Perception Engine field `matchAlgorithm`; when both are present,
`matchAlgorithmOverride` wins. `POST /api/push` sends both fields so it remains
compatible with C++ and RealityEngine_AI receivers.

`POST /api/perceive` and `POST /api/push` accept either:

```json
{ "compact": true }
```

or:

```json
{ "includeMachineResults": false }
```

Compact responses omit `machineResults` while retaining `stepNumber`,
`timestamp`, `perceptualSpace`, and `activeRegions`.

`POST /api/perceive` also accepts:

```json
{ "includePerceptualSpace": false }
```

to omit the merged perceptual-space vector from large responses. The response
still includes `mergeBatch`, an ordered list of machine output writes with the
target region, machine id, and output index. Merge application follows the
machine processing order, matching the RealityEngine_AI simulator semantics
more closely for overlapping output regions.

Runtime defaults for response projection and bounded history can be inspected
and changed through:

```http
GET /api/runtime/options
PATCH /api/runtime/options
```

Example patch:

```json
{
  "historyLimit": 128,
  "includeMachineResults": false,
  "includePerceptualSpace": true
}
```

`POST /api/push` remains synchronous by default. Supplying `async: true` returns
`202` with a `jobId` and `statusEndpoint`; callers poll `GET /api/push/:id` for
`queued`, `running`, `completed`, or `failed`.

Outbound `POST` calls made by the native HTTP client include idempotency keys
and retry once after reconnect. The server caches successful idempotent `POST`
responses, so retry after a transient connection failure does not repeat the
completed operation.

`POST /api/reset` in the Perception Engine shares the same single-flight guard.
If a push is already in progress, reset returns `409` rather than clearing state
while the push is waiting on Reality Engine.

Reality Engine machine CRUD/import, reset, simulation, diagnostics, and
`POST /api/perceive` use explicit state ownership. Registry-only reads can run
while `/api/perceive` owns simulator state. Machine CRUD/import and reset lock
both registry and simulator state, keeping LocalAI bootstrap imports and direct
machine CRUD from mutating simulator machines while a perception transition is
running.
