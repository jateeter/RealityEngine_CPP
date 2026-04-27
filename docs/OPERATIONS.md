# Operations

This repo provides native operational scripts:

- `./start.sh`
- `./stop.sh`

They are intentionally aligned with the deployment model used by
`RealityEngine_AI` and `localAIStack`.

## Start

```bash
./start.sh
```

Startup performs these steps:

1. Builds C++ binaries with `make`.
2. Verifies the machine repository at `../RealityEngine_AI/examples/machines`.
3. Verifies the unified Qdrant endpoint at `http://localhost:4333`.
4. Verifies the shared Qdrant data repository path
   `../localAIStack/volumes/qdrant`.
5. Checks for expected shared collections:
   - `localai_docs`
   - `reality-vectors`
6. Starts Reality Engine.
7. Waits for `GET /api/health`.
8. Verifies nonzero machine loading through `GET /api/machines`.
9. Starts Perception Engine.
10. Waits for `GET /api/health`.

## Stop

```bash
./stop.sh
```

Shutdown order:

1. Perception Engine.
2. Reality Engine.

The script sends `SIGTERM`, waits up to 15 seconds, then sends `SIGKILL` only if
the process has not exited.

## Environment

Copy `.env.example` to `.env` to override defaults:

```bash
cp .env.example .env
```

Important variables:

| Variable | Default |
| --- | --- |
| `REALITY_ENGINE_PORT` | `3100` |
| `PERCEPTION_ENGINE_PORT` | `3101` |
| `MACHINES_DIR` | `../RealityEngine_AI/examples/machines` |
| `QDRANT_URL` | `http://localhost:4333` |
| `QDRANT_STORAGE_DIR` | `../localAIStack/volumes/qdrant` |
| `QDRANT_LOCALAI_COLLECTION` | `localai_docs` |
| `QDRANT_REALITY_COLLECTION` | `reality-vectors` |

## Qdrant Ownership

Qdrant is not managed by `RealityEngine_CPP`.

Start or stop the shared Qdrant instance through localAIStack:

```bash
cd ../localAIStack
./scripts/start.sh
./scripts/stop.sh
```

`RealityEngine_CPP` only verifies that the shared endpoint and data repository
are available.

## Logs and Runtime Files

Runtime files are ignored by git:

| Path | Purpose |
| --- | --- |
| `logs/reality_engine.log` | Reality Engine service log. |
| `logs/perception_engine.log` | Perception Engine service log. |
| `run/reality_engine.pid` | Reality Engine PID file. |
| `run/perception_engine.pid` | Perception Engine PID file. |
