# RealityEngine_CPP

C++ reimplementation of the Reality Engine and Perception Engine services from
`RealityEngine_AI`, using the Scala/Akka implementations as the current
behavioral model.

This repository is intentionally self-contained for the first native port:
it uses standard C++ plus POSIX sockets and does not require CMake or external
package downloads.

## What This Repo Provides

- Native C++ Reality Engine service on port `3100` by default.
- Native C++ Perception Engine service on port `3101` by default.
- Startup loading of all machine JSON files from
  `../RealityEngine_AI/examples/machines`.
- Shared Qdrant deployment model with `RealityEngine_AI` and `localAIStack`.
- E2E validation against authored `inputSequences` in the machine JSON corpus.

## Build

```bash
make
make test
make e2e
```

## Run

```bash
./start.sh
./stop.sh
```

`start.sh` builds the binaries, verifies the shared Qdrant instance, starts both
native services, and confirms that machines loaded successfully from
`../RealityEngine_AI/examples/machines` before declaring startup complete.

The Reality Engine service listens under `/api/...`, mirroring the Scala
RealityEngine API. The Perception Engine service listens under `/api/...` and
pushes assembled 256-dimensional vectors to `POST /api/perceive` on the Reality
Engine.

Operational defaults live in [.env.example](.env.example). Copy it to `.env`
to override ports or paths.

Qdrant is shared, not owned, by this repo. The scripts verify the unified
localAIStack Qdrant endpoint at `http://localhost:4333` and preserve the common
data repository at `../localAIStack/volumes/qdrant`, matching the
RealityEngine_AI and localAIStack deployment model.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [API Equivalence](docs/API_EQUIVALENCE.md)
- [Operations](docs/OPERATIONS.md)
- [E2E Testing](docs/E2E_TESTING.md)
- [GitHub Wiki Source](wiki/Home.md)

## Scope

Implemented in this first C++ repo:

- RealityVector, CriticalEventSequence, Machine, OutputArbiter.
- Universal 256-dimensional PreceptionEngine extraction and output merge.
- PerceptualSpaceSimulator snapshot -> process -> merge loop.
- PerceptionEngine source assembly for test, simulated, and sensor sources.
- Native HTTP endpoints for the high-traffic Scala/Akka route shapes.
- Machine JSON loading for the existing `examples/machines/*.json` format.

See [docs/API_EQUIVALENCE.md](docs/API_EQUIVALENCE.md) for endpoint status and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for mapping from Scala classes to
C++ classes.
