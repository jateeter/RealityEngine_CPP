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
