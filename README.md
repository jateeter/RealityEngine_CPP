# RealityEngine_CPP

C++ reimplementation of the Reality Engine and Perception Engine services from
`RealityEngine_AI`, using the Scala/Akka implementations as the current
behavioral model.

The native services use Boost.Asio/Beast for HTTP transport while keeping the
domain layer independent of the server implementation.

## What This Repo Provides

- Native C++ Reality Engine service on port `3100` by default.
- Native C++ Perception Engine service on port `3101` by default.
- Startup loading of all machine JSON files from
  `../RealityEngine_AI/examples/machines`.
- Shared Qdrant deployment model with `RealityEngine_AI` and `localAIStack`.
- E2E validation against authored `inputSequences` in the machine JSON corpus.

## Build

```bash
brew install boost   # macOS, if Boost is not already installed
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
pushes assembled reality vectors to `POST /api/perceive` on the Reality Engine.
Deployment defaults to `VECTOR_DIMENSION=768` to match the current
RealityEngine_AI/localAIStack layout; domain classes still support smaller
test dimensions.

Operational defaults live in [.env.example](.env.example). Copy it to `.env`
to override ports or paths.

Qdrant is shared, not owned, by this repo. The scripts verify the unified
localAIStack Qdrant endpoint at `http://localhost:4333` and preserve the common
data repository at `../localAIStack/volumes/qdrant`, matching the
RealityEngine_AI and localAIStack deployment model.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [API Equivalence](docs/API_EQUIVALENCE.md)
- [Local AI Integration](docs/LOCAL_AI_INTEGRATION.md)
- [Operations](docs/OPERATIONS.md)
- [E2E Testing](docs/E2E_TESTING.md)
- [GitHub Wiki Source](wiki/Home.md)

## Scope

Implemented in this first C++ repo:

- RealityVector, CriticalEventSequence, Machine, OutputArbiter.
- Configurable universal PreceptionEngine extraction and output merge.
- PerceptualSpaceSimulator snapshot -> parallel process -> deterministic merge loop.
- PerceptionEngine source assembly for test, simulated, and sensor sources.
- Boost.Asio/Beast HTTP endpoints with bounded request workers and persistent
  PE-to-RE client connections.
- Compact `/api/perceive` and `/api/push` response mode for high-throughput
  integrations that do not need per-machine transition details.
- Machine JSON loading for the existing `examples/machines/*.json` format.

See [docs/API_EQUIVALENCE.md](docs/API_EQUIVALENCE.md) for endpoint status and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for mapping from Scala classes to
C++ classes.
