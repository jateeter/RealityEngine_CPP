# Integration Architecture

This document defines the provider-neutral integration model for the active
RealityEngine runtimes. The deprecated TypeScript prototype is frozen and used only as a
historical reference; `RealityEngine_Scala` replaces it for active runtime
reference work. `RealityEngine_Machines` owns the active corpus, contracts,
oracles, and trigger examples.

## Design Rule

Reality Engine remains deterministic. Perception Engine owns integration
dispatch and source mapping. External systems never resume Reality Engine
workflows directly; they contribute new perceptual facts through PE sensor
sources.

The normal flow is asynchronous:

1. PE assembles a vector and pushes it to RE.
2. RE processes machines and returns a deterministic step result.
3. PE scans `mergeBatch` for governance-resolved trigger outputs.
4. PE records versioned trigger envelopes and emits dispatch events without
   waiting for external agents, model calls, or tool loops.
5. External systems complete independently.
6. Completion returns through a configured PE source mapping.
7. Normal machine interconnections decide what that completion means.

## Core Abstractions

### Integration Registry

Startup configuration normalizes provider-specific settings into one catalog.
Existing MQTT and localAI environment variables remain compatibility inputs, but
new integrations should prefer a registry file such as
`config/integrations.example.json`.

RealityEngine_CPP now loads this registry at PE startup when
`INTEGRATIONS_CONFIG` points to a JSON file, or when `config/integrations.json`
exists in the working directory. `GET /api/integrations/status` reports the
loaded providers and source mappings. Completion callbacks may reference a
configured source mapping by `sourceMappingId`.

Supported integration kinds are:

- `mqtt`
- `localai`
- `openai`
- `ollama`
- `acp`
- `healthkit`
- `carekit`
- `mcp`
- `manual`

### Source Mapper

Every accepted inbound result resolves to the existing PE sensor-source shape:

```json
{
  "sensorId": "agent.example.completion",
  "region": { "offset": 4200, "length": 4 },
  "values": [1, 0, 0.82, 0],
  "ttlMs": 300000
}
```

Provider-specific payloads may differ, but PE commit semantics do not. MQTT
messages, HealthKit bridge uploads, CareKit bridge uploads, OpenAI responses,
Ollama responses, ACP harness results, MCP tool results, and manual callbacks
must all enter through configured sources.

### Trigger Envelope Dispatcher

PE observes RE `mergeBatch` entries after a push completes. If an entry carries
a governance decision produced from machine `metadata.triggerConfig`, PE builds
a `ces.terminal.event` envelope.

The envelope is based on
`RealityEngine_Machines/triggers/ai_trigger_envelope.template.json` and
captures:

- envelope and correlation identifiers
- RE/PE source identity
- CES machine, sequence, region, provenance, and deprecation data
- terminal output vector
- governance and paging contract
- dispatch target from `metadata.dispatchableAgent`
- trigger hook from `metadata.aiTrigger`

Dispatch is fire-and-record. The PE cycle does not wait for the external target.

### Dispatch Ledger

The ledger is an audit/outbox record, not a workflow engine. It tracks:

- `dispatchId`
- `envelopeId`
- `correlationId`
- target agent/system
- dispatch mode
- delivery status
- provider receipt, when available
- the versioned envelope payload

Agent or provider completion is represented later by source updates.

Adapters may read and annotate records through:

- `GET /api/dispatch/ledger`
- `GET /api/dispatch/records/{id}`
- `PATCH /api/dispatch/records/{id}`

The patch endpoint is for delivery metadata only: `status`, attempts, adapter
name, external provider run id, receipt payload, and error text. It does not
complete an agent result, unblock a PE cycle, or drive RE state. Completion
continues to flow through PE source mappings.

### Completion Ingest

Agent and provider results return to PE through
`POST /api/integrations/completions`. This endpoint is a provider-neutral
adapter around `POST /api/signals`; it accepts completion metadata plus either a
direct source target or a startup-resolved `sourceMapping` block:

```json
{
  "provider": "openai",
  "agent": "paging-decision",
  "correlationId": "corr_123",
  "envelopeId": "env_456",
  "sourceMappingId": "agent-completion-risk",
  "values": [1, 0, 0.82, 0],
  "ttlMs": 300000,
  "triggerPush": false
}
```

Default behavior is commit-only: PE updates or creates the configured sensor
source, broadcasts state, and returns. If `triggerPush` is explicitly true, PE
uses the existing push path. The endpoint is therefore compatible with CLI,
HTTPS, WS, MCP, OpenAI webhook, Ollama adapter, and HealthKit bridge returns
without adding a second PE queue.

Direct `sensorId`, `region`, or `sourceMapping` fields may still be supplied for
manual and test callbacks; those fields override the configured mapping.

### MCP Gateway

MCP is the tool/resource protocol for RE/PE capabilities. CLI, HTTPS, WS, and
provider adapters call the same internal operations; none bypass PE policy.

RealityEngine_CPP includes `bin/reality_engine_cli` as the canonical CLI wrapper
for adapter operations. It calls the same HTTPS endpoints exposed to MCP
servers and external adapters:

