# API Equivalence

Reference APIs:

- `RealityEngine_AI/scala/src/main/scala/com/realityengine/api/Routes.scala`
- `RealityEngine_AI/scala/perception-engine/src/main/scala/com/realityengine/perception/api/PerceptionRoutes.scala`

## Reality Engine API

Base path: `/api`

| Scala/Akka endpoint | C++ status | Notes |
| --- | --- | --- |
| `GET /health` | Implemented | Same `status`, `timestamp`, `version` shape. |
| `GET /config` | Implemented | Static C++ config for now. |
| `PUT /config/dimension` | Documented gap | Native engine dimension is configured at process startup through `VECTOR_DIMENSION`. |
| `PUT /config/threshold` | Documented gap | Comparators carry thresholds per vector. |
| `POST /vectors`, `GET/DELETE /vectors/:id`, `POST /vectors/search` | Documented gap | VectorStore/Qdrant replacement not included in first port. |
| `POST/GET/DELETE /sequences...` | Documented gap | Sequence CRUD will be added after machine-level parity. |
| `POST /engine/process` | Implemented | Legacy sequence process shape with output list. |
| `POST /engine/reset` | Implemented | Resets machine sequence state. |
| `GET /engine/stats` | Implemented | Includes machine/vector totals. |
| `GET /engine/active` | Stub-compatible | Returns empty object until sequence CRUD parity lands. |
| `GET /engine/history` | Stub-compatible | Returns empty array; simulator history is implemented separately. |
| `POST /machines` | Implemented | Accepts existing machine JSON schema. |
| `GET /machines` | Implemented | Returns `machines` array. |
| `GET /machines/:id` | Implemented | Returns full machine details. |
| `PUT /machines/:id` | Implemented | Replaces machine from JSON. |
| `DELETE /machines/:id` | Implemented | Removes from engine and simulator. |
| `POST /machines/:id/process` | Implemented | Machine-local input vector processing. |
| `POST /machines/:id/process-universal` | Implemented | Universal-space extraction before machine processing. |
| `POST /machines/process-universal/all` | Implemented | Input-atomic all-machine processing. |
| `POST /machines/:id/whatif` | Implemented | Processes copy of machine. |
| `POST /machines/:id/whatif-universal` | Implemented | Universal-space what-if on machine copy. |
| `POST/GET/DELETE /machines/:id/checkpoints...` | Documented gap | Snapshot persistence not included in first port. |
| `GET /machines/json/list` | Implemented | Lists files in configured machine directory. |
| `GET /machines/json/:name` | Implemented | Loads existing machine JSON into service. |
| `POST /machines/json/import` | Planned | Equivalent to `POST /machines` in current C++ build. |
| `GET /machines/:id/export` | Planned | Domain serialization exists; export route not yet mounted. |
| `GET /machine-graph` | Implemented | Nodes and overlap edges. |
| `POST /perceptual-simulation/configure/chunk` | Implemented | Chunk buffer. |
| `POST /perceptual-simulation/configure/commit` | Implemented | Configures simulator from buffer. |
| `POST /perceptual-simulation/start/stop/step/reset` | Implemented | Manual stepping; no background scheduler in first port. |
| `GET /perceptual-simulation/state/history` | Implemented | Same top-level shapes. |
| `POST /preception/diagnostic` | Implemented | Reports nonzero universal values and machine mappings. |
| `POST /perceive` | Implemented | Main Perception Engine push target. |

### Startup Loading

The Scala service auto-discovers machine JSON files at startup. The C++ service
does the same through the `MACHINES_DIR` argument passed by `start.sh`, defaulting
to:

```text
../RealityEngine_AI/examples/machines
```

Startup is considered failed if `/api/machines` reports zero loaded machines
after the Reality Engine health endpoint becomes available.

## Perception Engine API

Base path: `/api`

| Scala/Akka endpoint | C++ status | Notes |
| --- | --- | --- |
| `GET /health` | Implemented | Same shape. |
| `GET /state` | Implemented | Sources, assembled vector, global step, auto config. |
| `GET /integrations/localai/status` | C++ extension | Reports configured local AI API health and localAIStack-compatible sensor registration. |
| `POST /integrations/localai/bootstrap` | C++ extension | Registers localAIStack-compatible sensors and imports bridge machines when available. |
| `POST /signals` | C++ extension | Generic external-signal write path over sensor sources; optionally triggers `/api/push`. |
| `POST /push` | Implemented | Posts to Reality Engine `/api/perceive`. |
| `POST /auto/start`, `POST /auto/stop` | State-compatible | Tracks auto state; background scheduler is planned. |
| `PATCH /config` | Implemented | Supports `gte` and `equals`. |
| `POST /reset` | Implemented | Resets sources and persistent vector. |
| `GET /sources`, `POST /sources` | Implemented | Test, simulated, and sensor source configs. |
| `PATCH /sources/:id` | Partial | Replaces source with patch body. Merge-patch parity is planned. |
| `DELETE /sources/:id` | Implemented | Removes source. |
| `POST /sensors/:sensorId` | Implemented | Updates matching sensor source. |
| `GET /machines` | Implemented | Proxies Reality Engine `/api/machines`. |
| `GET /ws` | Documented gap | WebSocket broadcast not included in standard-library HTTP layer. |

## Migration Notes

The C++ service is suitable for native-domain parity and endpoint smoke testing.
Production parity still needs:

- VectorStore/Qdrant-compatible persistence/search.
- WebSocket broadcast support.
- Background auto-push scheduler.
- Checkpoint persistence and export route.
- More complete JSON patch semantics for perception sources.

The local AI endpoints are C++ extensions around the existing sensor source
model. They preserve Scala/Node Perception Engine behavior while making
localAIStack and custom AI gateways easier to connect.

`POST /api/push` is single-flight in the C++ service. A second concurrent push
returns `409` with `error: "push already in progress"` so source advancement,
`globalStep`, and persistent-vector carry-forward cannot race.
