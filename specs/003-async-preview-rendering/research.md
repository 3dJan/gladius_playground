# Research: Asynchronous Preview Rendering

**Date**: 2026-01-02  
**Feature**: 003-async-preview-rendering

## Executive Summary

The existing async rendering infrastructure is well-suited for async preview rendering. The main work involves:
1. Implementing `executeAsyncPreviewJob()` similar to existing `executeAsyncRenderJob()`
2. Removing blocking calls from the UI thread preview path
3. Adding a dedicated preview buffer to avoid contention with HQ progressive rendering

## Research Tasks

### 1. Current Blocking Points Analysis

**Task**: Identify all blocking calls in the preview rendering path

**Findings**:

| Blocking Call | Location | Why It Blocks | Resolution |
|---------------|----------|---------------|------------|
| `waitForComputeToken()` | `RenderWindow::renderWindow()` L257 | Mutex lock on `m_computeMutex` | Use `requestComputeToken()` (non-blocking) or skip if busy |
| `glFinish()` | `ComputeCore::renderLowResPreview()` L1590 | Waits for all GPU commands | Remove; use event-based sync |
| `renderScene()` | `ComputeCore::renderLowResPreview()` L1600 | Synchronous GPU dispatch | Use `renderSceneAsync()` with event |
| `resample()` | `ComputeCore::renderLowResPreview()` L1607 | Synchronous upscaling | Use async version or batch with render |
| `bind()` | `ComputeCore::renderLowResPreview()` L1612 | Triggers `transferPixels()` in readpixel mode | Defer bind to UI thread, use triple buffering |
| `clFinish()` | `GLImageBuffer::transferPixels()` L134 | Waits for OpenCL queue | Use events, not finish |

### 2. Existing Async Infrastructure Analysis

**Task**: Evaluate `AsyncRenderController` for preview rendering suitability

**Findings**:

The existing infrastructure provides:

1. **Job Queue**: `enqueueJob(RenderJob)` - ready to use, supports `RenderJobType::LowResPreview`
2. **Worker Thread Pool**: `coro::thread_pool` - manages background workers
3. **Triple Buffering**: `m_frameBuffers[3]` with state machine (Idle→Writing→Ready→Front)
4. **Epoch Tracking**: Cancellation of stale jobs via epoch comparison
5. **Result Queue**: `tryConsumeResult()` for UI thread to poll completed frames
6. **Worker Queue**: Separate `cl::CommandQueue` for async GPU operations

**Gap Analysis**:

| Feature | HQ Rendering | Preview Rendering | Gap |
|---------|--------------|-------------------|-----|
| Job Executor | `executeAsyncRenderJob()` | None | Need `executeAsyncPreviewJob()` |
| Buffer | Uses progressive buffer | None | Need dedicated preview buffer |
| Resolution | Full/progressive | Low-res | Need to respect `renderQualityWhileMoving` |
| SDF Mode | `AM_HYBRID` | `AM_ONLY_PRECOMPSDF` | Need to set mode correctly |
| Priority | Lower | Higher (latency sensitive) | May need priority queue |

### 3. OpenCL/OpenGL Interop Considerations

**Task**: Understand GPU resource sharing constraints

**Findings**:

1. **Shared Resources**: Both preview and HQ use same `m_primitives`, `m_resources`, render program
2. **Command Queues**: 
   - UI thread: `m_ComputeContext->GetQueue()`
   - Worker thread: `m_asyncController->workerQueue()`
3. **Buffer Contention**: 
   - `m_resultImage` is shared (GLImageBuffer with GL texture)
   - Worker writes to `m_frameBuffers[i].image` (ImageRGBA, CL-only)
   - UI thread copies from frame buffer to `m_resultImage`

**Interop Mode Detection**:
- `OutputMethod::interop`: GL/CL share texture memory
- `OutputMethod::readpixel`: Separate buffers, requires CPU transfer

**Recommendation**: Preview rendering should use the same pattern as HQ:
1. Render to CL-only `ImageRGBA` buffer on worker thread
2. Copy to `m_resultImage` via `enqueueCopyImage()` 
3. UI thread only calls `bind()` to update GL texture

