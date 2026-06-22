# RealityEngine_CPP Source Guidance

This directory contains the C++ RE/PE implementation.

- Treat RE and PE route behavior as cross-engine parity code.
- Keep machine loading and source activation behavior aligned with LSP and Scala.
- Use `clangd` and build with `make all`.
- For startup failures, inspect server entrypoints and `reality.cpp` before chasing higher-level tests.

