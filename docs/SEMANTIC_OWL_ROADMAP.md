# Semantic OWL Framework Roadmap

Last reviewed: 2026-09-16

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

### M2 - Integration Ontology Vocabulary - COMPLETE 2026-09-16

Target: semantic representation of MCP/localAIStack and ACP/OpenClaw paths.

Delivered in `RealityEngine_Machines#148`. Ontology `0.3.1 -> 0.4.0`.

M2 stood at one of its seven concepts, and that count was generous:
`re:CompletionMapping` was declared and its extension was **empty across all
twelve domains**. Nothing in the corpus described the route a completion takes
into the vector; the ontology only said such a route could be described.

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

| Criterion | Expected result | Measured 2026-09-16 |
| --- | --- | --- |
| Ontology report | ROBOT reports no syntax/profile errors. | **0 ERROR, 0 WARN, 0 INFO** |
| Corpus generation | Existing machine ABoxes continue to generate unchanged semantic identities. | `generate-owl.py --all --check` clean, **0 files changed** |
| Integration examples | At least one MCP and one ACP workflow classify under the new vocabulary. | HermiT classifies both; merged by `reason-owl.sh` in every scope, so it is gated rather than asserted |

Gates: `npm run owl:reason:corpus` and `owl:reason:arbiter` both OK under ELK +
HermiT; contracts 186 passed (was 176); `npm run validate` 1328 machines, 0
invalid. The corpus-wide merge (~18 min) was not run.

#### One axiom was removed rather than written

The invariant "every source mapping write exercises a declared completion
mapping" was first encoded in OWL, as a defined class over
`owl:maxCardinality 0`. It was **inert** — under the open world a write with no
asserted mapping is not a write with zero mappings, so nothing is entailed, and
HermiT classified the negative fixture as nothing. An empty extension and an
unsatisfiable class are indistinguishable from outside.

Same shape as `re:EscalationDetermination`, which was correct, open-world, and
evaluated 2 of 80 escalations. Same remedy: the OWL stays open-world and the
closed-world half moved to
`RealityEngine_Machines/tests/contracts/integration_vocabulary_test.py`.

This roadmap already assigns that check to M3 ("Deterministic mapping check")
and M5 ("Deterministic closed-world checks"), so nothing moved milestone. It is
recorded because an OWL class that appeared to do the job would have made M2
look finished while checking nothing.

#### Carried into M3

- `generate-owl.py` emits no `re:CompletionMapping` individuals, so the corpus's
  own completion routes remain undescribed — the two in
  `semantics/integration/examples.ttl` are the only ones in the repository.
  Generator work, and the natural first step of M3's "every completion writes
  only through an approved PE source mapping".
- `re:ResponseMapping` is the remaining declared class with an empty extension.

### M3 - Static Workflow Provability - COMPLETE 2026-09-16

Target: prove authored workflows before runtime.

Delivered in `RealityEngine_Machines#150` as `scripts/prove-workflows.py`, which
implements all six rules over a named corpus profile and emits findings as
`re:SemanticGuardrailViolation` individuals - the M2 class whose extension had
been empty since it was declared.

Five of the six rules are **absence** checks ("every X *has* a known Y"), which
is exactly what OWL's open world will not conclude, so they are deterministic
and closed over a profile. This roadmap already says so per rule. The reasoner
keeps classification and the RED contradiction `re:EscalationDetermination`
catches.

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

| Criterion | Expected result | Measured 2026-09-16 |
| --- | --- | --- |
| Limited corpus profile | Static semantic check passes. | `standard-deployment`, 12 machines, **0 violations** |
| Intentional bad binding fixture | Static semantic check fails. | **5 fixtures**, each producing exactly its expected violation |
| OpenClaw regression profile | `generate-regression-profile.py --check` is required before OpenClaw validation. | **now required**, see below |

Gates: `npm run prove:workflows`, `npm run prove:workflows:fixtures`, both
required by `tests/contracts/static_provability_test.py` and
`openclaw_profile_drift_test.py`. Contracts 197 passed (was 186); `npm run
validate` 1328 machines, 0 invalid.

#### Criterion 3 was not met, and not for the reason it looked like

`generate-regression-profile.py` has carried `--check` since it was written, and
its docstring says "Run it in CI and after any corpus change". A search across
every shell script, workflow and test in the workspace found **nothing that
calls it**. It was available, not required - a weaker thing that reads the same
in a status table.

It now runs in the contract suite of `RealityEngine_Machines`, the repository
that owns the corpus the profile is derived from and therefore the place the
profile goes stale.

