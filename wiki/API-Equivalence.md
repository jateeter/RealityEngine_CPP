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
- Auto state start/stop flags.
- Match algorithm config.
- Reset.
- Source list/create/update/delete.
- Sensor value push.
- localAIStack-compatible status/bootstrap endpoints.
- Generic external signal ingestion through `POST /api/signals`.
- Machine proxy.

## Known Gaps

- VectorStore/Qdrant persistence and search.
- WebSocket broadcast.
- Background auto-push scheduler.
- Checkpoint persistence.
- Export route.
- Full merge-patch behavior for perception sources.

See `docs/API_EQUIVALENCE.md` in the repo for the detailed endpoint table.
