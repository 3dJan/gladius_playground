# API Contract: Async Preview Rendering

**Version**: 1.0  
**Date**: 2026-01-02

## Overview

This document defines the internal C++ API contracts for async preview rendering.
These are internal APIs (not public library interfaces).

## Contracts

### 1. Preview Job Scheduling

**Interface**: `RenderWindow::scheduleAsyncPreviewJob()`

```cpp
/**
 * @brief Schedules an asynchronous preview render job
 * @param camera Snapshot of current camera state
 * @return Unique frame identifier for tracking
 * 
 * @pre Camera must be valid (non-zero viewport dimensions)
 * @post Job is enqueued to worker thread
 * @post m_asyncPreviewJobInFlight == true
 * 
 * @thread UI thread only
 * @complexity O(1) - non-blocking enqueue
 */
uint64_t scheduleAsyncPreviewJob(CameraSnapshot const& camera);
```

**Behavior**:
- Captures current epoch atomically
- Creates job descriptor with preview resolution
- Enqueues to `AsyncRenderController::workerQueue()`
- Returns immediately (non-blocking)

### 2. Preview Job Execution

**Interface**: `RenderWindow::executeAsyncPreviewJob()`

```cpp
/**
 * @brief Executes preview rendering on worker thread
 * @param job Job descriptor with camera and resolution
 * @return Result metadata (success, latency, cancellation status)
 * 
 * @pre Job epoch must match current epoch (not stale)
 * @post Preview buffer contains rendered frame OR job cancelled
 * 
 * @thread Worker thread only
 * @complexity O(GPU_render_time)
 */
PreviewResultMeta executeAsyncPreviewJob(PreviewRenderJob const& job);
```

**Cancellation Points**:
1. Before GPU dispatch: Check epoch, return early if stale
2. After GPU dispatch: Check epoch, discard result if stale

### 3. Non-Blocking Preview Render

**Interface**: `ComputeCore::renderLowResPreviewAsync()`

```cpp
/**
 * @brief Starts asynchronous low-resolution preview render
 * @param renderProgram GL-CL shared render program
 * @param primitives Scene primitive data
 * @param mode Activation mode (should be AM_ONLY_PRECOMPSDF)
 * @param renderQuality Preview quality setting (0.1-0.5 typical)
 * @param fullFrame Always true for preview (no region optimization)
 * @return OpenCL event to wait on for completion
 * 
 * @pre OpenGL context shared with OpenCL
 * @post GPU render dispatched, event signaled on completion
 * 
 * @thread Any thread with compute context access
 * @complexity O(1) dispatch, GPU work is async
 */
cl::Event renderLowResPreviewAsync(
    RenderProgram& renderProgram,
    CLMath::Primitives const& primitives,
    ActivationMode mode,
    float renderQuality,
    bool fullFrame);
```

**Changes from `renderLowResPreview()`**:
- Removes `glFinish()` - caller responsible for sync
- Returns `cl::Event` instead of void
- Does not call `m_resultImage->bind()` - caller handles

### 4. Preview Buffer Management

**Interface**: `AsyncRenderController::acquirePreviewBuffer()`

```cpp
/**
 * @brief Acquires a preview buffer for writing
 * @param epoch Current render epoch for tracking
 * @return Reference to buffer, or nullopt if all busy
 * 
 * @pre epoch > previously acquired epoch (monotonic)
 * @post Returned buffer is in Writing state
 * 
 * @thread Worker thread only
 * @complexity O(1)
 */
std::optional<FrameBuffer&> acquirePreviewBuffer(uint64_t epoch);
```

**Interface**: `AsyncRenderController::publishPreviewFrame()`

```cpp
/**
 * @brief Publishes completed preview frame for UI consumption
 * @param buffer The buffer to publish (must be in Writing state)
 * @param meta Result metadata
 * 
 * @pre buffer.state == FrameState::Writing
 * @post buffer.state == FrameState::Ready
 * @post Result available in preview result queue
 * 
 * @thread Worker thread only
 * @complexity O(1)
 */
void publishPreviewFrame(FrameBuffer& buffer, PreviewResultMeta const& meta);
```

### 5. Result Consumption

**Interface**: `RenderWindow::processAsyncPreviewResults()`

```cpp
/**
 * @brief Polls for completed preview frames (non-blocking)
 * @return true if a new preview frame is available
 * 
 * @post If true: m_resultImage updated with preview pixels
 * @post Old preview buffer transitioned to Idle
 * 
 * @thread UI thread only
 * @complexity O(1) poll + O(pixels) copy
 */
bool processAsyncPreviewResults();
```

## State Invariants

### Epoch Monotonicity
```
∀ job₁, job₂ : job₁.enqueueTime < job₂.enqueueTime 
              ⟹ job₁.epoch ≤ job₂.epoch
```

### Buffer Exclusivity
```
∀ buffer : |{thread : thread.owns(buffer)}| ≤ 1
```

### Frame Ordering
```
∀ frame₁, frame₂ : displayed(frame₁) ∧ displayed(frame₂) 
                  ⟹ frame₁.epoch ≤ frame₂.epoch
```

## Error Handling

| Condition | Response |
|-----------|----------|
| No buffer available | Skip frame, retry next poll |
| OpenCL error | Log error, return empty frame |
| Stale epoch | Cancel immediately, don't publish |
| GPU timeout | Cancel job, increment epoch |

## Performance Guarantees

| Metric | Requirement | Notes |
|--------|-------------|-------|
| UI thread blocking | < 1ms | No GPU waits on UI thread |
| Job cancellation latency | < 16ms | Cancel before next frame |
| Preview display latency | 1-3 frames | Triple buffer pipeline |
| Memory overhead | ~4MB | 2× preview buffers at 1080p×0.25 |
