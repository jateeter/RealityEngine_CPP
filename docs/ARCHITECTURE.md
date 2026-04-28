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
response writing, connection management, and client calls to Beast. Accepted
server connections are handled by a bounded worker pool instead of a detached
thread per connection. The client path keeps persistent HTTP/1.1 connections per
host and port, so frequent PE-to-RE calls avoid repeated TCP setup. The domain
layer remains independent of the transport implementation.

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
- Machine transitions fan out through a persistent bounded worker pool. Critical
  event sequences inside one machine still transition serially; parallelism is
  only between machines, and output merging keeps deterministic machine order.
- Output merge planning is built directly after transition futures complete, so
  small merge batches do not pay extra `std::async` scheduling cost. Shared
  perceptual-space writes remain serialized. Pending outputs are ordered by
  output region offset, region length, machine id, and output index. When
  regions overlap, later operations in that deterministic order win for the
  overlapping elements.
- Reality Engine HTTP handlers protect shared domain state with a service-level
  read/write lock. Read-only registry/state routes use shared ownership, while
  machine CRUD, JSON imports, resets, stateful machine processing, simulation
  operations, and `/api/perceive` use exclusive ownership over `machines`,
  `simulator`, and `preception` state.
- Perception Engine sensor/source writes are mutexed. `/api/push` is
  single-flight, and `/api/reset` uses that same guard so reset cannot interleave
  with a push after the vector snapshot but before source advancement.
- Perception Engine runs PE-to-RE push execution through a bounded worker queue
  with capacity `1`. The HTTP push request waits for the queued job result.
  Duplicate concurrent push attempts return `409` with `coalesced: true`; the
  worker performs one compact follow-up push after the in-flight push completes,
  preserving single-flight state advancement while avoiding lost signal updates.
  Queue saturation is reported with `429`.
- `POST /api/perceive` and `POST /api/push` accept `compact: true` or
  `includeMachineResults: false` to omit per-machine transition details from
  the response. The full merged perceptual space remains present so PE state can
  still carry Reality Engine output forward.
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
sensor source owns a region of the configured perceptual space and supplies
values with TTL expiry. Deployment defaults to `VECTOR_DIMENSION=768`, matching
the current `RealityEngine_AI` stack. The C++ service supports the localAIStack bridge sensors at
`[52:56]`, `[56:60]`, and `[64:68]`, plus generic ad hoc signal writes through
`POST /api/signals`.

The Reality Engine is not coupled to localAIStack, Ollama, LangGraph, or any
specific model provider. It only observes the assembled vector that the
Perception Engine pushes into `/api/perceive`.
