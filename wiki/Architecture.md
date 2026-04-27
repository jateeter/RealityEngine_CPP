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
| `PerceptualSpaceSimulator` | Runs snapshot, process, merge simulation phases. |
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
4. Machines process their local vectors.
5. Outputs are merged back into perceptual space.
6. Perception Engine can carry the merged perceptual space forward.
