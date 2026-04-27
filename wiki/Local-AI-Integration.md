# Local AI Integration

`RealityEngine_CPP` connects AI systems through the Perception Engine sensor
model. The Reality Engine receives assembled vectors and remains neutral about
the upstream AI provider. Deployment defaults to `VECTOR_DIMENSION=768`.

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
POST /api/integrations/localai/bootstrap
POST /api/signals
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

## Configuration

```bash
LOCAL_AI_API_URL=http://localhost:4000
LOCAL_AI_MACHINES_DIR=../localAIStack/data/machines
LOCAL_AI_BOOTSTRAP=false
VECTOR_DIMENSION=768
```

Set `LOCAL_AI_BOOTSTRAP=true` to register sensors and import bridge machines at
Perception Engine startup.
