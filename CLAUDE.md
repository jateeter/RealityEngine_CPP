# RealityEngine_CPP Guidance

Last reviewed: 2026-09-25

See `/Users/johnt/workspace/GitHub/CLAUDE.md` for the integrated application map. Update both this file and the root map when C++ engine ownership, startup behavior, API parity, or PE integration changes.

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
- `scripts/`: support utilities (`smoke-test.sh`).

## Building

**Builds are controlled through `RealityEngine_CI`, not from here.** This repo is
an independent git repository, not a subproject of CI or of any other engine.
Read the contract before building or deploying:

    RealityEngine_CI/docs/BUILD_CONTROL_CONTRACT.md

```bash
cd ../RealityEngine_CI && ./scripts/regression-test.sh --execute --build-only
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

## Step completion point (RealityEngine_CI#375)

A step composes in parallel and resolves atomically: every machine's composition
runs as a `std::future` on the domain worker pool, `run_phases` joins every one
(`futures[i].get()`) before resolution, and OSRE(n) is committed once. The
committed (ISRE, OSRE) pair is published through `PerceptualSpaceRuntime::onStepCommitted`
to the server, which records `completedStep` and `notify_all`s a
`std::condition_variable` under `spaceRuntimeMutex` (the lock every step runs
under). `GET /api/engine/steps/:n/pair?timeoutMs=` waits on it with `wait_for`,
which releases the lock while waiting. The two history routes read under the same
lock. Steps are numbered from 0.

## Arbitration retention and the instance clock (RealityEngine_CI#296)

`onStepCommitted` also ticks the instance's Lamport clock and, with
`arbitrationRetention` on, keeps the step's records (`last_arbitration()`) in
`arbitrationSteps` under its step number and Lamport value, before the
completion is published. A reset (-1) drops them; the clock keeps its value.
`GET /api/arbitration` answers the legacy object while retention is off (its
bytes unchanged) and the window's steps as a list while it is on (`?step=N`
reads one; 404/410/409/400), records by cell and contributions by
`(provider, originId, cesId, outputVectorId)`. Controls `arbitrationRetention`
(default `false`) and `arbitrationWindow` (default 1, max 1024).

The clock is `{instance, lamport, step}` (`GET /api/engine/clock`,
`InstanceClock` in `src/instance_clock.cpp`). A UUID belongs to an **instance**,
never an engine type or image: CI allocates it (`INSTANCE_UUID`); without one
the server mints a v7 UUID at boot. `lamport` ticks once per committed step and
never resets. An allocated instance keeps `<uuid>.lamport` in
`INSTANCE_CLOCK_DIR` (default `~/.reality-engine/clock/`) — a high-water mark
reserved 1024 ahead, written (temp + rename) before any tick past it — and holds
`lockf` on `<uuid>.lock` for its life, so a second live process with the same
UUID refuses to start. An unreadable or unwritable clock refuses the start too.

## Standing rules — authoritative in `../RealityEngine_CI/docs/ENGINEERING_CONTRACT.md`

These apply here and are **not** restated in this file. The table is an index
to the contract, not a copy of it: it names every rule so you know what to look
up, and the contract's wording governs wherever the two differ.

| Rule | In short |
| --- | --- |
| Qualify every "registry" | Never the bare word — instance / machine / cesgen / arbitration / domain / semantic-bus / tag. |
| Regenerate a stale `<name>` registry, don't fail it | Each `<name>` registry is a view of the running system. A gate regenerates it and fails only on a disagreement that survives regeneration. |
| Verify a merge beyond the hosted checks | A green PR is not a verified PR; the hosted path cannot reach the integration points. Name what you could not exercise, and record what you noticed but did not chase. |
| _CI is the authority | Peripheral repos keep minimal CI that forces local validation; RealityEngine_CI verifies fixes against a live universe. Check its `docs/` before adding CI anywhere else. |
| Name it `CLAUDE.md` | Uppercase, always. On a case-insensitive filesystem `claude.md` is the same inode; dedupe on `st_ino`, never on a resolved path. |
| Never commit to main | Branch from `origin/main`, PR, verify, squash-merge, clean up. |
| Use bash, not zsh | Shell work runs in `/opt/homebrew/bin/bash` (5.x), not zsh or macOS `/bin/bash` 3.2: any loop, unquoted variable, glob or `set --` goes through it with `set -euo pipefail`, and you check the command's exit status, not the pipeline tail. |

Read the contract for the full text, the qualifier table, and the cleanup steps.
