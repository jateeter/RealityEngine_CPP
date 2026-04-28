# Roadmap

The current repository is a self-contained native C++ port focused on behavior
and API compatibility.

## Next Implementation Targets

- Qdrant-backed vector store and vector search parity.
- WebSocket broadcast compatibility.
- Background auto-push scheduler in the Perception Engine service.
- Checkpoint persistence APIs.
- Machine export endpoint.
- Full JSON merge-patch semantics for perception source updates.
- Additional production HTTP hardening, including WebSocket support and
  configurable connection limits.

## Compatibility Principle

`RealityEngine_AI` remains the reference model. Changes to behavior or API shape
should be reflected in E2E corpus tests where possible.