```bash
bin/reality_engine_cli pe integrations-status
bin/reality_engine_cli pe dispatch-ledger
bin/reality_engine_cli pe dispatch-read <dispatchId>
bin/reality_engine_cli pe dispatch-update <dispatchId> --status delivered --adapter openai --external-run-id resp_123 --increment-attempts
bin/reality_engine_cli pe ollama-status
bin/reality_engine_cli pe ollama-dispatch <dispatchId> --source-mapping-id agent-completion-risk
bin/reality_engine_cli pe openai-status
bin/reality_engine_cli pe openai-dispatch <dispatchId> --source-mapping-id agent-completion-risk
bin/reality_engine_cli pe acp-status
bin/reality_engine_cli pe acp-dispatch <dispatchId> --source-mapping-id acp-openclaw-completion
bin/reality_engine_cli pe healthkit-status
bin/reality_engine_cli pe healthkit-ingest --sample-type step-count --source-mapping-id healthkit-activity --values 1,0,0.9,0
bin/reality_engine_cli pe carekit-status
bin/reality_engine_cli pe carekit-ingest --sample-type task-adherence --source-mapping-id carekit-task --values 1,0,0.8,0.95
bin/reality_engine_cli pe completion --source-mapping-id agent-completion-risk --agent paging-decision --values 1,0,0.82,0
```

This keeps CLI, HTTPS, and MCP workflows behaviorally equivalent. The CLI does
not call providers or run agents; it observes/annotates PE records and commits
completed results through PE source mappings.

Recommended tools:

- `re.read_state`
- `re.list_machines`
- `re.read_machine`
- `pe.list_sources`
- `pe.push_signal`
- `pe.enqueue_push`
- `trigger.replay`
- `dispatch.read_ledger`

Mutating tools must be policy-gated.

## Provider Adapters

### OpenAI

OpenAI dispatch uses the Responses API where hosted agentic behavior, remote MCP
tools, webhooks, or managed model execution are useful. OpenAI provider run IDs
are ledger metadata only. PE still validates outputs and commits source updates.

RealityEngine_CPP preserves the same provider-neutral contract used by Ollama:

- `GET /api/integrations/openai/status` reports the configured Responses API
  endpoint, model, API-key presence, and `/models` reachability when
  `OPENAI_API_KEY` is configured.
- `POST /api/integrations/openai/dispatch` takes a recorded `dispatchId`, builds
  a `POST /v1/responses` request from the trigger envelope, annotates the
  dispatch record as delivering/delivered/failed, and, when the response text is
  JSON containing a numeric `values` array, commits the result through
  `/api/integrations/completions`.
- `bin/reality_engine_cli pe openai-dispatch <dispatchId>` wraps the same HTTPS
  endpoint for scripts and future MCP tools.

OpenAI execution is explicit and caller-driven. PE push cycles still never wait
on OpenAI, hosted tool execution, or completion handling.

### ACP / OpenClaw xACP

ACP is the editor/client-to-agent protocol boundary. For this integration, PE
treats OpenClaw as the example xACP platform surface:

- `openclaw acp` is an ACP server bridge that forwards editor/client work into
  an OpenClaw Gateway session.
- OpenClaw ACP agent sessions use the ACP runtime path (`/acp ...` and
  `sessions_spawn({ runtime: "acp" })`) to run external harnesses.
- PE does not host the ACP session, execute the harness, or wait for an ACP turn.

RealityEngine_CPP exposes the PE-side handoff contract:

- `GET /api/integrations/acp/status` reports the configured OpenClaw command,
  Gateway URL, session key, target agent, completion mapping, and the fixed
  no-wait contract.
- `POST /api/integrations/acp/dispatch` takes a recorded `dispatchId`, annotates
  the dispatch ledger with an `openclaw-xacp` receipt, and returns `202
  Accepted`. The endpoint does not launch OpenClaw or block on ACP output.
- ACP/OpenClaw completion returns through `POST /api/integrations/completions`
  using `provider: "acp"` and the configured `sourceMappingId`, such as
  `acp-openclaw-completion`.

The intended external adapter loop is:

1. Poll or read PE dispatch records.
2. Call `acp-dispatch` to record that an OpenClaw/xACP handoff was accepted.
3. Drive OpenClaw through its ACP surface outside the PE push cycle.
4. Commit the finished values through `/api/integrations/completions`.

This preserves the same no-wait dispatch rule used by OpenAI, Ollama, MCP, and
manual adapters while keeping RE isolated from ACP runtime state.

### Ollama

Ollama dispatch uses local `/api/chat`, `/api/generate`, `/api/embed`, or
OpenAI-compatible endpoints. PE owns tool execution, structured-output
validation, retry policy, and source commits.

RealityEngine_CPP implements Ollama first because it is local and PE-controlled:

- `GET /api/integrations/ollama/status` reports the configured local endpoint,
  model, default completion mapping, and `/api/tags` reachability.
