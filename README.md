# RealityEngine_CPP

Native C++ implementation of the Reality Engine and Perception Engine
services.  Black-box equivalent to [`RealityEngine_AI`](../RealityEngine_AI)
(TypeScript, default) and [`RealityEngine_LSP`](../RealityEngine_LSP)
(Common Lisp) on the same machine JSON corpus, governance contracts, MQTT
mapping registry, and Prometheus metrics shape.

The native services use Boost.Asio/Beast for HTTP transport while keeping
the domain layer independent of the server implementation.  No external
runtime deps beyond Boost — the MQTT client is hand-rolled to preserve
that contract.

## What This Repo Provides

- Native C++ Reality Engine service on port `3299` by default.
- Native C++ Perception Engine service on port `3300` by default.
- Startup loading of all machine JSON files from
  `../RealityEngine_AI/examples/machines` (1009 example machines across
  11 domains).
- Shared Qdrant deployment model with `RealityEngine_AI` and `localAIStack`.
- Built-in MQTT v3.1.1 client + mapping registry (see [MQTT Integration](#mqtt-integration)).
- E2E validation against authored `inputSequences` in the machine JSON corpus.
- Prometheus `/api/metrics` (text exposition) with `runtime="cpp"` label
  uniformly across every metric — same shape as AI / LSP for a single
  cross-runtime Grafana dashboard.

## Build

```bash
brew install boost   # macOS, if Boost is not already installed
make all
make test            # unit + STA + MQTT mapping tests
make e2e             # 1009-machine end-to-end pass
```

## Run

The simplest entry point is the unified orchestrator from `RealityEngine_AI`:

```bash
# From either repo — both routes work
./startUniverse.sh --re-engine=cpp --pe-engine=cpp   # this repo's startUniverse.sh
                                                      # auto-selects --re-engine=cpp
                                                      # --pe-engine=cpp and delegates
                                                      # to RealityEngine_AI/startUniverse.sh
./stopUniverse.sh                                    # tears the CPP engines down
```

Or run the native binaries directly:

```bash
./start.sh
./stop.sh
```

`start.sh` builds the binaries, verifies the shared Qdrant instance, starts
both native services, and confirms that machines loaded successfully from
`../RealityEngine_AI/examples/machines` before declaring startup complete.
When `MQTT_BROKER_HOST` is exported, the PE auto-boots an MQTT bridge using
the canonical env-var contract (see [Configuration](#configuration)).

The Reality Engine service listens under `/api/...` matching the AI runtime.
The Perception Engine service listens under `/api/...` and pushes assembled
reality vectors to `POST /api/perceive` on the Reality Engine.  Deployment
defaults to `VECTOR_DIMENSION=768` as a legacy compatibility floor; the
intended vector model derives the logical perceptual-space dimension from
active machine and source mappings — see [Vector Management](docs/VECTOR_MANAGEMENT.md).

Operational defaults live in [.env.example](.env.example).  Copy it to
`.env` to override ports or paths.

Qdrant is shared, not owned, by this repo.  The scripts verify the unified
localAIStack Qdrant endpoint at `http://localhost:4333` and preserve the
common data repository at `../localAIStack/volumes/qdrant`.

Adapter-facing CLI:

```bash
make bin/reality_engine_cli
bin/reality_engine_cli pe integrations-status --pe-url http://localhost:3300
bin/reality_engine_cli pe dispatch-ledger --pe-url http://localhost:3300
bin/reality_engine_cli pe ollama-status --pe-url http://localhost:3300
bin/reality_engine_cli pe ollama-dispatch <dispatchId> --source-mapping-id agent-completion-risk
bin/reality_engine_cli pe openai-status --pe-url http://localhost:3300
bin/reality_engine_cli pe openai-dispatch <dispatchId> --source-mapping-id agent-completion-risk
bin/reality_engine_cli pe acp-status --pe-url http://localhost:3300
bin/reality_engine_cli pe acp-dispatch <dispatchId> --source-mapping-id acp-openclaw-completion
bin/reality_engine_cli pe healthkit-status --pe-url http://localhost:3300
bin/reality_engine_cli pe healthkit-ingest --sample-type step-count --source-mapping-id healthkit-activity --values 1,0,0.9,0
bin/reality_engine_cli pe carekit-status --pe-url http://localhost:3300
bin/reality_engine_cli pe carekit-ingest --sample-type task-adherence --source-mapping-id carekit-task --values 1,0,0.8,0.95
bin/reality_engine_cli pe completion --source-mapping-id agent-completion-risk --agent cli --values 1,0,0.75,0
```

The CLI is a thin wrapper over the PE HTTPS API. It is intended for scripts and
MCP adapters that need the same status, ledger, dispatch annotation, and
completion-ingest contract without embedding HTTP details.

## MQTT Integration

The PE includes a hand-rolled MQTT v3.1.1 client and a mapping registry
that decides how broker topics project into perceptual space.  Topics
describe the outside world; the registry alone encodes RE offsets.

Mapping file: `config/mqtt-mappings.example.json` (and
`config/mqtt-mappings.yuma-agriculture.json` for the live yuma demo).
Schema fields per rule: `id`, `topicFilter` (with `+` / `#` wildcards),
`sensorIdTemplate` (`{1}`, `{2}` captures), `region { offset, length }`,
`extract { type: csv-float | json | raw | single-float, pointer?, index? }`,
`normalize { mode: passthrough | minmax | linear | band, min, max, scale,
offset, clamp }`, `ttlMs`, `qos`, `acceptRetained`, `pushMode: debounced
| manual | immediate`, `debounceMs`.

```bash
# Boot with MQTT enabled
MQTT_BROKER_HOST=broker.example.com \
MQTT_BROKER_PORT=1883 \
MQTT_MAPPINGS_FILE=$PWD/config/mqtt-mappings.example.json \
./start.sh
```

Endpoints exposed by the PE:

- `GET /api/mqtt/status` — connection state + bridge counters + broker config
- `GET /api/mqtt/mappings` — loaded registry + per-mapping counters

Three helper tools ship with the repo for broker discovery and testing
without the full PE/RE stack:

```bash
./bin/mqtt_probe <host> [port=1883] [seconds=20] [filter=#]
./bin/mqtt_demo <host> <port> <mappings.json> [seconds=30]
./bin/mqtt_demo_agriculture <host> <port> <mappings.json> <ag-machines-dir> [seconds=30]
```

A complete walk-through against `yuma.lateraledge.cloud:1883` (the Lateral
Edge sensor mesh) is documented in
[docs/MQTT_YUMA_DEMONSTRATION.md](docs/MQTT_YUMA_DEMONSTRATION.md) — covers
discovery, the agriculture-domain mapping file, the full audit trail from
broker payload to Prometheus paging-decisions counter, and reproduction
commands for every layer.

## Configuration

Canonical env vars (set in `.env`, on the CLI, or via `startUniverse.sh`
flags):

| Variable | Default | Purpose |
|---|---|---|
| `REALITY_ENGINE_PORT` | `3299` | RE bind port |
| `PERCEPTION_ENGINE_PORT` | `3300` | PE bind port |
| `VECTOR_DIMENSION` | `768` | Perceptual-space dimension floor |
| `MACHINES_DIR` | `../RealityEngine_AI/examples/machines` | Source of startup machines |
| `QDRANT_URL` | `http://localhost:4333` | Shared Qdrant REST |
| `LOCAL_AI_API_URL` | `http://localhost:4000` | localAIStack integration target |
| `LOCAL_AI_BOOTSTRAP` | `false` | Auto-register localAIStack sensors + machines |
| `RE_STRICT_STA` | unset | When `1`, enforce Single Transition Assumption on life-safety machines |
| `INTEGRATIONS_CONFIG` | `config/integrations.json` when present | Provider-neutral startup registry for integration source mappings |
| `OLLAMA_BASE_URL` | `http://localhost:11434` | Local Ollama adapter base URL |
| `OLLAMA_MODEL` | `gpt-oss:20b` | Default Ollama model for PE-controlled dispatch |
| `OLLAMA_COMPLETION_SOURCE_MAPPING_ID` | `agent-completion-risk` | Default source mapping for Ollama completion commits |
| `OPENAI_API_KEY` | unset | API key for caller-driven OpenAI Responses dispatch |
| `OPENAI_BASE_URL` | `https://api.openai.com/v1` | OpenAI-compatible Responses API base URL |
| `OPENAI_MODEL` | `gpt-5` | Default OpenAI model for PE-controlled dispatch |
| `OPENAI_COMPLETION_SOURCE_MAPPING_ID` | `agent-completion-risk` | Default source mapping for OpenAI completion commits |
| `ACP_ENABLED` | `false` | Enable ACP/OpenClaw adapter metadata in PE status |
| `ACP_COMMAND` / `OPENCLAW_ACP_COMMAND` | `openclaw acp` | External OpenClaw ACP command recorded in no-wait handoff receipts |
| `ACP_GATEWAY_URL` / `OPENCLAW_GATEWAY_URL` | `ws://127.0.0.1:18789` | OpenClaw Gateway URL recorded for xACP handoff |
| `ACP_SESSION_KEY` / `OPENCLAW_ACP_SESSION` | `agent:main:main` | OpenClaw Gateway session key for example xACP handoff |
| `ACP_COMPLETION_SOURCE_MAPPING_ID` | `agent-completion-risk` | Default source mapping for ACP/OpenClaw completion commits |
| `HEALTHKIT_BRIDGE_ID` | `healthkit-ios-bridge` | Expected Apple-platform HealthKit bridge identity |
| `HEALTHKIT_DEFAULT_SOURCE_MAPPING_ID` | `healthkit-activity` | Default source mapping for HealthKit bridge ingest |
| `HEALTHKIT_BRIDGE_TOKEN` | unset | Optional shared token required in HealthKit bridge ingest payloads |
| `CAREKIT_BRIDGE_ID` | `carekit-ios-bridge` | Expected Apple-platform CareKit bridge identity |
| `CAREKIT_DEFAULT_SOURCE_MAPPING_ID` | `carekit-task` | Default source mapping for CareKit bridge ingest |
| `CAREKIT_BRIDGE_TOKEN` | unset | Optional shared token required in CareKit bridge ingest payloads |
| `MQTT_BROKER_HOST` | unset | Set to enable the MQTT bridge |
| `MQTT_BROKER_PORT` | `1883` | Broker port |
| `MQTT_CLIENT_ID` | `reality-engine-pe` | Client identifier |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | unset | Optional auth |
| `MQTT_KEEPALIVE` | `60` | Keepalive seconds |
| `MQTT_MAPPINGS_FILE` | unset | Path to the registry JSON |
| `MQTT_MAPPINGS_JSON` | unset | Inline registry JSON (alternative to file) |
| `MQTT_ALLOW_REGION_OVERLAP` | `0` | Suppress overlap warnings when `1` |
| `TRIGGERS_ENABLED` | `false` | Enable PE trigger envelope recording from RE `mergeBatch` results |
| `TRIGGER_DISPATCH_MODE` | `dry-run` | Label for trigger dispatch target (`dry-run`, `graphql`, `openai`, `ollama`, `acp`, `mcp`) |
| `TRIGGER_GRAPHQL_URL` | `${LOCAL_AI_API_URL}/graphql` | localAIStack GraphQL target metadata for trigger envelopes |

## Documentation

- [Machine Concept](MACHINE_CONCEPT.md) — DFA theory, JSON schema, perceptual mapping, regex equivalences, STA
- [Documentation Index](docs/README.md)
- [Architecture](docs/ARCHITECTURE.md)
- [API Equivalence](docs/API_EQUIVALENCE.md)
- [Integration Architecture](docs/INTEGRATION_ARCHITECTURE.md)
- [Reality Engine OpenAPI](docs/openapi/reality-engine.yaml)
- [Perception Engine OpenAPI](docs/openapi/perception-engine.yaml)
- [MQTT Yuma Demonstration](docs/MQTT_YUMA_DEMONSTRATION.md)
- [Local AI Integration](docs/LOCAL_AI_INTEGRATION.md)
- [Vector Management](docs/VECTOR_MANAGEMENT.md)
- [Operations](docs/OPERATIONS.md)
- [E2E Testing](docs/E2E_TESTING.md)
- [Acronyms](docs/ACRONYMS.md)
- [Bibliography](docs/BIBLIOGRAPHY.md)
- [GitHub Wiki Source](wiki/Home.md)

## Scope

Implemented in this repo:

- `RealityVector`, `CriticalEventSequence`, `Machine`, `OutputArbiter`,
  governance / `PagingDecision`, deprecation lifecycle, STA checker,
  Option A1 narrow-cell declaration + pack/unpack helpers.
- Configurable universal `PreceptionEngine` extraction and output merge.
- `PerceptualSpaceSimulator` snapshot → parallel process → deterministic
  merge loop.  Merge batch sorted by `(machineId, sequenceId, outputIndex)`
  using byte-wise lex — byte-identical to AI / LSP ordering.
- `PerceptionEngine` source assembly for test, simulated, and sensor sources.
- MQTT v3.1.1 client + `MappingRegistry` + `MqttBridge` with fan-out
  dispatch (one PUBLISH on a shared topic drives every matching rule)
  and per-mapping observability counters.
- Boost.Asio/Beast HTTP endpoints with bounded request workers and
  persistent pooled PE-to-RE client connections.
- Runtime controls for HTTP keep-alive limits, outbound timeouts, domain
  worker capacity, and worker-pool metrics.
- Compact `/api/perceive` and `/api/push` response mode for high-throughput
  integrations that don't need per-machine transition details.
- Machine JSON loading for the shared `examples/machines/*.json` corpus.

See [docs/API_EQUIVALENCE.md](docs/API_EQUIVALENCE.md) for endpoint status
across AI / CPP / LSP and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for
the domain-layer class topology.
