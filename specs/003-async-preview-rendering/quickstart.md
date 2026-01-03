# Quickstart: Async Preview Rendering

**Feature**: 003-async-preview-rendering  
**Status**: Planning Complete

## Overview

This document provides a quick reference for implementing async preview rendering.

## Current State (Synchronous)

```
┌────────────────────────────────────────────┐
│ UI Thread (blocks during preview render)   │
├────────────────────────────────────────────┤
│ renderWindow()                             │
│   └─▶ renderSync()                         │
│         └─▶ m_core->renderLowResPreview()  │  ◀── Blocks 30-100ms
│               ├─▶ renderScene()            │
│               ├─▶ resample()               │
│               ├─▶ glFinish() ◀── GPU sync  │
│               └─▶ bind()     ◀── Pixel xfer│
│   └─▶ displayFrame()                       │
└────────────────────────────────────────────┘
```

## Target State (Asynchronous)

```
┌─────────────────────────┐     ┌─────────────────────────┐
│ UI Thread (<1ms/frame)  │     │ Worker Thread           │
├─────────────────────────┤     ├─────────────────────────┤
│ renderWindow()          │     │ executeAsyncPreviewJob()│
│   ├─▶ enqueuePreviewJob()─────▶│   ├─▶ renderScene()    │
│   ├─▶ displayLastFrame()│     │   ├─▶ resample()        │
│   └─▶ processResults()◀───────│   └─▶ publishFrame()    │
└─────────────────────────┘     └─────────────────────────┘
```

## Key Files to Modify

| File | Purpose |
|------|---------|
| `RenderWindow.cpp` | Add `scheduleAsyncPreviewJob()`, `executeAsyncPreviewJob()`, result processing |
| `RenderWindow.h` | Add preview buffer, atomic epoch, job state |
| `ComputeCore.cpp` | Add `renderLowResPreviewAsync()` (remove `glFinish()`) |
| `ComputeCore.h` | Add async method declaration |
| `AsyncRenderController.h` | Add preview buffer pool (optional, reuse existing) |
| `AsyncRenderTypes.h` | Add `PreviewRenderJob`, `PreviewResultMeta` structs |

## Implementation Phases

### Phase 1: Add Async Preview Job Executor (~4h)

1. Add `PreviewRenderJob` struct to `AsyncRenderTypes.h`
2. Add `executeAsyncPreviewJob()` to `RenderWindow.cpp`
3. Follow existing `executeAsyncRenderJob()` pattern
4. Use `RenderJobType::LowResPreview` (already defined)

### Phase 2: Non-Blocking ComputeCore (~2h)

1. Add `renderLowResPreviewAsync()` to `ComputeCore.cpp`
2. Remove `glFinish()` call
3. Return `cl::Event` for async wait
4. Keep synchronous version for backward compatibility

### Phase 3: Wire Up UI Thread (~3h)

1. Replace `renderSync()` path with `scheduleAsyncPreviewJob()`
2. Add preview result polling in `renderWindow()`
3. Display last valid frame while rendering
4. Handle camera movement interruption

### Phase 4: Testing & Optimization (~3h)

1. Add unit tests for async preview cancellation
2. Measure FPS improvement (target: 60+ FPS during camera movement)
3. Profile buffer management overhead
4. Test edge cases (rapid camera movement, scene changes)

## Testing Checklist

- [ ] FPS stays above 60 during camera drag
- [ ] Preview appears within 3 frames of camera stop
- [ ] Rapid camera movement doesn't cause flickering
- [ ] Scene changes cancel stale preview jobs
- [ ] Memory usage stable (no buffer leaks)
- [ ] No race conditions (run with ThreadSanitizer)

## Rollback Plan

If issues arise:
1. Revert to synchronous path by checking flag
2. Add `GLADIUS_ASYNC_PREVIEW_ENABLED` environment variable
3. Default to async, fallback to sync if problems

## Success Metrics

| Metric | Before | After |
|--------|--------|-------|
| FPS during camera drag | 15-30 | 60+ |
| UI responsiveness | Stutters | Smooth |
| Preview latency | 0 frames | 1-3 frames |
