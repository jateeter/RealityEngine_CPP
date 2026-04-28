# API Equivalence

The C++ APIs mirror the Scala/Akka route shapes used by `RealityEngine_AI`.

## Implemented Reality Engine Areas

- Health and config.
- Machine list, load, replace, delete, process.
- Universal input processing.
- What-if processing.
- Machine graph.
- Perceptual simulation configure, step, state, and history.
- `POST /api/perceive`.

## Implemented Perception Engine Areas

- Health and state.
- Push to Reality Engine.
- Single-flight push protection; concurrent pushes return `409`.
- Reset shares the push guard; reset during an active push returns `409`.
- Auto state start/stop flags.
- Match algorithm config.
- Reset.
- Source list/create/update/delete.
- Sensor value push.
- localAIStack-compatible status/bootstrap endpoints.
- Generic external signal ingestion through `POST /api/signals`.
- Machine proxy.

## Concurrency Notes

Reality Engine protects machine CRUD/import, reset, simulation, diagnostics,
and `/api/perceive` through a service read/write lock. Read-only registry/state
routes can share access, while stateful transitions and registry mutations take
exclusive access.

Perception Engine serializes sensor/source writes and uses a single-flight
push/reset guard. Push execution runs through a bounded worker queue with
capacity `1`; concurrent duplicate pushes return `409`, and queue saturation
returns `429`.

## Known Gaps

- VectorStore/Qdrant persistence and search.
- WebSocket broadcast.
- Background auto-push scheduler.
- Checkpoint persistence.
- Export route.
- Full merge-patch behavior for perception sources.

See `docs/API_EQUIVALENCE.md` in the repo for the detailed endpoint table.
