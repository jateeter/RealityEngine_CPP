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

## Data Flow

1. Perception Engine assembles a vector using the configured deployment dimension.
2. Perception Engine posts it to Reality Engine `POST /api/perceive`.
3. Reality Engine snapshots mapped machine inputs.
4. Machines process their local vectors across a bounded worker fan-out.
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

Output merge planning is scheduled with futures, but writes into the shared
perceptual space are serialized. Pending outputs are ordered by output region
offset, region length, machine id, and output index. If output regions overlap,
the later operation in that deterministic order wins for the overlapping
elements.
