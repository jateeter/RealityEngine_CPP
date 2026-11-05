# RealityEngine_CPP Tests Guidance

This directory contains C++ unit, integration, and e2e tests.

- Use `make test` for local logic and `make e2e` for runtime/corpus/API behavior.
- Keep OpenClaw, MQTT, corpus loading, and byte-equivalence failures reported separately.
- Prefer explicit endpoint/env setup when tests talk to live services.

## Standing rules

The standing rules are in
`../../RealityEngine_CI/docs/ENGINEERING_CONTRACT.md`, which is
authoritative. They are not listed or summarised here, so this file never
falls behind the contract. Read it before changing anything here.
