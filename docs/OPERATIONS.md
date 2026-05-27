# Operations

This repo provides native operational scripts:

- `./start.sh`
- `./stop.sh`

They are intentionally aligned with `localAIStack` and the active runtime
deployment model. `RealityEngine_Scala` replaces the locked historical
`RealityEngine_AI` runtime for active reference work.

## Start

```bash
./start.sh
```

Startup performs these steps:

1. Builds C++ binaries with `make`.
2. Verifies the machine repository at `../RealityEngine_Machines/machines`.
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
11. Reports optional local AI API reachability at `LOCAL_AI_API_URL`.

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
| `REALITY_ENGINE_PORT` | `3299` |
| `PERCEPTION_ENGINE_PORT` | `3300` |
| `VECTOR_DIMENSION` | `768` |
| `MACHINES_DIR` | `../RealityEngine_Machines/machines` |
| `QDRANT_URL` | `http://localhost:4333` |
| `QDRANT_STORAGE_DIR` | `../localAIStack/volumes/qdrant` |
| `QDRANT_LOCALAI_COLLECTION` | `localai_docs` |
| `QDRANT_REALITY_COLLECTION` | `reality-vectors` |
| `LOCAL_AI_API_URL` | `http://localhost:4000` |
| `LOCAL_AI_MACHINES_DIR` | `../localAIStack/data/machines` |
| `LOCAL_AI_BOOTSTRAP` | `false` |
| `HTTP_WORKERS` | max of `2` and hardware concurrency |
| `HTTP_SESSION_TIMEOUT_MS` | `5000` |
| `HTTP_MAX_KEEPALIVE_REQUESTS` | `32` |
| `HTTP_IDEMPOTENCY_CACHE_SIZE` | `2048` |
| `HTTP_CLIENT_TIMEOUT_MS` | `5000` |
| `HTTP_CLIENT_POOL_SIZE` | `4` |
| `DOMAIN_WORKERS` | max of `2` and hardware concurrency |
| `DOMAIN_QUEUE_CAPACITY` | `DOMAIN_WORKERS * 256` |

Set `LOCAL_AI_BOOTSTRAP=true` to register the default localAIStack sensor
sources and import bridge machines into the C++ Reality Engine when the
Perception Engine starts.

`HTTP_WORKERS` applies to each native service process and controls the Asio
event-loop worker count. `HTTP_SESSION_TIMEOUT_MS` and
`HTTP_MAX_KEEPALIVE_REQUESTS` bound keep-alive sessions.
`HTTP_IDEMPOTENCY_CACHE_SIZE` controls the per-process cache of successful
idempotent `POST` responses keyed by the `Idempotency-Key` header.

`HTTP_CLIENT_TIMEOUT_MS` bounds outbound Beast client connect/read/write calls.
`HTTP_CLIENT_POOL_SIZE` controls the persistent connection pool used per
host:port for PE-to-RE and local AI HTTP calls.

`DOMAIN_WORKERS` and `DOMAIN_QUEUE_CAPACITY` tune the Reality Engine machine
transition worker pool. Runtime metrics are available from:

```http
GET /api/runtime/metrics
```

Runtime response and history controls are available without restart:

```http
GET /api/runtime/options
PATCH /api/runtime/options
```

The patch body can set `historyLimit`, `includeMachineResults`, and
`includePerceptualSpace`.

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