- `POST /api/integrations/ollama/dispatch` takes a recorded `dispatchId`,
  builds an Ollama `/api/chat` request from the trigger envelope, annotates the
  dispatch record as delivering/delivered/failed, and, when the model response
  contains a JSON `values` array, commits the result through
  `/api/integrations/completions`.
- `bin/reality_engine_cli pe ollama-dispatch <dispatchId>` wraps the same HTTPS
  endpoint for local scripts and future MCP tools.

The adapter is explicit and caller-driven. PE push cycles still never wait on
Ollama, model execution, tool calls, or completion handling.

### HealthKit

HealthKit integration is device-side. A native Apple bridge owns HealthKit
authorization, anchored reads, and user-confirmed writes. PE receives only
authorized bridge payloads and maps them into sources. Clinical HealthKit records
are read-only from HealthKit.

RealityEngine_CPP implements only the PE bridge contract:

- `GET /api/integrations/healthkit/status` reports the configured bridge id,
  default source mapping, optional bridge-token requirement, and ingest endpoint.
- `POST /api/integrations/healthkit/ingest` accepts one normalized sample or a
  `samples[]` batch from a native Apple-platform bridge and commits each sample
  through the same PE source path as `/api/signals`.
- `bin/reality_engine_cli pe healthkit-ingest ...` wraps the HTTPS endpoint for
  bridge smoke tests and future MCP tools.

The native app remains outside this repo. It is responsible for HealthKit
entitlements, user authorization, anchored object queries, unit normalization,
on-device privacy handling, and any user-confirmed HealthKit writes. PE expects
already-authorized, normalized values and optional provenance metadata; PE does
not talk to HealthKit directly.

### CareKit

CareKit follows the same PE-side bridge pattern as HealthKit. A native Apple
app owns the CareKit store, care plans, tasks, contacts, outcomes, UI, user
consent, and any device-local writeback. PE receives only authorized normalized
bridge payloads and maps them into sources.

RealityEngine_CPP implements only the PE bridge contract:

- `GET /api/integrations/carekit/status` reports the configured bridge id,
  default source mapping, optional bridge-token requirement, and ingest endpoint.
- `POST /api/integrations/carekit/ingest` accepts one normalized task/outcome
  sample or a `samples[]` batch from a native Apple-platform bridge and commits
  each sample through the same PE source path as `/api/signals`.
- `bin/reality_engine_cli pe carekit-ingest ...` wraps the HTTPS endpoint for
  bridge smoke tests and future MCP tools.

RE does not talk to CareKit directly. CareKit state affects RE only when PE
aggregates the mapped CareKit source into an input-space Reality Event.

### localAIStack GraphQL

The existing localAIStack `updateProcessState` trigger remains a dispatch
target. It is modeled as a `graphql` mode for trigger envelopes.

## C++ Status

RealityEngine_CPP currently includes the first implementation slice:

- `TRIGGERS_ENABLED` gates trigger envelope creation.
- `TRIGGER_DISPATCH_MODE` labels the dispatch mode, defaulting to `dry-run`.
- `TRIGGER_GRAPHQL_URL` configures the localAIStack GraphQL target metadata.
- `GET /api/triggers/status` reports trigger dispatcher counters.
- `GET /api/dispatch/ledger` returns recent recorded envelopes.
- `GET /api/dispatch/records/{id}` reads one recorded envelope.
- `PATCH /api/dispatch/records/{id}` annotates adapter/provider delivery
  metadata without changing PE/RE workflow state.
- `GET /api/integrations/status` reports startup-loaded integration registry
  state and source mappings.
- `POST /api/integrations/completions` maps provider/agent results into PE
  sources through the same path as `/api/signals`.
- `GET /api/integrations/acp/status` and
  `POST /api/integrations/acp/dispatch` provide the OpenClaw/xACP no-wait
  handoff surface.

This slice intentionally does not perform provider/model calls inside the PE
cycle. External completion must return through source mappings.

## ACP / OpenClaw Roadmap

1. Contract hardening: add cross-runtime conformance tests for ACP status,
   accepted-no-wait dispatch receipts, dispatch ledger patching, and ACP
   completion ingest through `sourceMappingId`.
2. External adapter: build a small OpenClaw runner outside PE that reads accepted
   ACP records, invokes `openclaw acp` or `acpx openclaw`, and posts completion
   values back through `/api/integrations/completions`.
3. Session policy: add registry fields for allowed Gateway URLs, session-key
   prefixes, target agents, working directories, and permission profiles; keep
   policy validation at the PE adapter edge.
4. Observability: emit ACP handoff metrics and correlate OpenClaw session keys,
   ACP run IDs, dispatch IDs, envelope IDs, and PE completion IDs.
5. Runtime parity: mirror the C++ ACP endpoints in the LSP runtime and document
   the TypeScript/AI reference contract so AI, CPP, and LSP expose the same
   integration shape.
6. OpenClaw example: provide a runnable example that maps
   `acp-openclaw-completion` into a domain machine, drives an OpenClaw ACP
   session from a recorded dispatch, and verifies that RE state changes only
   after PE ingests the completion source.
