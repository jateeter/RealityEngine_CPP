# Semantic OWL Framework Roadmap

Last reviewed: 2026-08-11

## Objective

Complete the semantic verification framework so authored workflows and runtime
data paths are provable across:

| Path | Target |
| --- | --- |
| Static corpus verification | ROBOT can report, reason, and fail on invalid workflow semantics before deployment. |
| Dynamic runtime verification | PE/RE integration traces can be exported to RDF and checked against the same ontology and corpus manifest. |
| PE.x.MCP/localAIStack | localAIStack invocations, evidence, and completions are ontology-linked to PE source mappings. |
| PE.x.ACP/localOpenClawStack | OpenClaw agent bindings, ACP dispatches, and completions are ontology-linked to machine inputs and PE source mappings. |

## Workstreams

| ID | Workstream | Owner repo | Completion gate |
| --- | --- | --- | --- |
| S1 | Make ROBOT a real semantic CI gate | `RealityEngine_CI`, `RealityEngine_Machines` | CI installs ROBOT and `scripts/owl-reasoner-check.sh` fails selected semantic profiles when ROBOT report or reasoning fails. |
| S2 | Extend the ontology for integration paths | `RealityEngine_Machines` | `re-core.ttl` models MCP invocation, ACP dispatch, provider identity, agent binding, completion mapping, source mapping writes, and autonomy class. |
| S3 | Generate static ABoxes for integration bindings | `RealityEngine_Machines`, `localAIStack`, `localOpenClawStack` | Generated RDF joins machines, semantic buses, localAIStack allowed endpoints, OpenClaw agent specs, and completion mappings. |
| S4 | Add static provability checks | `RealityEngine_CI`, `RealityEngine_Machines` | ROBOT plus deterministic profile checks prove each selected workflow has valid machine identity, source mapping, completion mapping, action vocabulary, and provider binding. |
| S5 | Export runtime semantic traces | `RealityEngine_CPP`, `RealityEngine_Scala`, `RealityEngine_LSP` | `GET /api/audit/semantics` records can be exported as RDF/JSON-LD with stable IRIs and dispatch/completion join keys. |
| S6 | Add dynamic ROBOT validation | `RealityEngine_CI` | Regression test emits trace RDF, merges it with ontology and profile ABoxes, and runs ROBOT report/reason out of band. |
| S7 | Promote coverage from limited corpus to domain/full corpus | `RealityEngine_CI`, `RealityEngine_Machines`, `localOpenClawStack` | Limited profile remains checkable; wider profiles become blocking after drift is eliminated. |

## Milestones

### M1 - Blocking Static ROBOT Gate

Target: selected limited semantic profiles.

Actions:

| Step | Detail |
| --- | --- |
| Install ROBOT in CI | Ensure hosted and local semantic lanes have a deterministic `ROBOT_BIN`. |
| Fail on missing ROBOT in semantic lanes | Keep developer convenience skip locally, but do not allow semantic CI to pass without ROBOT. |
| Preserve artifacts | Store ROBOT report, merged ontology, reasoned ontology, and generated manifest in CI artifacts. |
| Gate selected corpus | Start with the standard deployment corpus and health/OpenClaw/localAI machines. |

Acceptance criteria:

| Criterion | Expected result |
| --- | --- |
| `RealityEngine_CI/scripts/owl-reasoner-check.sh` | Fails if ROBOT is missing in semantic CI. |
| ROBOT report | Zero ERROR rows for selected profiles. |
| ROBOT reason | Produces classified output for selected profiles. |

### M2 - Integration Ontology Vocabulary

Target: semantic representation of MCP/localAIStack and ACP/OpenClaw paths.

Add ontology classes and properties for:

| Concept | Required relationships |
| --- | --- |
| `re:MCPInvocation` | machine, sequence, provider, endpoint, request class, result class. |
| `re:MCPToolResult` | invocation, evidence artifact, confidence, source mapping target. |
| `re:ACPDispatch` | machine, sequence, dispatch id, agent binding, no-wait contract. |
| `re:OpenClawAgentBinding` | machine, agent id, input axes, autonomy mode, completion mapping. |
| `re:CompletionMapping` | source id, offset, length, value domain, allowed writer. |
| `re:SourceMappingWrite` | completion mapping, vector region, writer, correlation id. |
| `re:SemanticGuardrailViolation` | violation type, severity, record id, policy. |

Acceptance criteria:

| Criterion | Expected result |
| --- | --- |
| Ontology report | ROBOT reports no syntax/profile errors. |
| Corpus generation | Existing machine ABoxes continue to generate unchanged semantic identities. |
| Integration examples | At least one MCP and one ACP workflow classify under the new vocabulary. |

### M3 - Static Workflow Provability

Target: prove authored workflows before runtime.

Rules to encode and check:

