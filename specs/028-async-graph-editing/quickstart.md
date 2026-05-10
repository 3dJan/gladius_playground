# Quickstart: Async Graph Editing

**Feature**: 028-async-graph-editing  
**Date**: 2026-04-15

## Overview

This feature makes structural graph edits (add/delete nodes, create/delete links, paste, extract-to-function) non-blocking by splitting the update pipeline into a fast UI phase and a background processing phase.

## Architecture at a Glance

```
┌─ UI Thread (< 16 ms per edit) ──────────────────────────────┐
│  1. Apply visual edit to Assembly (add node, create link)    │
│  2. Capture undo snapshot (History::storeState)              │
│  3. Increment structural edit epoch                          │
│  4. Set debounce pending flag                                │
│  5. [After debounce] Deep-copy Assembly → snapshot           │
│  6. [After debounce] Dispatch StructuralUpdateJob            │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─ Background Worker ─────────────────────────────────────────┐
│  validate → updateTypes → updateInputsAndOutputs →          │
│  updateParameterRegistration → flatten → codegen →          │
│  compile → SDF → bbox                                       │
│  (all on snapshot, checks epoch for early cancellation)     │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─ UI Thread (result consumption) ────────────────────────────┐
│  Check epoch freshness → swap types → apply validation →    │
│  invalidate render                                          │
└──────────────────────────────────────────────────────────────┘
```

## Key Changes from Current Architecture

| Current (blocking) | New (async) |
|---------------------|-------------|
| `updateTypes()` on UI thread | Deferred to background worker |
| `updateInputsAndOutputs()` on UI thread | Deferred to background worker |
| `updateParameterRegistration()` on UI thread | Deferred to background worker |
| Assembly deep copy for undo on UI thread | Undo snapshot still on UI (lightweight per-Model), worker snapshot at dispatch |
| `refreshModelAsync()` guard blocks on compilation | Guard checked on background thread, not UI |

## Key Design Decisions

1. **Snapshot isolation** (not fine-grained locking): Background worker gets an independent Assembly copy. No concurrent access to shared mutable state.

2. **Epoch-based cancellation**: Reuses the `AsyncRenderController` pattern. Stale workers exit early at checkpoints.

3. **Debounce coalescing**: 50 ms debounce timer coalesces rapid edits into single background cycles. Similar to existing `m_parameterThrottle`.

4. **Stale types are OK**: Between edit and background completion, node ports show stale/default types. Updated silently when results arrive.

5. **Undo is synchronous**: Undo snapshot captured immediately on UI thread before the edit. Background cancellation on undo is automatic via epoch increment.

## Files to Modify

| File | Change |
|------|--------|
| `gladius/src/Document.h/.cpp` | Add `StructuralEditEpoch`, debouncer, result slot; refactor `refreshWorker()` to accept snapshot |
| `gladius/src/ui/MainWindow.cpp` | Remove sync `updateInputsAndOutputs()` and `updateParameterRegistration()` from `nodeEditor()`; add debounce dispatch and result consumption in frame loop |
| `gladius/src/ui/ModelEditor.cpp` | Remove sync `updateTypes()` from structural change path; keep for parameter-only fast path |
| `gladius/src/nodes/Assembly.h/.cpp` | No structural changes needed (copy constructor already works) |
| `gladius/src/nodes/Model.h/.cpp` | Ensure `updateTypes()` results can be read back by UI thread after background completion |
| `gladius/src/nodes/History.h/.cpp` | No changes needed if keeping full Assembly snapshot for undo |

## New Files

| File | Purpose |
|------|---------|
| `gladius/tests/StructuralUpdatePipelineTests.cpp` | Tests for epoch cancellation, coalescing, result consumption |
| `gladius/tests/AssemblySnapshotTests.cpp` | Tests for snapshot independence (edits to original don't affect copy) |

## Testing Strategy

- **Unit tests**: Epoch increment/comparison, debounce timing, snapshot isolation, result consumption
- **Integration tests**: Add node → verify background compilation triggers → verify types update after completion
- **Performance tests**: Measure UI thread frame time during rapid structural edits on 100+ node model
- **Regression tests**: Undo/redo with concurrent background processing; auto-compile toggle behavior
