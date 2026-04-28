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
- Single-flight push protection; concurrent pushes return `409` with
  `coalesced: true`, then the worker performs at most one compact follow-up
  push.
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

Reality Engine separates machine-registry ownership from mutable simulator
ownership. Registry-only reads can run while `/api/perceive` owns simulator
state. Machine CRUD/import and reset lock both state owners.

Perception Engine serializes sensor/source writes and uses a single-flight
push/reset guard. Push execution runs through a bounded worker queue with
capacity `1`; concurrent duplicate pushes return `409` with `coalesced: true`,
and queue saturation returns `429`.

`POST /api/perceive` and `POST /api/push` support compact responses with
`compact: true` or `includeMachineResults: false`. Compact responses omit
`machineResults` while retaining the merged `perceptualSpace`.

## Known Gaps

- VectorStore/Qdrant persistence and search.
- WebSocket broadcast.
- Background auto-push scheduler.
- Checkpoint persistence.
- Export route.
- Full merge-patch behavior for perception sources.

See `docs/API_EQUIVALENCE.md` in the repo for the detailed endpoint table.
