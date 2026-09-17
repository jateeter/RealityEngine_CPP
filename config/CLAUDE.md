# RealityEngine_CPP Config Guidance

This directory contains runtime and integration config for the C++ engine.

- Keep defaults aligned with the root application map.
- Preserve compatibility with CI-generated `INTEGRATIONS_CONFIG`.
- Use JSON schema support where applicable.
- Treat config changes as parity-sensitive when they affect RE/PE endpoints or source mapping.

