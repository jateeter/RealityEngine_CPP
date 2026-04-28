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
REALITY_ENGINE_PORT=3100
PERCEPTION_ENGINE_PORT=3101
VECTOR_DIMENSION=768
MACHINES_DIR=../RealityEngine_AI/examples/machines
QDRANT_URL=http://localhost:4333
QDRANT_STORAGE_DIR=../localAIStack/volumes/qdrant
LOCAL_AI_API_URL=http://localhost:4000
LOCAL_AI_MACHINES_DIR=../localAIStack/data/machines
LOCAL_AI_BOOTSTRAP=false
HTTP_WORKERS=
HTTP_QUEUE_CAPACITY=
```

Set `LOCAL_AI_BOOTSTRAP=true` when the C++ Perception Engine should register
localAIStack-compatible sensors and import bridge machines during startup.

`HTTP_WORKERS` defaults to the max of `2` and hardware concurrency.
`HTTP_QUEUE_CAPACITY` defaults to `HTTP_WORKERS * 64`. Both apply per native
service process and bound Beast request handling inside the process.
