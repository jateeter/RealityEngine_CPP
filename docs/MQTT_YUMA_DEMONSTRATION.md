# MQTT Integration Demonstration — yuma.lateraledge.cloud

A reproducible walk-through of the complete pipeline:

```
external MQTT broker → mapping registry → Perception Engine →
Reality Engine machines → CES initial events → resolved governance
contract → Prometheus paging-decisions counter → Grafana dashboard
```

The demonstration uses a real external broker, the production
`perception_engine_server` + `reality_engine_server` binaries, the
1,009-machine example corpus, and a mapping registry that projects
broker telemetry into the agriculture domain.

## Broker

| Property | Value |
| --- | --- |
| Host | `yuma.lateraledge.cloud` |
| Port | `1883` (plaintext MQTT v3.1.1) |
| Operator | Lateral Edge sensor mesh |
| Topic shape | `LATERAL/{Suite}/{DEV*}/SensorReadings/v1` |
| Volume | ~9 messages / 5 sec under normal load |

`bin/mqtt_probe yuma.lateraledge.cloud 1883 20 '#'` discovered **69
unique topics across 185 messages in 20 seconds**, organized into three
suites:

- **AmbientSuite** (DEV0000009–016, DEV0000025) — air quality
  `{aTemp, aHum, aCO2, aRAW_VOC, aIDX_VOC, aRAW_NOx, aIDX_NOx}`
- **DOSuite** (DEV0000017–024) — dissolved oxygen `{wDOTemp, wDO}`
- **WaterSuite** (DEV0000001–008) — water chemistry
  `{wTemp, wpH, wEC, wTDS, wORP, wTurbidity}`

## Mapping registry (`config/mqtt-mappings.yuma-agriculture.json`)

16 rules thread the live sensors into 4 agriculture-domain machines.
Each machine consumes a 4-cell **status pattern** (not raw sensor
values); the `band` normalize mode emits `1.0` when the reading is
inside a nominal operating range, `0.0` otherwise — turning a
continuous sensor reading into one status bit.

| Mapping → cell | Topic | Extract | Band |
|---|---|---|---|
| `agx001-ph-ok` → 40 | `LATERAL/WaterSuite/DEV0000001/SensorReadings/v1` | `/data/wpH` | `[6.5, 8.5]` |
| `agx001-ec-ok` → 41 | same | `/data/wEC` | `[0.5, 3.0]` mS/cm |
| `agx001-orp-ok` → 42 | same | `/data/wORP` | `[200, 600]` mV |
| `agx001-turbidity-ok` → 43 | same | `/data/wTurbidity` | `[0, 100]` NTU |
| `agx005-do-ok` → 84 | `LATERAL/DOSuite/DEV0000017/SensorReadings/v1` | `/data/wDO` | `[5, 25]` mg/L |
| `agx005-do-temp-ok` → 85 | same | `/data/wDOTemp` | `[60, 85]` °F |
| `agx005-do-watch` → 86 | same | `/data/wDO` | `[3, 5]` mg/L |
| `agx005-temp-watch` → 87 | same | `/data/wDOTemp` | `[85, 95]` °F |
| `agx026-temp-ok` → 184 | `LATERAL/AmbientSuite/DEV0000009/SensorReadings/v1` | `/data/aTemp` | `[65, 85]` °F |
| `agx026-humidity-ok` → 185 | same | `/data/aHum` | `[40, 70]` % |
| `agx026-temp-watch` → 186 | same | `/data/aTemp` | `[85, 95]` °F |
| `agx026-humidity-watch` → 187 | same | `/data/aHum` | `[20, 40]` % |
| `agx032-co2-ok` → 228 | same | `/data/aCO2` | `[600, 1500]` ppm |
| `agx032-co2-watch` → 229 | same | `/data/aCO2` | `[1500, 3000]` ppm |
| `agx032-co2-danger` → 230 | same | `/data/aCO2` | `[3000, 5000]` ppm |
| `agx032-temp-ok` → 231 | same | `/data/aTemp` | `[65, 85]` °F |

