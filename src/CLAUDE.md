# RealityEngine_CPP Source Guidance

This directory contains the C++ RE/PE implementation.

- Treat RE and PE route behavior as cross-engine parity code.
- Keep machine loading and source activation behavior aligned with LSP and Scala.
- Use `clangd` and build with `make all`.
- For startup failures, inspect server entrypoints and `reality.cpp` before chasing higher-level tests.

## Ollama `format` (structured output) — three-engine parity

Whether a native Ollama dispatch sends `format` depends on the model: Ollama
refuses it for some (`nemotron-3-nano` → `structured output is unavailable`).
The C++, LSP and Scala PEs and the Manager TypeScript PE must send or omit it
**identically** for every model — a divergence reads as an engine defect in the
parity sweep. The rule and the model list are canonical in
`RealityEngine_CI/docs/OLLAMA_INTEGRATION.md` § "Structured output"; here it is
`ollama_model_supports_structured_output` (`perception_engine_server.cpp`) with the `formatJson` body switch. When `format` is omitted the engine instead
puts the mapping's required keys and an example shape in the system prompt
(`completion_contract_prompt`, byte-identical text in all four) and, for these
models, sends `think: false`. A change to the list or the rule lands in all four
repos together.

## Standing rules

The standing rules are in
`../../RealityEngine_CI/docs/ENGINEERING_CONTRACT.md`, which is
authoritative. They are not listed or summarised here, so this file never
falls behind the contract. Read it before changing anything here.
