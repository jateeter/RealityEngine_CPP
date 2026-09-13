# RealityEngine_CPP Guidance

Last reviewed: 2026-06-22

See `/Users/johnt/workspace/GitHub/claude.md` for the integrated application map. Update both this file and the root map when C++ engine ownership, startup behavior, API parity, or PE integration changes.

## Role

This repo contains the C++20 Reality Engine and Perception Engine implementations. It participates as `cpp-1` in multi-engine runs and is a primary parity target against LSP and Scala.

## Codebase Map

- `src/reality_engine_server.cpp`: RE HTTP server entrypoint.
- `src/perception_engine_server.cpp`: PE HTTP server entrypoint.
- `src/reality.cpp`: machine loading and core runtime behavior.
- `src/http.cpp`: HTTP routing/support.
- `src/mqtt_bridge.cpp`, `src/mqtt_client.cpp`, `src/mqtt_mapping.cpp`: MQTT integration.
- `src/sta_checker.cpp`: state/temporal analysis support.
- `include/reality/`: public headers.
- `config/`: runtime defaults and integration config.
- `tests/`: unit, integration, and e2e coverage.
- `docs/openapi/`: generated API documentation.
- `scripts/`, `tools/`: support utilities.

## Building

**Builds are controlled through `RealityEngine_CI`, not from here.** This repo is
an independent git repository, not a subproject of CI or of any other engine.
Read the contract before building or deploying:

    RealityEngine_CI/docs/BUILD_CONTROL_CONTRACT.md

```bash
cd ../RealityEngine_CI && ./scripts/regression-test.sh --build-only
```

The per-repo commands below are for working on this repository alone — never as
the last step before a parity claim, which the provenance gate will refuse
anyway if the artifact does not match the source.

## Key Commands

```bash
make all
make test
make e2e
make e2e-services
make e2e-healthkit-spezi
./start.sh
./stop.sh
```

Use `make all`, not `make build`.

## Runtime Contract

- The machine corpus should usually load from `../RealityEngine_Machines/machines`.
- Startup, corpus loading, and PE source state are common causes of parity drift.
- **Machine ingestion and PE source membership** are governed by the canonical
  contract, not by this file. See "## Machine ingestion" below.
- Keep `/api/machines`, `/api/engine/active`, `/api/perceive`, `/api/pe/*`, and MQTT behavior aligned with LSP and Scala.
- OpenClaw/ACP environment defaults should match the root application map.

## Machine ingestion

Governed by the canonical contract, which lives in `RealityEngine_CI` and
nowhere else:

    RealityEngine_CI/SURFACE_SPEC.md  §  Machine ingestion

Do not restate it here. It defines what ingesting a machine interns, how
`PE_SOURCE_BOOTSTRAP` gates it, and how those sources compose `ISRESeed(n)` —
and it governs this repository's implementation of all three.

Implemented in `perception_engine_server.cpp`: `source_bootstrap_env` parses the
flag, `bootstrap_test_sources_from_reality` interns at boot. Note #40 asked for
the opposite default and was implemented before #46 inverted it — the flag
handling and the read-path removal from #40 both stand; only the default moved.

## Editor tooling

Use `clangd` for C++20. The Makefile is the build source of truth. Generate `compile_commands.json` only when needed for navigation, and keep generated files uncommitted unless requested.

## Editing Rules

- Inspect `start.sh`, `src/reality_engine_server.cpp`, `src/reality.cpp`, and `src/perception_engine_server.cpp` together for startup issues.
- Run `make test` for local logic changes and `make e2e` for corpus/API changes.
- Do not commit binaries, logs, generated reports, or runtime state.

## MUST: every use of the word "registry" carries a qualifier

**The word "registry" MUST NEVER appear unqualified. Every single use of the
word takes a qualifier naming which registry is meant.**

This is a hard requirement, not a style preference. It applies to every
occurrence in every context, with no exceptions: prose, end-of-task summaries,
commit messages, PR bodies, issue titles and bodies, code comments, docstrings,
variable and function names, log lines, and documentation.

Wrong, in every case — these are all violations:

- "the registry"
- "a versioned registry"
- "the registry file" / "update the registry" / "registry-backed"
- "check the registry first"
- "registry drift"

Right — a qualifier every time:

- "the **instance** registry"
- "a versioned **cesgen** registry"
- "the **arbitration** registry"
- "**machine** registry drift"

If you type the word "registry" and the word immediately before it is not a
qualifier, stop and add one. Re-read every summary and every message for the
bare word before sending it — that is where this rule is actually broken, because
the surrounding context makes the referent feel obvious in the moment. That
feeling is exactly the assumption the rule exists to block.

Qualifiers currently in use. **This list is open, not exhaustive** — a registry
added later gets a qualifier too; nothing is ever promoted to being "the
registry" by virtue of being the one under discussion:

- **instance** registry — `/tmp/re-registry/re-registry.json`, served at
  `:5999/re-registry.json`. Running RE/PE instances with `re_url`/`pe_url`/ports,
  plus `services` and `allocation`. What `RE_REGISTRY_URL` points at.
- **machine** registry — the machines a runtime holds in memory, reported by
  `GET /api/machines`. Distinct from `GET /api/machines/json/list`, the on-disk
  corpus catalog.
- **cesgen** registry — `RealityEngine_Machines/domains/ces-contract-registry.json`.
  Which CES output-stream contract shards exist, what corpus each was recorded
  against, whether each is current.
- **arbitration** registry — `machines/domains/arbitration-registry.json`.
- **domain** registry — `machines/domains/domain-registry.json`.
- **semantic-bus** registry — `machines/domains/semantic-bus-registry.json`.
- **tag** registry — `RealityEngine_CI/docs/TAG_REGISTRY.md`.

## MUST: verify a merge beyond the hosted checks

**A green PR is not a verified PR. Never merge on the hosted checks alone.**

The hosted path does not exercise this system's integration points. A PR can show
every check green and still be unverified, because the checks that ran were a
security scan and — at most — a corpus gate. `localAIStack`, `localOpenClawStack`,
Ollama, Qdrant, MQTT, the OpenClaw ACP gateway and the multi-engine universe are
**not** reachable from the hosted runners, so nothing on that path can tell you
whether the change works where it has to work.

Observed repeatedly: RealityEngine_Machines PRs report exactly one check
(GitGuardian). That is not evidence about the corpus, the registries, the
engines, or any bridge.

Before merging, verify **locally**, and say in the PR which of these you ran and
what they returned:

- The repo's own gates — `validate-corpus.sh`, the contract suite,
  `npm test`, `make test`, `sbt test` — whichever the change touches.
- The integration points the change can reach: a live 3-of-3 universe, the
  local AI stack, the OpenClaw gateway, MQTT — whichever the change can affect.
- The specific behaviour the change claims, with the numbers it produced.

If an integration point cannot be exercised, **say so in the PR** and name it.
An unverified area that is named is a known gap; an unverified area that is
silent reads as tested.

A hosted green tells you the change did not break the hosted path. That is worth
having and is not the question being asked at merge time.
