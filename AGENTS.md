# Codex Guidance: RealityEngine_CPP

Read `claude.md` for the current codebase map and parity context.

## Role

This repo contains the C++20 Reality Engine and Perception Engine implementation. It is a parity target against LSP and Scala.

## Development Rules

- Use `make all`, not `make build`.
- Treat RE routes, PE routes, machine loading, MQTT mapping, and PE source state as cross-engine contracts.
- Keep corpus loading pointed at `../RealityEngine_Machines/machines` unless a test intentionally overrides it.
- Use structured JSON handling rather than ad hoc string manipulation.

## Bug Triage

- For startup or empty registry issues, inspect `start.sh`, `src/reality_engine_server.cpp`, `src/reality.cpp`, and `src/perception_engine_server.cpp` together.
- For parity drift, compare `/api/machines`, `/api/engine/active`, `/api/perceive`, and `/api/pe/state` against LSP and Scala.
- For integration failures, separate RE state, PE state, MQTT mapping, and OpenClaw/localAI source activation.

## Verification

Common commands:

```bash
make all
make test
make e2e
make e2e-services
make e2e-healthkit-spezi
```

## Artifact Hygiene

Do not commit binaries, generated logs, runtime state, local captures, or generated compile databases unless explicitly requested.