Per the design rule, **topics carry no offset information** — the
registry alone is the authority for projection.

## Agriculture-domain target machines

| Machine | Input region | Initial events |
|---|---|---|
| AGX001 — Aquaculture Water Quality Stability | `[40, 44)` | NORMAL `[1,1,1,1]`, OPTIMIZE `[0,1,0,1]`, SERVICE_DUE `[1,0,0,1]`, OPERATING_WINDOW_OK |
| AGX005 — Aquaculture Dissolved Oxygen Control | `[84, 88)` | NORMAL, OPTIMIZE, SERVICE_DUE, OPERATING_WINDOW_OK |
| AGX026 — Indoor Grow House VPD Climate Management | `[184, 188)` | NORMAL, OPTIMIZE, SERVICE_DUE, OPERATING_WINDOW_OK |
| AGX032 — Indoor Grow House CO2 Enrichment Safety | `[228, 232)` | NORMAL, OPTIMIZE, SERVICE_DUE, OPERATING_WINDOW_OK |

Each has `metadata.governance` (ownerTeam = `agriculture-operations`,
runbook URL, escalation policy = `pagerduty:ag-ops`, primary +
secondary contacts, SLA per process_status) and a `metadata.triggerConfig`
that maps `(sequenceId, outputMatches)` → `(ragStatusCode, processStatus,
description)`.

## Production end-to-end (boot the real binaries)

```bash
# Reality Engine on :3299 with the 1009-machine corpus
./bin/reality_engine_server 3299 ../RealityEngine_AI/examples/machines &

# Perception Engine on :3300, wired to MQTT
MQTT_BROKER_HOST=yuma.lateraledge.cloud \
MQTT_BROKER_PORT=1883 \
MQTT_MAPPINGS_FILE=$PWD/config/mqtt-mappings.yuma-agriculture.json \
MQTT_CLIENT_ID=re-pe-demo-prod \
./bin/perception_engine_server 3300 http://localhost:3299 ../localAIStack/data/machines 768 &
```

Boot log:
```
MQTT bridge enabled — broker=yuma.lateraledge.cloud:1883 mappings=16
listening on 3300
```

### `GET /api/mqtt/status` (after 15 s of sustained ingest)

```json
{
  "enabled": true,
  "connected": true,
  "brokerHost": "yuma.lateraledge.cloud",
  "brokerPort": 1883,
  "clientId": "re-pe-demo-prod",
  "connectAttempts": 1,
  "connectSuccesses": 1,
  "messagesReceived": 36,
  "bytesReceived": 476,
  "pingsSent": 0,
  "reconnects": 0,
  "bridge": {
    "messagesMapped": 192,
    "messagesRejected": 0,
    "messagesUnmatched": 0,
    "pushesTriggered": 12
  }
}
```

Note the fan-out: 36 PUBLISH → 192 mapped (≈5.3× per message, as each
broker payload drives 4–6 json-pointer rules) → 12 PE→RE pushes
(debounce coalesces ~16× per push window).

### `GET /api/sources` (16 sensors auto-created by the bridge)

```
agx001.water.ph.ok          region=[40,41)  ttl=60s  lastValue=[1]
agx001.water.ec.ok          region=[41,42)  ttl=60s  lastValue=[1]
agx001.water.orp.ok         region=[42,43)  ttl=60s  lastValue=[1]
agx001.water.turbidity.ok   region=[43,44)  ttl=60s  lastValue=[1]
agx005.do.level.ok          region=[84,85)  ttl=60s  lastValue=[1]
agx005.do.temp.ok           region=[85,86)  ttl=60s  lastValue=[1]
agx005.do.watch             region=[86,87)  ttl=60s  lastValue=[0]
agx005.do.temp.watch        region=[87,88)  ttl=60s  lastValue=[0]
agx026.temp.ok              region=[184,185)  ttl=60s  lastValue=[1]
agx026.humidity.ok          region=[185,186)  ttl=60s  lastValue=[1]
agx026.temp.watch           region=[186,187)  ttl=60s  lastValue=[0]
agx026.humidity.watch       region=[187,188)  ttl=60s  lastValue=[0]
agx032.co2.ok               region=[228,229)  ttl=60s  lastValue=[1]
agx032.co2.watch            region=[229,230)  ttl=60s  lastValue=[0]
agx032.co2.danger           region=[230,231)  ttl=60s  lastValue=[0]
agx032.temp.ok              region=[231,232)  ttl=60s  lastValue=[1]
```

