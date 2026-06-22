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
- Keep `/api/machines`, `/api/engine/active`, `/api/perceive`, `/api/pe/*`, and MQTT behavior aligned with LSP and Scala.
- OpenClaw/ACP environment defaults should match the root application map.

## LSP Support

Use `clangd` for C++20. The Makefile is the build source of truth. Generate `compile_commands.json` only when needed for navigation, and keep generated files uncommitted unless requested.

## Editing Rules

- Inspect `start.sh`, `src/reality_engine_server.cpp`, `src/reality.cpp`, and `src/perception_engine_server.cpp` together for startup issues.
- Run `make test` for local logic changes and `make e2e` for corpus/API changes.
- Do not commit binaries, logs, generated reports, or runtime state.
