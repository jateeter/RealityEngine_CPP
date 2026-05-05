# Vector Management

Reality Engine should treat the perceptual input space as a logical coordinate
space, not as a permanently fixed dense vector with one deployment-wide
dimension. `VECTOR_DIMENSION` is a compatibility floor for transport and legacy
payloads; it must not be the source of truth for how large reality can become.

The source of truth is the active machine universe.

## Philosophy

The perceptual vector `En` is a sparse, dynamically expandable address space.
Machines reserve input and output regions inside that address space through
`perceptualMapping`. Adding a machine can extend the address space; removing a
machine can release regions for future packing or compaction. A runtime push can
still be materialized as a dense vector for current APIs, but that dense vector
is only a snapshot projection of the sparse logical space.

This keeps machine interconnection independent from semantic embedding storage.
localAIStack and Ollama operate on fixed-size embedding vectors for RAG and
language-model workflows. Reality Engine operational vectors are different:
they are coordinate-addressed signal lanes with region ownership, overlap, and
merge semantics. They should use separate metadata, collections, and lifecycle
rules so a growing perceptual matrix never forces a localAIStack Qdrant
collection migration.

## Principles

- **Logical dimension is derived.** The minimum required dimension is
  `max(offset + length)` across all active machine input/output regions and
  registered PE sources.
- **Dense vectors are projections.** APIs can continue accepting `number[]`, but
  the engine should internally resize or materialize from sparse regions as
  needed.
- **No truncation for reality vectors.** Runtime perceptual vectors must never be
  truncated to a configured dimension. If a payload is shorter than the required
  dimension, missing values are zeros.
- **Region ownership is explicit.** Every input source and machine output should
  be tracked as a region reservation with owner, purpose, lifecycle, and whether
  overlap is intentional.
- **Packing is a metadata operation.** Repacking moves mappings and source
  reservations; it should preserve connection topology and produce a migration
  record. It should not rewrite localAIStack embedding collections.
- **Embedding stores are isolated.** Qdrant collections used by localAIStack
  retain their configured embedding dimension. Reality Engine runtime snapshots,
  if persisted, use a distinct store strategy.

## Storage Model

Use two storage classes:

| Store | Vector shape | Owner | Use |
| --- | --- | --- | --- |
| Semantic vector store | Fixed embedding dimension | localAIStack/Ollama/RAG | Similarity search over text, tools, memories, and documents. |
| Reality vector store | Sparse regions or versioned dense snapshots | Reality/Perception Engine | Operational state, source signals, machine outputs, and replay/debug snapshots. |

Recommended reality-vector persistence options:

1. Store sparse region documents: `{ spaceVersion, region, values, owner,
   timestamp }`.
2. Store full dense snapshots only for bounded history/debug, with
   `spaceVersion` and `dimension` metadata.
3. If Qdrant similarity search is needed for operational snapshots, create a
   Reality Engine-owned collection named by dimension/version, for example
   `reality_vectors_v<spaceVersion>_<dimension>`. Do not place these points in
   localAIStack embedding collections.

## Dynamic Dimension Lifecycle

1. **Load machines.** Parse all machine `perceptualMapping` entries.
2. **Build region registry.** Track input and output reservations plus PE source
   reservations.
3. **Derive required dimension.** Compute the logical high-water mark from the
   registry.
4. **Resize runtime state.** Grow `PerceptualSpace`, `PreceptionEngine`, and
   `PerceptionEngine::persistentVector` when the high-water mark exceeds the
   current materialized length.
5. **Reject unsafe shrink by default.** Machine removal should release regions
   but should not automatically shrink live vectors unless a controlled compact
   operation is requested.
6. **Compact explicitly.** A compaction pass can repack mappings and sources,
   assign a new `spaceVersion`, and emit a migration map from old coordinates to
   new coordinates.

## Suggested Implementation Path

### Phase 1: Make Dimension Derived

- Add a `VectorSpaceRegistry` to the domain layer. It should expose:
  - `reserve(ownerId, kind, offset, length, allowOverlapReason?)`
  - `release(ownerId)`
  - `requiredDimension()`
  - `connections()` for output-to-input overlap reporting
  - `validate()` for accidental overlap, out-of-range writes, and orphaned
    reservations
- On Reality Engine startup, load machines first, build the registry, and set
  runtime dimension to `max(VECTOR_DIMENSION, registry.requiredDimension())`.
- On machine import, grow the simulator and preception state if the imported
  mapping extends the current dimension.
- On source registration in PE, reserve the source region and grow
  `persistentVector` as needed.

### Phase 2: Stop Truncating Runtime Vectors

- Replace fixed normalization for runtime perceptual vectors with
  `ensureLength(requiredDimension)`.
- Treat shorter push payloads as sparse-with-zero-fill.
- Return `dimension`, `requiredDimension`, and `spaceVersion` in `/api/health`,
  `/api/runtime/metrics`, `/api/machines`, and PE `/api/state`.

### Phase 3: Separate Reality Storage From Embedding Storage

- Keep localAIStack Qdrant collections unchanged.
- Introduce a Reality Engine-owned `RealityVectorStore` abstraction with sparse
  region writes and optional dense snapshot history.
- If Qdrant is reused physically, use separate collection names and explicit
  `spaceVersion` payload metadata. Collection creation/deletion stays under
  Reality Engine ownership, not localAIStack ownership.

### Phase 4: Controlled Repacking

- Promote the existing example-corpus repack process into an API/admin tool:
  inspect, plan, validate, apply.
- Require compaction plans to prove that all intentional input/output overlaps
  are preserved.
- Emit a migration report with old/new ranges, removed holes, and affected
  machines/sources.
- Apply repacks during maintenance windows or under a versioned dual-read mode
  where PE can translate old source coordinates into the new space.

## Compatibility Guidance

For now, `VECTOR_DIMENSION` should remain as a compatibility floor and default
dense projection size. New deployments should set it at or above the active
registry requirement, but the runtime should not depend on manual configuration
once Phase 1 is implemented.

localAIStack compatibility sensors remain ordinary PE source reservations. Their
regions can participate in the dynamic registry without changing localAIStack's
embedding dimension or Ollama model configuration.
