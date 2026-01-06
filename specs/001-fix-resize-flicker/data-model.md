# Data Model: Fix Resize Flicker

**Feature**: Window Resize Without Flicker  
**Status**: N/A - No Data Entities

## Summary

This feature does not introduce or modify any data entities. The fix is purely behavioral, affecting how the rendering viewport handles window resize events. No persistent data structures, database schemas, or serializable entities are involved.

## Rationale

Window resize handling is an ephemeral, real-time UI operation that:
- Does not create or modify stored data
- Does not introduce new domain entities or value objects
- Operates solely on transient rendering state (frame buffers, viewport dimensions)
- Does not require data persistence, serialization, or migration

## Affected State (Transient, Non-Persisted)

For reference, the following transient state variables are involved in resize handling:

| State Variable | Type | Purpose |
|----------------|------|---------|
| `m_renderWindowSize_px` | `Vector2` | Current viewport dimensions (ephemeral) |
| `m_dirty` | `bool` | Render invalidation flag (ephemeral) |
| `m_renderWindowState.isMoving` | `bool` | Camera/viewport movement flag (ephemeral) |
| `m_asyncCurrentEpoch` | `atomic<uint64_t>` | Async job versioning (ephemeral) |

None of these are persisted or constitute data entities in the domain model sense.

---

**Conclusion**: No data model documentation required for this feature.