### 4. Preview vs HQ Rendering Differences

**Task**: Document differences in render configuration

| Aspect | Preview (Low-Res) | HQ (Progressive) |
|--------|-------------------|------------------|
| Resolution | `renderQualityWhileMoving` (~10-25%) | `renderQuality` (100%) |
| SDF Mode | `AM_ONLY_PRECOMPSDF` | `AM_HYBRID` |
| Start Line | Always 0 (full frame) | Progressive (0, stepSize, 2*stepSize, ...) |
| Step Size | Full height | Adaptive (8→larger as stable) |
| Frame Rate | As fast as possible | Target 500ms per chunk |
| Cancellation | Any camera input | Epoch change |

### 5. State Machine for Async Preview

**Task**: Design preview buffer state transitions

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
    ┌───────┐   camera   ┌──────────┐  complete  ┌───────┐│
    │ Idle  │──────────▶│ Writing  │──────────▶│ Ready ││
    └───────┘   move     └──────────┘           └───────┘│
        ▲                     │                     │     │
        │                     │ cancel              │     │
        │                     │ (new input)         │     │
        │                     ▼                     │     │
        └─────────────────────┘                     │     │
                                                    │     │
                              promote to front ◀────┘     │
                              (UI thread)                 │
                                    │                     │
                                    ▼                     │
                              ┌───────┐                   │
                              │ Front │───────────────────┘
                              └───────┘  replaced by newer
```

**Key Insight**: Preview jobs should be **fire-and-forget**. If camera moves again before preview completes, cancel the in-flight job and start a new one. This ensures the freshest preview is always displayed.

## Decisions

### Decision 1: Separate Preview Buffer vs Shared

**Decision**: Use dedicated preview buffer, separate from HQ progressive buffer

**Rationale**: 
- Avoids contention between preview and HQ rendering
- Preview can be cancelled without losing HQ progress
- Simpler state management

**Alternatives Rejected**:
- Shared buffer: Would require complex priority/preemption logic

### Decision 2: Preview Job Priority

**Decision**: Preview jobs execute immediately, cancelling any prior preview job

**Rationale**:
- User needs visual feedback within 100ms
- Preview frames are cheap (low-res, precomputed SDF)
- No need for queuing—latest camera position is all that matters

**Alternatives Rejected**:
- Priority queue: Over-engineered for this use case

### Decision 3: Resample Location

**Decision**: Resample on worker thread before publishing

**Rationale**:
- Keeps UI thread free of compute work
- Resample is fast on GPU
- Result buffer is display-ready

**Alternatives Rejected**:
- UI thread resample: Would block main thread

### Decision 4: GL Texture Update Strategy

**Decision**: Use `invalidateContent()` + deferred `bind()` on UI thread

**Rationale**:
- `bind()` must happen on GL context thread (UI thread)
- Worker marks buffer dirty, UI thread transfers when displaying
- Already implemented pattern in existing code

## Implementation Approach

### Phase 1: Add Async Preview Job Executor

1. Add `executeAsyncPreviewJob()` method to `RenderWindow`
2. Follow pattern of `executeAsyncRenderJob()` but:
   - Use `AM_ONLY_PRECOMPSDF` mode
   - Render full frame (not progressive)
   - Use preview resolution
3. Register executor for `LowResPreview` job type

### Phase 2: Remove Blocking Calls

1. In `RenderWindow::renderAsync()`:
   - Replace synchronous `renderLowResPreview()` call with job enqueue
   - Remove `waitForComputeToken()` for preview path
2. In `ComputeCore`:
   - Add `renderLowResPreviewAsync()` returning `cl::Event`
   - Remove `glFinish()` from preview path

### Phase 3: Add Preview Buffer Management

1. Add `m_asyncPreviewBuffer` to `RenderWindow`
2. Implement preview-specific buffer lifecycle
3. Ensure preview buffer is separate from HQ progressive buffer

### Phase 4: Integration & Testing

1. Add FPS measurement during camera movement
2. Add unit test for preview job execution
3. Add integration test verifying 55+ FPS target
