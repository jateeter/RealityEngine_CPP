# Documentation Index

This is the maintained documentation set for RealityEngine_CPP.

## Core

| Document | Purpose |
| --- | --- |
| [../README.md](../README.md) | Project overview, build, run, and scope. |
| [../MACHINE_CONCEPT.md](../MACHINE_CONCEPT.md) | Canonical machine model — DFA theory, JSON schema, perceptual mapping, regex equivalences, STA. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Visual native architecture overview. |
| [API_EQUIVALENCE.md](API_EQUIVALENCE.md) | API parity against the active Scala replacement surface and other runtimes. |
| [INTEGRATION_ARCHITECTURE.md](INTEGRATION_ARCHITECTURE.md) | Provider-neutral PE integration, trigger dispatch, source mapping, and MCP architecture. |
| [SEMANTIC_OWL_ANALYSIS.md](SEMANTIC_OWL_ANALYSIS.md) | Current OWL/ROBOT semantic coverage analysis for PE.x.MCP and PE.x.ACP flows. |
| [SEMANTIC_OWL_ROADMAP.md](SEMANTIC_OWL_ROADMAP.md) | Roadmap for static and dynamic ROBOT validation across semantic workflows. |
| [VECTOR_MANAGEMENT.md](VECTOR_MANAGEMENT.md) | Dynamic perceptual-vector strategy. |
| [ACRONYMS.md](ACRONYMS.md) | Acronym definitions. |
| [BIBLIOGRAPHY.md](BIBLIOGRAPHY.md) | External and project references. |

## Suite Repositories

| Repository | Role |
| --- | --- |
| [RealityEngine_AI](../../RealityEngine_AI) | Locked historical reference; replaced by RealityEngine_Scala and not an active runtime target or corpus source. |
| [RealityEngine_CPP](..) | Native C++ RE/PE runtime. |
| [RealityEngine_LSP](../../RealityEngine_LSP) | Common Lisp RE/PE runtime. |
| [RealityEngine_Scala](../../RealityEngine_Scala) | Scala RE/PE runtime surface. |
| [RealityEngine_Machines](../../RealityEngine_Machines) | Active machine corpus, contracts, oracles, and trigger examples. |
| [RealityEngine_Manager](../../RealityEngine_Manager) | Orchestration and visualization frontend built against the canonical HTTP surface. |
| [RealityEngine_CI](../../RealityEngine_CI) | Deployment, compatibility, and CI tooling for the suite. |

## Operations

| Document | Purpose |
| --- | --- |
| [OPERATIONS.md](OPERATIONS.md) | Startup, shutdown, environment variables, and Qdrant ownership. |
| [LOCAL_AI_INTEGRATION.md](LOCAL_AI_INTEGRATION.md) | localAIStack bridge and guarded invocation endpoints. |
| [MQTT_YUMA_DEMONSTRATION.md](MQTT_YUMA_DEMONSTRATION.md) | End-to-end MQTT integration walk-through against `yuma.lateraledge.cloud:1883`. |
| [E2E_TESTING.md](E2E_TESTING.md) | Corpus, domain, and service-boundary validation. |
| [HEALTHKIT_SPEZI_BRIDGE.md](HEALTHKIT_SPEZI_BRIDGE.md) | Recommended SpeziHealthKit iOS bridge contract for PE HealthKit ingest. |

## API Contracts

| Document | Purpose |
| --- | --- |
| [openapi/reality-engine.yaml](openapi/reality-engine.yaml) | Reality Engine OpenAPI contract. |
| [openapi/perception-engine.yaml](openapi/perception-engine.yaml) | Perception Engine OpenAPI contract. |
