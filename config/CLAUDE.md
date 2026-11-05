# RealityEngine_CPP Config Guidance

This directory contains runtime and integration config for the C++ engine.

- Keep defaults aligned with the root application map.
- Preserve compatibility with CI-generated `INTEGRATIONS_CONFIG`.
- Use JSON schema support where applicable.
- Treat config changes as parity-sensitive when they affect RE/PE endpoints or source mapping.

## Standing rules

The standing rules are in
`../../RealityEngine_CI/docs/ENGINEERING_CONTRACT.md`, which is
authoritative. They are not listed or summarised here, so this file never
falls behind the contract. Read it before changing anything here.
