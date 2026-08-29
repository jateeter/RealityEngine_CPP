# Shared Qdrant

`RealityEngine_CPP` uses the same Qdrant deployment model as
`RealityEngine_Machines`.

Qdrant is owned by `localAIStack`.

## Defaults

| Setting | Value |
| --- | --- |
| REST URL | `http://localhost:4333` |
| gRPC URL | `http://localhost:4334` |
| Storage directory | `../localAIStack/volumes/qdrant` |
| localAIStack collection | `localai_docs` |
| Reality Engine collection | `reality-vectors` |

## Ownership Boundary

`RealityEngine_CPP` verifies Qdrant during startup but does not start, stop, or
delete Qdrant data.

Manage Qdrant through localAIStack:

```bash
cd ../localAIStack
./scripts/start.sh
./scripts/stop.sh
```

