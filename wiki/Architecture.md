# Architecture

`RealityEngine_CPP` is a native C++ port of the Reality Engine domain model and
service layer.

## Core Components

| Component | Purpose |
| --- | --- |
| `RealityVector` | Matches input vectors and asserts next vectors or output vectors. |
| `CriticalEventSequence` | Runs active vectors and applies deferred activation. |
| `Machine` | Groups sequences and applies an output arbiter. |
| `OutputArbiter` | Implements `AND`, `OR`, and `PASSTHROUGH` output behavior. |
| `PreceptionEngine` | Extracts machine-local input from the universal space. |
| `PerceptualSpaceSimulator` | Runs snapshot, parallel per-machine process, merge simulation phases. |
| `PerceptionEngine` | Assembles persistent vectors from sources. |

## Binaries

| Binary | Purpose |
| --- | --- |
| `bin/reality_engine_server` | Reality Engine HTTP API. |
| `bin/perception_engine_server` | Perception Engine HTTP API and push client. |
| `bin/reality_engine_tests` | Core smoke/unit tests. |
| `bin/e2e_machine_sequences` | Machine corpus E2E runner. |

## HTTP Transport

The service HTTP layer uses Boost.Asio/Beast. `reality::http::Server` keeps the
small internal route API used by the services while Beast handles request
parsing, response writing, connection management, and PE-to-RE/localAI client
calls. Accepted sockets are processed by a bounded worker pool instead of
detached per-connection threads. Keep-alive sessions have request-count and
idle-time limits. Outbound client calls reuse a persistent HTTP/1.1 connection
pool per host and port, with bounded connect/read/write timeouts.

## Data Flow

1. Perception Engine assembles a vector using the configured deployment dimension.
2. Perception Engine posts it to Reality Engine `POST /api/perceive`.
3. Reality Engine snapshots mapped machine inputs.
4. Machines process their local vectors through a persistent bounded worker pool
   with a bounded queue.
5. Outputs are merged back into perceptual space in deterministic machine order.
6. Perception Engine can carry the merged perceptual space forward.

## Machine Parallelism

`PerceptualSpaceSimulator::run_phases()` snapshots all machine inputs before
starting any transition work. Each machine then runs independently against its
snapshot, while critical event sequences inside that machine remain serialized
through `Machine::process_input()`. Shared perceptual-space writes happen only
after all machine transitions finish, preserving input atomicity and avoiding
cross-machine clock coupling.

## Output Merge Policy

Machine transition work is scheduled with futures. Merge planning is built
directly after transition futures complete, and writes into the shared
perceptual space are serialized. Pending outputs are ordered by output region
offset, region length, machine id, and output index. If output regions overlap,
the later operation in that deterministic order wins for the overlapping
elements.

## Service Concurrency

Reality Engine HTTP handlers protect the machine registry with a read/write
lock and mutable simulation state with a simulator lock. `/api/perceive` owns
the simulator lock but does not block registry-only reads such as
`GET /api/machines`. Machine CRUD/import and reset lock both state owners.

Perception Engine sensor/source writes are mutexed. `/api/push` is
single-flight, and `/api/reset` uses that same guard so reset cannot interleave
with a push after the vector snapshot but before source advancement.

PE-to-RE push execution runs through a bounded worker queue with capacity `1`.
The HTTP push request waits for the queued job result to preserve API shape,
while duplicate concurrent pushes return `409` with `coalesced: true`. The
worker performs one compact follow-up push after the in-flight push completes,
which preserves single-flight source advancement. The follow-up is capped at one
compact push per active push cycle so duplicate callers cannot keep the worker
in an unbounded drain loop. Queue saturation returns `429`.

`POST /api/perceive` and `POST /api/push` accept `compact: true` or
`includeMachineResults: false` to omit per-machine transition details from the
response while retaining the merged perceptual space.
