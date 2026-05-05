# Local AI Integration

`RealityEngine_CPP` connects AI systems through the Perception Engine sensor
model. The Reality Engine receives assembled vectors and remains neutral about
the upstream AI provider. `VECTOR_DIMENSION` is a legacy compatibility floor;
the logical perceptual-space dimension should be derived from active machine and
source mappings. See [Vector Management](Vector-Management).

## Compatibility Profile

The built-in localAIStack profile mirrors the current Python bridge:

| Sensor | Region | Purpose |
| --- | --- | --- |
| `localai_rag_retrieval` | `[52:56]` | Retrieval count and average score. |
| `localai_rag_grading` | `[56:60]` | Kept ratio and rewrite count. |
| `localai_agent_activity` | `[64:68]` | Tool calls, errors, reasoning depth. |

Bootstrap can import the bridge machines from `../localAIStack/data/machines`.

## Endpoints

```http
GET /api/integrations/localai/status
GET /api/integrations/localai/catalog
POST /api/integrations/localai/bootstrap
POST /api/integrations/localai/invoke
POST /api/signals
```

`GET /api/integrations/localai/catalog` dynamically reads the localAIStack
operational surface from `LOCAL_AI_API_URL`, including `/health`,
`/graph/schema`, and `/graphql/events`. It returns allowed graph, RAG, chat,
GraphQL, and ingestion endpoints plus the active Reality Engine topology
metadata when localAIStack is reachable.

`POST /api/integrations/localai/invoke` provides guarded dynamic access to
allowed localAIStack `GET` and `POST` paths. Example:

```json
{
  "method": "POST",
  "endpoint": "/graph/rag",
  "payload": {
    "question": "What documents describe Reality Engine integration?"
  }
}
```

`POST /api/signals` updates an existing sensor or creates one when a region is
provided:

```json
{
  "sensorId": "localai_rag_retrieval",
  "values": [0.4, 0.82, 0.0, 0.0],
  "triggerPush": true
}
```

Use `compactPush: true` with `triggerPush: true` to keep the inline push result
small. Concurrent push attempts are single-flight coalesced: the duplicate
request returns `409` with `coalesced: true`, and the worker performs one
compact follow-up push after the active push completes.

## Configuration

```bash
LOCAL_AI_API_URL=http://localhost:4000
LOCAL_AI_MACHINES_DIR=../localAIStack/data/machines
LOCAL_AI_BOOTSTRAP=false
VECTOR_DIMENSION=768
```

Set `LOCAL_AI_BOOTSTRAP=true` to register sensors and import bridge machines at
Perception Engine startup.

Machine `inputSequences` are also materialized as inactive PE `test` sources on
startup. Each source carries the source inputs, sequence metadata, and the full
authored sequence object for PE simulation and inspection.
