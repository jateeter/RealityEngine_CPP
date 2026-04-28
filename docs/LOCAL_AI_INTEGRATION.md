# Local AI Integration

`RealityEngine_CPP` integrates local AI systems through the Perception Engine.
The Reality Engine remains provider-neutral: it receives only assembled reality
vectors through `POST /api/perceive`.

## Design

The C++ Perception Engine follows the `RealityEngine_AI` localAIStack bridge:

1. External AI systems register or update named sensor sources.
2. Sensor values are written into configured perceptual-space regions.
3. `POST /api/push` assembles the persistent vector and forwards it to the C++
   Reality Engine.
4. The Reality Engine processes all machines and returns the post-merge
   perceptual space.
5. The Perception Engine carries that post-merge vector forward as the base for
   the next push.

This keeps AI integration flexible. A caller can use localAIStack-compatible
sensors, a custom API gateway, or direct signal writes without changing machine
logic.

## localAIStack Compatibility

The default compatibility profile mirrors
`localAIStack/services/api/core/reality_bridge.py`.

| Sensor | Region | Meaning |
| --- | --- | --- |
| `localai_rag_retrieval` | `[52:56]` | Retrieval document count and average score. |
| `localai_rag_grading` | `[56:60]` | Kept-document ratio and rewrite count. |
| `localai_agent_activity` | `[64:68]` | Tool calls, tool errors, and reasoning depth. |

Bootstrap imports these bridge machines when present in
`../localAIStack/data/machines`:

- `rag_corrective_cycle.json`
- `session_rag_context.json`
- `session_agent_context.json`
- `ai_load_bridge.json`
- `agent_activity_classifier.json`

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `VECTOR_DIMENSION` | `768` | Shared perceptual-space size for the C++ RE and PE deployment. |
| `LOCAL_AI_API_URL` | `http://localhost:4000` | Health/status endpoint for the external AI API. |
| `LOCAL_AI_MACHINES_DIR` | `../localAIStack/data/machines` | Optional bridge-machine import directory. |
| `LOCAL_AI_BOOTSTRAP` | `false` | When true, registers sensors and imports bridge machines on Perception Engine startup. |

## API

### Status

```http
GET /api/integrations/localai/status
```

Returns local AI health, configured URL, sensor registration status, and machine
directory.

### Bootstrap

```http
POST /api/integrations/localai/bootstrap
```

Registers the default localAIStack sensor sources if missing and imports the
default bridge machines into the C++ Reality Engine if their names are not
already loaded.

### Generic External Signal

```http
POST /api/signals
```

Existing sensor update:

```json
{
  "sensorId": "localai_rag_retrieval",
  "values": [0.4, 0.82, 0.0, 0.0],
  "triggerPush": true
}
```

Ad hoc sensor registration and update:

```json
{
  "sensorId": "custom_agent_signal",
  "name": "custom/agent_signal",
  "region": { "offset": 180, "length": 4 },
  "values": [1.0, 0.0, 0.25, 0.0],
  "ttlMs": 30000,
  "triggerPush": false
}
```

`triggerPush=true` immediately runs the PE-to-RE push cycle and returns the push
result inline. `triggerPush=false` only updates the perception source; the next
manual or automatic push will include the signal.

Set `compactPush: true` with `triggerPush: true` to omit per-machine transition
details from the inline push result while keeping the merged perceptual space.

Concurrent push attempts are coalesced. The request that arrives while another
push is already in flight receives `409` with `coalesced: true`; after the
in-flight push completes, the worker performs one compact follow-up push so the
latest sensor values are carried into Reality Engine without advancing source
cursors in parallel.

## Alignment Notes

The Scala/Node Perception Engine source model remains the primary compatibility
surface:

- `test` sources step through authored input sequences.
- `simulated` sources generate synthetic values.
- `sensor` sources accept external values with TTL expiry.

The C++ implementation adds `/api/signals` and `/api/integrations/localai/*` as
operator-friendly wrappers around the same sensor source model. These wrappers
do not change Reality Engine semantics.