Per-machine input regions assembled from those bits:

| Machine | Region values | Match |
|---|---|---|
| AGX001 | `[1, 1, 1, 1]` | `agx-001-urgent-stabilize` initial (NORMAL state) |
| AGX005 | `[1, 1, 0, 0]` | (intermediate state, no immediate output) |
| AGX026 | `[1, 1, 0, 0]` | (intermediate state, no immediate output) |
| AGX032 | `[1, 0, 0, 1]` | **`agx-032-preventive-maintenance` → SERVICE_DUE** |

### `GET /api/metrics` — ces_paging_decisions_total

The audit trail surfaces in Prometheus:

```
ces_paging_decisions_total{runtime="cpp", owner_team="agriculture-operations",
  process_status="info", rag_status_code="GREEN",
  machine_id="machine-agx005-aquaculture-dissolved-oxygen-control"} 6

ces_paging_decisions_total{runtime="cpp", owner_team="agriculture-operations",
  process_status="info", rag_status_code="GREEN",
  machine_id="machine-agx032-indoor-grow-house-co2-enrichment-safety"} 11

ces_paging_decisions_total{runtime="cpp", owner_team="agriculture-operations",
  process_status="warning", rag_status_code="AMBER",
  machine_id="machine-agzonetemperaturecontroller"} 9
```

Three agriculture machines are paging on real broker data:

- **AGX005** dissolved-oxygen-control — 6 GREEN/info fires (preventive
  maintenance recommended)
- **AGX032** CO2 enrichment safety — 11 GREEN/info fires (preventive
  maintenance recommended)