#### Two rules were wrong before their numbers meant anything

Recorded because each produced a plausible result that was false.

R6 first required `re:EscalationAction`, and reported all five RED
determinations in the limited profile as violations - all five prescribe
`re:RouteReferral` or `re:RouteReview`, which are notifications and do reach a
person. The bar is this roadmap's own wording: "downgraded to non-critical
*automation*".

R6 then attributed a RED trigger rule to every determination of its sequence.
`AICapacityThrottler` carries seven rules over seven determinations, two RED and
five AMBER; that reported nominal outputs as unactioned RED and inflated the
corpus result to 567. **The join does not exist in the ABox**:
`re:matchesOutputPosition` indexes the machine's output value vector, not the
sequence's determination list. R6 now reads only what is asserted and reports
the reach limit - 625 machines state RED only on a trigger rule - as an explicit
ungated line.

#### What the six rules found, and what is gated

Corpus-wide (1328 machines) R6 evaluates 80 critical determinations - matching
the 80 escalating output events `escalation_rag_test.py` documents - and finds
13 violations: 8 critical determinations prescribing no action at all, and 5
life-safety determinations prescribing only `re:DispatchAgent`, which
`re-core.ttl` defines as having "no direct human recipient". Tracked in
`RealityEngine_Machines#149`, with the classification question left open.

**The gate is the limited profile only.** Gating the full corpus would either
block unrelated changes or invite the rule to be widened until the corpus
passed. That split is S7's: "Limited profile remains checkable; wider profiles
become blocking after drift is eliminated."

#### Still ungated, and reported as such

- R2's workflow-class half has no source of truth: no `integrations.json` entry
  declares `allowedOperations`, so endpoint membership is checked and
  per-workflow-class operation permission is not.
- R6 cannot reach the 625 trigger-rule-only machines. Joinability is M4's
  problem, and M4 needs it anyway for runtime traces.

### M4 - Runtime Trace RDF - COMPLETE 2026-09-16

Target: runtime data paths are reasoner-visible.

Delivered in `RealityEngine_Machines#151` as `scripts/export-runtime-trace.py`.
Ontology `0.4.0 -> 0.5.0`.

**The join was already there.** The engines emit `GET /api/audit/semantics`
records whose `machineIri`, `sequenceIri`, `stepIri` and `determinationIri` are
byte-identical to the IRIs `generate-owl.py` writes into the ABox, so the second
criterion was a thing to verify rather than a thing to build. The corpus join
goes through `machineName`, never `machineId`: ids are re-minted per runtime, so
an id-keyed trace measures the runtime rather than the corpus.

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

| Criterion | Expected result | Measured 2026-09-16 |
| --- | --- | --- |
| Trace export | CI can save a complete RDF/JSON-LD trace for one PE.x.RE.x.PE cycle. | **1440 events**, 14,521 lines of Turtle + JSON-LD, merges and reports clean under ROBOT |
| Joinability | Every runtime event joins to a machine IRI or an explicit external-provider IRI. | **1440/1440** |
| Non-blocking PE | Runtime trace export does not add synchronous ROBOT calls to `POST /api/push`. | **zero** reasoner references across all four runtime source trees; push ~1.2s |

Gates: `npm run trace:export`, `npm run trace:check`, both required by
`tests/contracts/runtime_trace_test.py`. Contracts 205 passed (was 197).

Criterion 3 holds by construction rather than by discipline - the exporter reads
finished surfaces over HTTP and is not linked into any runtime, so it cannot be
on the push path. Its test is the one M4 test that never skips; the other two
describe a run and skip with a reason when no universe is up, which must not
take the hot-path guard down with them.

#### Three identifiers the exporter refuses to invent

A push id, a correlation id on PE source writes, and the output region on a
sequence observation. No runtime surface emits any of them. They are reported as
counted gaps rather than synthesised: a generated push id would look exactly
like a real one and would join two events that nothing actually connected.
Closing them is engine work, and M5 wants them.

#### A finding: 7 machines are live but not in the corpus

`localai/agent_activity_classifier` and six siblings are loaded by the engine
and have no ABox - localAIStack injects them at runtime rather than the corpus
authoring them. They are attributed to an explicit `re:IntegrationProvider` IRI,
which is what this criterion names second, and counted as a gap so the number
stays visible.

#### Not traced in this deployment

Dispatches ran in dry-run mode, so no completion write-back existed to trace,
and no MCP invocation was recorded during the traced cycle. Those two event
kinds have vocabulary and fixtures but no live evidence yet.

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
