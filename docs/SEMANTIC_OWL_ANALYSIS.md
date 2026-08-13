# Semantic OWL Integration Analysis

Last reviewed: 2026-08-11

## Scope

This analysis covers OWL/ROBOT semantic verification for the active
RealityEngine universe, with emphasis on:

| Path | Meaning |
| --- | --- |
| `PE.x.MCP.x.PE` | Perception Engine interaction with localAIStack MCP/RAG/Ollama services, followed by source-mapped completion back into PE. |
| `PE.x.ACP.x.PE` | Perception Engine interaction with localOpenClawStack through ACP/xACP, followed by source-mapped completion back into PE. |

The active corpus and semantic model are owned by `RealityEngine_Machines`.
`RealityEngine_CPP`, `RealityEngine_Scala`, and `RealityEngine_LSP` consume that
corpus and expose runtime semantic identity; they do not locally redefine the
ontology.

## Current Evidence

| Validation class | Current status |
| --- | --- |
| RE/PE interoperability | Exercised through `RealityEngine_CI` regression profiles across active engine pairs. Service health, registry alignment, contract parity and schema validity pass; byte-level response equivalence is asserted and currently fails past the first input event (`RealityEngine_Scala#33`, `RealityEngine_LSP#38`). See `API_EQUIVALENCE.md`. |
| Machine corpus loading | Driven from `RealityEngine_Machines`; startup checks require nonzero loaded machines. |
| JSON schemas | Validated in the corpus and integration repositories. |
| MQTT data flow | Validated through CI/runtime integration evidence. |
| HealthKit/iPhone ingress | Enabled through `localHealthKitBridge` and startup-configured PE mappings. |
| Epic/PIM ingress | Enabled through the OpenCommons Health/PIM integration path. |
| Semantic identity | Exposed through `GET /api/machines/semantics/:name`. |
| Semantic buses | Exposed through `GET /api/buses/semantic` and `GET /api/buses/semantic/:id`. |
| Runtime semantic audit | Exposed through `GET /api/audit/semantics`. |
| Static OWL generation | Implemented in `RealityEngine_Machines/semantics`. |
| ROBOT reasoning | Available through `RealityEngine_Machines/scripts/reason-owl.sh`, which now reports against a project profile and reasons under both ELK and HermiT (`RealityEngine_Machines#46`). Still non-blocking when ROBOT is absent: CI must install it for the gate to be real. |

Active limited-machine regression worktrees are compatible with this model. The
important control is that limited profiles remain explicit, generated OpenClaw
profiles remain checkable, and promotion from limited to wider corpus coverage is
handled as a CI milestone.

## Current Semantic Model

`RealityEngine_Machines` provides the ontology and generated semantic corpus:

| Asset | Role |
| --- | --- |
| `semantics/ontology/re-core.ttl` | TBox and RealityEngine semantic vocabulary. |
| `semantics/abox/*.ttl` | Generated machine/domain ABoxes. These are generated artifacts and should not be hand-edited. |
| `semantics/abox-manifest.json` | Stable machine semantic identity manifest. |
| `domains/semantic-bus-registry.json` | Domain/input-space semantic bus allocation and interconnect registry. |
| `docs/SEMANTIC_AUDIT_CONTRACT.md` | Runtime semantic audit contract for PE and RE records. |
| `scripts/reason-owl.sh` | ROBOT report and ELK reasoning entrypoint. |

`RealityEngine_CPP` already reads the manifest and bus registry at runtime and
exposes them through HTTP. This is the key API-equivalence anchor: semantic
coverage is not merely a corpus build artifact; it is part of the runtime
surface.

## Static Provability Path

The static path should answer: "Can this authored workflow be proven coherent
before any deployment runs?"

Today, the static path can prove:

| Proven item | Mechanism |
| --- | --- |
| Machine semantic identity exists | ABox manifest and `GET /api/machines/semantics/:name` parity checks. |
| Machine actions use normalized vocabulary | Corpus generation and strict action checks. |
| Semantic bus reservations are named and discoverable | Semantic bus registry and RE bus endpoints. |
| Ontology has no ROBOT report errors when ROBOT is installed | `scripts/reason-owl.sh`. |
| Reasoner can classify the generated ontology/ABox bundle | ROBOT `reason --reasoner ELK`. |

The static path does not yet fully prove:

| Gap | Why it matters |
| --- | --- |
| MCP tool calls are ontology-linked to source mappings | A localAIStack tool result can be schema-valid without being semantically tied to the PE region it updates. |
| ACP/OpenClaw agent specs are ontology-linked to machine inputs | Generated OpenClaw agents carry input axes, but the OWL model should prove the axes, completion mapping, and machine semantics are aligned. |
| Completion write-backs are provably constrained | A completion mapping should be provably restricted to the intended PE source region and allowed value domain. |
| Safety/escalation invariants are reasoner-visible | RED/life-safety dispatches need explicit semantic classes and restrictions, not only JSON tests. |
| Limited regression profile membership is reasoner-visible | The limited corpus should be an explicit named profile with expected machine/agent bindings. |

