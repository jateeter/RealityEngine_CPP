# Architecture

`RealityEngine_CPP` follows the Scala/Akka implementation in
`RealityEngine_AI/scala` and keeps the same conceptual boundaries.

| Scala/Akka class | C++ class | Responsibility |
| --- | --- | --- |
| `RealityVector` | `reality::RealityVector` | Comparator matching, active state, next-vector and output-vector assertions. |
| `CriticalEventSequence` | `reality::CriticalEventSequence` | Deferred activation over active vectors; initial vectors remain armed. |
| `OutputArbiter` | `reality::OutputArbiter` | `AND`, `OR`, `PASSTHROUGH` output decisions. |
| `Machine` | `reality::Machine` | Runs all sequences, applies arbiter, returns `MachineTransitionResult`. |
| `PreceptionEngine` | `reality::PreceptionEngine` | Extracts machine input from the universal perceptual space and merges outputs. |
| `PerceptualSpaceSimulator` | `reality::PerceptualSpaceSimulator` | Snapshot -> parallel per-machine process -> deterministic merge loop for interconnected machines. |
| `perception.engine.PerceptionEngine` | `reality::PerceptionEngine` | Assembles persistent vectors from test/simulated/sensor sources. |

## Services

Two binaries are provided:

- `reality_engine_server`: equivalent to the Scala RealityEngine API service.
- `perception_engine_server`: equivalent to the Scala Perception Engine API service.

The operational scripts launch them in dependency order:

1. Reality Engine starts and loads machine JSON files.
2. `start.sh` verifies `/api/health`.
3. `start.sh` verifies `/api/machines` returns a nonzero machine count.
4. Perception Engine starts and points at the local Reality Engine URL.
5. If `LOCAL_AI_BOOTSTRAP=true`, Perception Engine registers localAIStack-style
   sensor sources and imports local AI bridge machines into the Reality Engine
   when they are available.

The HTTP layer uses Boost.Asio/Beast. `reality::http::Server` preserves the
small internal route API used by the services while delegating request parsing,
response writing, connection management, timers, and client calls to Beast.
Accepted server connections run as asynchronous Beast sessions on the shared
Asio worker pool. Keep-alive sessions have request-count and idle-time limits
so one idle client cannot pin resources indefinitely. The client path keeps a
small persistent HTTP/1.1 connection pool per host and port, so frequent
PE-to-RE calls avoid repeated TCP setup without serializing all outbound traffic
through one socket. Outbound connect/read/write operations are bounded by
`HTTP_CLIENT_TIMEOUT_MS`. Native POST retries use `Idempotency-Key` headers and
the server caches successful idempotent responses. The domain layer remains
independent of the transport implementation.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `include/reality/` | Public C++ headers for JSON, HTTP, and engine/domain types. |
| `src/http.cpp` | Boost.Asio/Beast HTTP server and client implementation behind the local route API. |
| `src/reality.cpp` | Core model, engine, simulator, perception source, and machine loader implementation. |
| `src/reality_engine_server.cpp` | Native Reality Engine API service. |
| `src/perception_engine_server.cpp` | Native Perception Engine API service. |
| `tests/reality_engine_tests.cpp` | Fast unit/smoke coverage for core behavior. |
| `tests/e2e_machine_sequences.cpp` | Corpus E2E runner for `RealityEngine_AI/examples/machines/*.json`. |
| `docs/` | Repo documentation. |
| `wiki/` | GitHub Wiki source pages for this repository. |

## Behavioral Notes

- GTE matching follows the Scala rule: reference values at or above threshold
  expect high input; values below threshold expect low input.
- Sequence activation is deferred until after all currently active vectors are
  evaluated, preventing same-cycle cascades.
- Perceptual simulation is input-atomic: all machine inputs are snapshotted
  before any output is merged back into shared space.
- Machine transitions fan out through a persistent bounded worker pool with a
  bounded queue. Each run reserves capacity for the whole machine batch before
  submitting work, so a transition cycle is admitted atomically instead of
  partially filling the queue. Critical event sequences inside one machine still
  transition serially; parallelism is only between machines, and output merging
  keeps deterministic machine order. Runtime metrics are exposed at
  `GET /api/runtime/metrics`.
- Output merge planning is built directly after transition futures complete, so
  small merge batches do not pay extra `std::async` scheduling cost. Shared
  perceptual-space writes remain serialized. Pending outputs are ordered by
  output region offset, region length, machine id, and output index. When
  regions overlap, later operations in that deterministic order win for the
  overlapping elements. The ordered merge plan is returned as `mergeBatch`.
- Reality Engine HTTP handlers protect shared domain state with a service-level
  read/write lock for the machine registry plus a simulator lock for mutable
  perceptual simulation state. `/api/perceive` owns the simulator lock but does
  not block registry-only read routes such as `GET /api/machines`. Machine CRUD
  and reset operations lock both the registry and simulator because they update
  both machine copies.
- Perception Engine sensor/source writes are mutexed. `/api/push` is
  single-flight, and `/api/reset` uses that same guard so reset cannot interleave
  with a push after the vector snapshot but before source advancement.
- Perception Engine runs PE-to-RE push execution through a bounded worker queue
  with capacity `1`. The HTTP push request waits for the queued job result.
  Duplicate concurrent push attempts return `409` with `coalesced: true`; the
  worker performs at most one compact follow-up push after the in-flight push
  completes. This preserves single-flight state advancement and prevents a noisy
  caller from keeping the worker in an unbounded coalesced-drain loop. Queue
  saturation is reported with `429`.
- `POST /api/perceive` and `POST /api/push` accept `compact: true` or
  `includeMachineResults: false` to omit per-machine transition details from
  the response. `POST /api/perceive` also accepts `includePerceptualSpace:
  false` for large-response projection. Runtime defaults and simulator history
  retention are controlled by `GET/PATCH /api/runtime/options`.
- Machine JSON uses the existing `RealityEngine_AI/examples/machines/*.json`
  schema.

## Shared Qdrant Model

`RealityEngine_CPP` follows the same ownership boundary as `RealityEngine_AI`:
Qdrant is owned and operated by `localAIStack`, exposed on host port `4333`, and
stored at `../localAIStack/volumes/qdrant`. This repo verifies Qdrant and its
expected collections during startup but does not start, stop, or delete Qdrant
data.

## Local AI Signal Model

External AI systems integrate through Perception Engine sensor sources. A
sensor source owns a region of the perceptual space and supplies values with TTL
expiry. `VECTOR_DIMENSION` is a compatibility floor for dense vector projection;
the intended model is a dynamically derived logical dimension based on active
machine and source reservations. See [Vector Management](VECTOR_MANAGEMENT.md).
The C++ service supports the localAIStack bridge sensors at
`[52:56]`, `[56:60]`, and `[64:68]`, plus generic ad hoc signal writes through
`POST /api/signals`.

The Reality Engine is not coupled to localAIStack, Ollama, LangGraph, or any
specific model provider. It only observes the assembled vector that the
Perception Engine pushes into `/api/perceive`.