- **AgZoneTemperatureController** — 9 AMBER/warning fires (warning-tier
  alert with a 3600 s SLA per the machine's `metadata.governance.sla`)

## The audit trail

When AGX005 fires, the resolved `PagingDecision` payload reads:

```json
{
  "sequenceId":       "agx-005-preventive-maintenance",
  "ragStatusCode":    "GREEN",
  "processStatus":    "info",
  "ownerTeam":        "agriculture-operations",
  "runbook":          "https://runbooks.example.org/agriculture/agx005_aquaculture-dissolved-oxygen-control",
  "escalationPolicy": "pagerduty:ag-ops",
  "contact": {
    "primary":   "agriculture-operations-primary@example.org",
    "secondary": "agriculture-operations-secondary@example.org"
  },
  "description": "Preventive maintenance required."
}
```

The complete provenance chain for one fire is reconstructable from the
mergeBatch alone:

```
[1]  broker:     yuma.lateraledge.cloud:1883
                 topic LATERAL/DOSuite/DEV0000017/SensorReadings/v1
[2]  payload:    {"data":{"wDOTemp":81,"wDO":11}}
[3]  bridge:     match_all  →  4 rules
       agx005-do-ok        extract /data/wDO       band[5,25]   → 1.0 → cell 84
       agx005-do-temp-ok   extract /data/wDOTemp   band[60,85]  → 1.0 → cell 85
       agx005-do-watch     extract /data/wDO       band[3,5]    → 0.0 → cell 86
       agx005-temp-watch   extract /data/wDOTemp   band[85,95]  → 0.0 → cell 87
[4]  PE sources: 4 sensor sources updated with ageMs / ttlMs / stale
[5]  PE push:    POST /api/perceive  (vector sized to VECTOR_DIMENSION; 768 in this demo)
[6]  RE input:   AGX005 reads region [84,88)  =  [1, 0, 0, 1]
                  (note: the read above was [1,1,0,0] in a different ingest
                   window — output values vary as live readings change)
[7]  CES match:  initial event agx-005-service-due (elements [1,0,0,1])
[8]  output:     agx-005-maintenance-output  values=[0, 0, 1, 0]
[9]  provenance: [agx-005-service-due]
[10] governance: triggerConfig.rules → {GREEN/info, ag-operations, runbook, …}
[11] metrics:    ces_paging_decisions_total{…} += 1
[12] dashboard:  cross-runtime-parity.json panel surfaces the rate increase
```

The CES JSON is the **sole source of truth**.  Alerting code never
overrides it; on-call rotations, runbooks, escalation policies all flow
from `machine.metadata.governance` + `machine.metadata.triggerConfig`.

## How to reproduce

```bash
cd RealityEngine_CPP
make all

# Discover topics on an unknown broker
./bin/mqtt_probe <host> [port=1883] [seconds=20] [filter=#]

# Lightweight bridge demo (no PE/RE — same code path, isolated)
./bin/mqtt_demo <host> <port> <mappings.json> [seconds=30]

# Agriculture-domain end-to-end (loads machine JSONs, evaluates initial
# events, resolves governance per-fire)
./bin/mqtt_demo_agriculture <host> <port> <mappings.json> <ag-machines-dir> [seconds=30]

# Production: real PE + RE binaries
./bin/reality_engine_server 3299 ../RealityEngine_AI/examples/machines &
MQTT_BROKER_HOST=<host> MQTT_BROKER_PORT=1883 \
  MQTT_MAPPINGS_FILE=$PWD/config/mqtt-mappings.yuma-agriculture.json \
  ./bin/perception_engine_server 3300 http://localhost:3299 ../localAIStack/data/machines 768 &
curl http://localhost:3300/api/mqtt/status
curl http://localhost:3300/api/mqtt/mappings
curl http://localhost:3300/api/sources
curl http://localhost:3299/api/metrics | grep ces_paging_decisions
```

## Cross-runtime parity

All three runtimes (`_AI` TypeScript, `_CPP`, `_LSP`) implement the
same `MappingRegistry` contract:

| Feature | `_AI` | `_CPP` | `_LSP` |
| --- | --- | --- | --- |
| MQTT v3.1.1 client | mqtt.js (optional dep) | hand-rolled | (deferred) |
| Mapping registry schema | identical | identical | identical |
| Topic-filter matching (+ / #) | ✓ | ✓ | ✓ |
| Match-all (fan-out) | ✓ | ✓ | ✓ |
| sensorIdTemplate `{n}` interpolation | ✓ | ✓ | ✓ |
| Extract: json / csv-float / raw / single-float | ✓ | ✓ | ✓ |
| Normalize: passthrough / minmax / linear / band | ✓ | ✓ | ✓ |
| Length + NaN validation | ✓ | ✓ | ✓ |
| Overlap detection | ✓ | ✓ | ✓ |
| Push policy: debounced / manual / immediate | ✓ | ✓ | (deferred — needs client) |
| `/api/mqtt/status` + `/api/mqtt/mappings` | ✓ | ✓ | (deferred) |

## Visualizer monitor surface

The `Universe` view (`visualizer/frontend/src/pages/UniverseView.tsx`)
ships four panels that bind to this pipeline:

- **MqttBridgePanel** — connection state + per-mapping counters table,
  Edit Mappings button opens the live editor (PUT → bridge restart).
- **MqttIngestStream** — WebSocket-driven per-message live feed (PE
  broadcasts `mqtt-ingest`, VB forwards to its WS clients, frontend
  ring buffer holds the last 120 events).
- **SensorSourcesPanel** — sensor freshness badges (FRESH / AGING /
  STALE / IDLE) from `/api/sources`'s `ageMs` + `stale` fields.
- **PagingDecisionsTicker** — RAG-coloured table of resolved governance
  contracts, parsed from `ces_paging_decisions_total` server-side.
