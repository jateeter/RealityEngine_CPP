# CareKit Bridge — native Apple app to the Perception Engine

The pipeline this bridge covers:

```
native Apple app (CareKit store) → normalized task/outcome payload →
POST /api/integrations/carekit/ingest → PE source mapping →
PE sensor source → PE push → Reality Engine machines
```

The native app owns the CareKit store, care plans, tasks, contacts, outcomes,
UI, user consent, and any device-local write-back. PE receives only authorized,
already-normalized bridge payloads and maps them into sources.

## The contract this implements

The rules every external integration obeys — that projection is registry-owned
and never carried by the payload, that ingress is the only thing that activates a
source, that governance is CES-owned — are specified once, in `RealityEngine_CI`:

    RealityEngine_CI/docs/EXTERNAL_INTEGRATION_CONTRACT.md

This document is a **worked instance** of that contract. It supplies what the
contract deliberately does not know: the bridge identity and its shared-secret
auth, the source mappings and their regions, the ingest payload shape, and the
reproduction commands.

Against the §5 verification ladder this bridge stands at **rung 2**. The PE
contract is implemented and exercised by fixtures in three runtimes, and
`enabled: false` is its default in the shipped registry. **No native CareKit app
exists** — unlike HealthKit, which has `localHealthkitBridge`. Nothing here has
been run against a real CareKit store, and a rung-2 pass is not evidence that it
would work.

The per-runtime route surface is **not** restated here.
`GET /api/integrations/carekit/status`, `POST /api/integrations/carekit/ingest`,
their response shapes, and which runtimes implement them live in
`RealityEngine_CI/SURFACE_SPEC.md` § CareKit Integration.

## Bridge identity

`RealityEngine_CI/config/integrations.json`, entry `carekit-ios-bridge`:

| Property | Value |
|---|---|
| `id` / `bridgeId` | `carekit-ios-bridge` |
| `kind` | `carekit` |
| `enabled` | `false` — opt in per deployment |
| `transport` | `https` |
| `defaultSourceMappingId` | `carekit-task` |
| Authentication | shared secret via `CAREKIT_BRIDGE_TOKEN`; `tokenConfigured` is reported by `/status`, the token itself never is |

Environment overrides, read at PE startup: `CAREKIT_BRIDGE_ID`,
`CAREKIT_DEFAULT_SOURCE_MAPPING_ID`, `CAREKIT_BRIDGE_TOKEN`.

## Source mappings

Two mappings carry the two things a care plan produces. Per
`EXTERNAL_INTEGRATION_CONTRACT.md` §2.1, these are the sole authority for
projection — a bridge payload names a task and a sample type, never a region.

| Mapping | Region | `sensorIdTemplate` | Extract pointers |
|---|---|---|---|
| `carekit-task` | `[4310:4314]` | `carekit.{taskId}.{sampleType}` | `/completed`, `/missed`, `/adherence`, `/confidence` |
| `carekit-outcome` | `[4314:4318]` | — | `/value`, `/adherenceValue`, `/trend`, `/confidence` |

Both normalize `passthrough` with `clamp: true`, hold `ttlMs` 900000 (15
minutes), and push `debounced` at 250 ms. The TTL is the operative difference
from the agent mappings at `[4200:4210]`, which expire in 5 minutes: adherence is
a slow signal, and a task completed twenty minutes ago is still true.

## What the PE implements

RealityEngine_CPP implements only the PE bridge contract:

- `GET /api/integrations/carekit/status` — configured bridge id, default source
  mapping, whether a bridge token is required, and the ingest endpoint.
- `POST /api/integrations/carekit/ingest` — one normalized task/outcome sample,
  or a `samples[]` batch, committed through the same PE source path that
  `POST /api/signals` uses. This is the contract's §3 ingress rule: CareKit is
  not special-cased downstream.
- `bin/reality_engine_cli pe carekit-status`
- `bin/reality_engine_cli pe carekit-ingest --sample-type task-adherence --source-mapping-id carekit-task --values 1,0,0.8,0.95`

RE never talks to CareKit. CareKit state reaches RE only when PE aggregates the
mapped source into an input-space Reality Event — the contract's §2.5.

## What stays in the native app

CareKit entitlements, user authorization and consent, care-plan and task
modelling, outcome capture, on-device privacy handling, and any user-confirmed
write-back to the CareKit store. PE expects already-authorized, normalized values
plus optional provenance metadata.

## How to reproduce

```bash
cd RealityEngine_CPP
make all

# Start the PE with the bridge enabled
CAREKIT_BRIDGE_TOKEN=<secret> \
INTEGRATIONS_CONFIG=../RealityEngine_CI/config/integrations.json \
./bin/perception_engine_server 5300 http://localhost:5301 ../RealityEngine_Machines/machines &

curl http://localhost:5300/api/integrations/carekit/status

# Ingest one normalized task sample
./bin/reality_engine_cli pe carekit-ingest \
  --sample-type task-adherence \
  --source-mapping-id carekit-task \
  --values 1,0,0.8,0.95

curl http://localhost:5300/api/sources | grep carekit
```

Fixture coverage exists in `tests/e2e_services.sh` here,
`tests/core-tests.lisp` in RealityEngine_LSP, and
`MappingIntegrationGateSpec.scala` in RealityEngine_Scala.

## Known limitations

- **No native app.** Rung 3 and 4 of the verification ladder are unreachable
  until one exists; `localHealthkitBridge` is the model for what that would be.
- **`SURFACE_SPEC.md` § CareKit Integration shows `defaultSourceMappingId:
  "carekit-activity"`** while the shipped registry configures `carekit-task`.
  One of the two is wrong and they should be reconciled before a native bridge
  is written against either.