| Rule | Gate |
| --- | --- |
| Every dispatchable action has a known machine IRI | ROBOT + manifest join. |
| Every MCP/localAIStack endpoint used by PE is allowed for its workflow class | Deterministic profile check plus OWL class membership. |
| Every OpenClaw generated agent has exactly one intended machine binding in the active profile | Deterministic profile check. |
| Every agent input axis maps to an authored machine input axis or semantic bus lane | ROBOT class membership plus generated binding check. |
| Every completion writes only through an approved PE source mapping | Deterministic mapping check and RDF trace class. |
| RED/life-safety actions cannot be downgraded to non-critical automation | OWL classification plus deterministic invariant check. |

Acceptance criteria:

| Criterion | Expected result |
| --- | --- |
| Limited corpus profile | Static semantic check passes. |
| Intentional bad binding fixture | Static semantic check fails. |
| OpenClaw regression profile | `generate-regression-profile.py --check` is required before OpenClaw validation. |

### M4 - Runtime Trace RDF

Target: runtime data paths are reasoner-visible.

Emit or derive RDF/JSON-LD for:

| Runtime event | Required identifiers |
| --- | --- |
| PE source write | source id, source mapping id, offset, length, writer, correlation id. |
| PE push | push id, RE endpoint, selected match algorithm, source records. |
| RE sequence observation | machine IRI, sequence IRI, determination, action code, output region. |
| MCP/localAIStack invocation | provider id, endpoint, allowed operation id, evidence artifact, completion id. |
| ACP/OpenClaw dispatch | dispatch id, agent id, session key hash, no-wait status, completion id. |
| Completion write-back | completion mapping id, values length, vector target, producer, status. |

Acceptance criteria:

| Criterion | Expected result |
| --- | --- |
| Trace export | CI can save a complete RDF/JSON-LD trace for one PE.x.RE.x.PE cycle. |
| Joinability | Every runtime event joins to a machine IRI or an explicit external-provider IRI. |
| Non-blocking PE | Runtime trace export does not add synchronous ROBOT calls to `POST /api/push`. |

### M5 - Dynamic ROBOT Runtime Validation

Target: actual runs are checked against authored semantics.

Validation sequence:

| Step | Command shape |
| --- | --- |
| Run regression profile | `RealityEngine_CI/scripts/regression-test.sh` with selected semantic profile. |
| Export audit traces | Fetch `GET /api/audit/semantics` and integration ledgers from active PE/RE endpoints. |
| Convert to RDF | Produce one runtime trace graph per run id. |
| Merge | ROBOT merge ontology, selected profile ABoxes, and runtime trace graph. |
| Report and reason | ROBOT report/reason fails on semantic errors and classifies runtime events. |
| Deterministic closed-world checks | Validate exact region writes, cardinality, and forbidden endpoint use. |

Acceptance criteria:

| Criterion | Expected result |
| --- | --- |
| MCP/localAIStack trace | Invocation, evidence, completion, and PE write-back classify cleanly. |
| ACP/OpenClaw trace | Dispatch, agent binding, completion, and PE write-back classify cleanly. |
| Guardrail violation fixture | Dynamic validation fails and produces a named violation record. |

## PE.x.MCP/localAIStack Completion Definition

A PE.x.MCP/localAIStack workflow is complete when:

| Requirement | Evidence |
| --- | --- |
| Invocation is allowed | Endpoint and operation are in the static semantic profile. |
| Evidence is attributable | RAG/graph/agent output has provider IRI, correlation id, and source machine IRI. |
| Completion is source-mapped | Completion values target an approved source mapping and vector region. |
| Criticality is preserved | Critical event sequence determination is not weakened by local AI output. |
| Runtime trace reasons cleanly | ROBOT report/reason succeeds on the trace graph and static profile. |

## PE.x.localOpenClawStack Completion Definition

A PE.x.localOpenClawStack workflow is complete when:

| Requirement | Evidence |
| --- | --- |
| Agent binding is current | OpenClaw generated profile matches the selected corpus profile. |
| Dispatch is no-wait | PE records dispatch acceptance and does not block the push cycle for agent execution. |
| Agent axes are aligned | OpenClaw input axes map to machine input axes or semantic bus lanes. |
| Completion is source-mapped | Agent result writes only through the approved PE completion mapping. |
| Runtime trace reasons cleanly | ROBOT report/reason succeeds on the trace graph and static profile. |

## Final Completion Criteria

The semantic OWL framework is complete when all of these are true:

| Area | Required state |
| --- | --- |
| Static coverage | ROBOT is blocking for selected profiles and promotable to full corpus. |
| Dynamic coverage | Runtime traces for PE.x.RE.x.PE, PE.x.MCP.x.PE, and PE.x.ACP.x.PE are RDF-exported and reasoned out of band. |
| API equivalence | C++, Scala, and LSP expose matching semantic identity and audit surfaces for the active profile. |
| Dashboards | Semantic guardrail metrics show manifest availability, audit volume, IRI join ratio, dispatch records, and violation counts. |
| Drift control | Limited corpus and OpenClaw regression profiles are explicit, generated, and checkable. |
| Hot-path safety | ROBOT never runs inside the synchronous PE push or ACP dispatch path. |
