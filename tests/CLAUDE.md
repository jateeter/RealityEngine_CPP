# RealityEngine_CPP Tests Guidance

This directory contains C++ unit, integration, and e2e tests.

- Use `make test` for local logic and `make e2e` for runtime/corpus/API behavior.
- Keep OpenClaw, MQTT, corpus loading, and byte-equivalence failures reported separately.
- Prefer explicit endpoint/env setup when tests talk to live services.

