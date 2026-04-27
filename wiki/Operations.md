# Operations

Use:

```bash
./start.sh
./stop.sh
```

## Startup Guarantees

`start.sh` verifies:

- Binaries build successfully.
- Machine directory exists.
- Shared Qdrant endpoint is reachable.
- Shared Qdrant storage directory exists or is reported.
- Expected Qdrant collections are present when Qdrant is reachable.
- Reality Engine health endpoint responds.
- Reality Engine has loaded at least one machine from
  `../RealityEngine_AI/examples/machines`.
- Perception Engine health endpoint responds.

## Shutdown

`stop.sh` stops Perception Engine first and Reality Engine second. It preserves
Qdrant and all shared persistent data.

## Configuration

Copy `.env.example` to `.env` and edit as needed.

Common overrides:

```bash
REALITY_ENGINE_PORT=3000
PERCEPTION_ENGINE_PORT=3001
MACHINES_DIR=../RealityEngine_AI/examples/machines
QDRANT_URL=http://localhost:4333
QDRANT_STORAGE_DIR=../localAIStack/volumes/qdrant
```

