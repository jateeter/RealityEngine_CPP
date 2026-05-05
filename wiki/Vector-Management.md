# Vector Management

Reality Engine treats the perceptual input space as a logical coordinate space,
not as a permanently fixed dense vector. `VECTOR_DIMENSION` is a compatibility
floor for transport and legacy dense projections; the active machine universe
and Perception Engine source reservations should define the required dimension.

The practical rule is:

```text
runtimeDimension = max(configured VECTOR_DIMENSION, max(offset + length))
```

where `max(offset + length)` is computed across active machine input/output
regions and registered PE sources.

## Storage Boundary

localAIStack and Ollama use fixed-size semantic embedding vectors for RAG and
model workflows. Reality Engine operational vectors are coordinate-addressed
signals with region ownership, intentional overlap, and merge semantics.

Do not store dynamically growing reality vectors in localAIStack embedding
collections. If operational state must be persisted, use either sparse region
records or Reality Engine-owned versioned snapshot collections.

## Implementation Path

1. Add a vector-space registry for machine and source reservations.
2. Derive the required logical dimension from that registry.
3. Grow PE and RE runtime vectors when machines or sources extend the
   high-water mark.
4. Zero-fill shorter dense payloads; never truncate runtime perceptual vectors.
5. Keep localAIStack Qdrant collections unchanged.
6. Repack only through explicit, versioned maintenance operations that preserve
   existing machine interconnections.