## Dynamic Runtime Path

The dynamic path should answer: "Did this actual data path preserve the semantic
contract during a PE.x.RE.x.PE cycle?"

Today, runtime evidence includes:

| Runtime record | Source |
| --- | --- |
| Machine semantic identity and hash | RE manifest lookup. |
| Sequence observation records | RE semantic audit records. |
| PE perception/source write records | PE semantic audit records. |
| Dispatch ledger records | PE dispatch/completion paths. |
| Semantic metrics | CI metrics parity and guardrail dashboard. |

The runtime path does not yet run ROBOT over live traces. That should be added
as an out-of-band verifier, not as synchronous work inside the PE push cycle.
The PE hot path should remain non-blocking; semantic trace material should be
exported, normalized to RDF, and reasoned by CI or a sidecar process.

## PE.x.MCP/localAIStack Analysis

localAIStack provides local RAG/Ollama/MCP-adjacent capabilities through guarded
PE integration endpoints. The current contract is pragmatic and correct:
PE invokes allowed endpoints, localAIStack produces an answer or completion, and
PE writes results back through source mappings.

The semantic upgrade is to make each localAIStack interaction ontology-visible:

| Needed concept | Purpose |
| --- | --- |
| `re:MCPInvocation` | Runtime event representing a guarded tool/resource invocation. |
| `re:MCPToolResult` | Runtime event representing returned localAIStack evidence. |
| `re:LocalAIProvider` | Provider identity for local RAG/Ollama services. |
| `re:SourceMappingWrite` | Completion write-back into a PE source region. |
| `re:AllowedInvocation` | Static policy linking machine action, tool class, endpoint, and autonomy level. |
| `re:EvidenceArtifact` | RAG result, graph result, or agent summary used as decision evidence. |

The key invariant is simple: localAIStack may inform or complete a PE source
mapping, but it must not bypass the source mapping, write outside the allowed
region, or convert non-critical evidence into a critical action without an
ontology-valid trigger path.

## PE.x.localOpenClawStack Analysis

localOpenClawStack provides ACP/xACP agent execution. The correct runtime shape
is already documented: PE accepts an ACP handoff, records the dispatch, returns
immediately, and lets an external adapter drive OpenClaw. Completion returns to
PE through the completion/write-back path.

The semantic upgrade is to prove the generated OpenClaw agent set matches the
limited or full machine corpus profile:

| Needed concept | Purpose |
| --- | --- |
| `re:ACPDispatch` | Runtime event representing PE acceptance of OpenClaw handoff. |
| `re:OpenClawAgentBinding` | Static binding between machine, OpenClaw agent, input axes, and completion mapping. |
| `re:AgentInputAxis` | Named semantic axis expected by the agent. |
| `re:CompletionMapping` | Mapping from agent output to PE source region and value domain. |
| `re:NoWaitDispatch` | Constraint that OpenClaw work is outside the synchronous PE push path. |
| `re:AgentAutonomyMode` | Explicit classification of suggest, draft, approve, execute, or restricted autonomy. |

The key invariant is: OpenClaw can reason over observations and produce proposed
normalized inputs, but RE remains the deterministic evaluator of critical event
sequences and PE remains the sole owner of source-mapped writes.

## ROBOT Fit

ROBOT is a good fit for:

| Use | Fit |
| --- | --- |
| Ontology syntax and profile report | Strong. |
| Classification and consistency with ELK/HermiT | Strong for static generated corpus bundles. |
| Static profile drift checks | Strong when generated ABoxes include profile membership and agent bindings. |
| Runtime trace consistency | Strong if traces are exported to RDF named graphs. |
| Closed-world cardinality and "exactly this route wrote exactly this region" checks | Partial; pair ROBOT with deterministic JSON/RDF trace checks, and optionally SHACL later. |

The immediate path should use ROBOT for OWL consistency and classification, plus
small deterministic checkers for closed-world runtime facts. That combination is
more reliable than pretending OWL alone enforces all operational constraints.

## Inefficiency And Exploit Points

| Point | Risk | Control |
| --- | --- | --- |
| Missing ontology identity on integration events | Hard to join runtime traces to corpus semantics. | Include `machineIri`, `sequenceIri`, `sourceMappingId`, and `completionMappingId` in dispatch/completion ledgers. |
| ROBOT optionality | CI can report semantic coverage while silently skipping reasoning. | Install ROBOT in semantic CI lanes and fail selected profiles when ROBOT is absent. |
| Open-world OWL assumptions | Invalid closed-world runtime writes may remain logically satisfiable. | Add deterministic trace checks beside ROBOT. |
| Limited profile drift | OpenClaw generated agents can fall out of sync with the regression corpus. | Make profile generation `--check` mandatory before OpenClaw startup validation. |
| Runtime reasoner latency | Synchronous PE push could become slow or flaky. | Run ROBOT over exported traces out of band. |
