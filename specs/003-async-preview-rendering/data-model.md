# Data Model: Asynchronous Preview Rendering

**Date**: 2026-01-02  
**Feature**: 003-async-preview-rendering

## Overview

This document defines the data structures and state machines for async preview rendering.

## Key Entities

### 1. Preview Buffer State Machine

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Preview Buffer Lifecycle                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌────────┐                                                         │
│  │  Idle  │◀───────────────────────────────────────────┐            │
│  └────┬───┘                                            │            │
│       │                                                │            │
│       │ acquirePreviewBuffer(epoch)                    │            │
│       │ [camera moved]                                 │            │
│       ▼                                                │            │
│  ┌─────────┐                                           │            │
│  │ Writing │──────┐                                    │            │
│  └────┬────┘      │                                    │            │
│       │           │ releaseOnCancel()                  │            │
│       │           │ [new camera input OR epoch change] │            │
│       │           └────────────────────────────────────┘            │
│       │                                                             │
│       │ publishPreviewFrame()                                       │
│       │ [render complete]                                           │
│       ▼                                                             │
│  ┌───────┐                                                          │
│  │ Ready │                                                          │
│  └───┬───┘                                                          │
│      │                                                              │
│      │ promotePreviewToFront()                                      │
│      │ [UI thread picks up]                                         │
│      ▼                                                              │
│  ┌───────┐                                                          │
│  │ Front │──────────────────────────────────────────────────────────┘
│  └───────┘  [replaced by newer Ready buffer]                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 2. Preview Job Data Structure

```cpp
struct PreviewRenderJob
{
    uint64_t epoch;           // Cancellation token
    uint64_t frameId;         // Unique frame identifier
    uint32_t width;           // Preview resolution width
    uint32_t height;          // Preview resolution height
    CameraSnapshot camera;    // Camera state at job creation
    bool requiresSdfValid;    // True if SDF must be precomputed
};
```

### 3. Preview Result Metadata

```cpp
struct PreviewResultMeta
{
    uint64_t frameId;           // Matches job.frameId
    uint64_t epoch;             // Matches job.epoch
    uint64_t latencyNs;         // Time from job enqueue to completion
    bool cancelled;             // True if job was cancelled
    bool sdfWasValid;           // True if SDF was available
};
```

## State Transitions

### Camera Input → Preview Job Lifecycle

```
┌─────────────────────────────────────────────────────────────────────┐
│                       Event Timeline                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  UI Thread                      Worker Thread                       │
│  ─────────                      ─────────────                       │
│                                                                     │
│  [Mouse drag start]                                                 │
│       │                                                             │
│       ├──▶ invalidateView()                                         │
│       │    ├─ epoch++                                               │
│       │    └─ cancel in-flight jobs                                 │
│       │                                                             │
│       ├──▶ enqueuePreviewJob(epoch, camera)                         │
│       │              │                                              │
│       │              └────────────────────────▶ [job received]      │
│       │                                              │              │
│       │                                              ├─▶ acquire    │
│       │                                              │   buffer     │
│       │                                              │              │
│       │                                              ├─▶ render     │
│       │                                              │   preview    │
│  [Next frame]                                        │              │
│       │                                              │              │
│       ├──▶ processAsyncResults()                     │              │
│       │    └─ (no result yet)                        │              │
│       │                                              │              │
│       ├──▶ displayLastValidFrame()                   ├─▶ publish    │
│       │                                              │   frame      │
│  [Next frame]                                        │              │
│       │                                              ▼              │
│       ├──▶ processAsyncResults()◀───────────[result available]      │
│       │    ├─ promotePreviewToFront()                               │
│       │    └─ update m_resultImage                                  │
│       │                                                             │
│       ├──▶ displayNewPreviewFrame()                                 │
│       │                                                             │
│  [Mouse drag continues]                                             │
│       │                                                             │
│       └──▶ (repeat cycle with new epoch)                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## Thread Safety Model

### Shared Resources

| Resource | Owner | UI Thread Access | Worker Thread Access |
|----------|-------|------------------|----------------------|
| `m_asyncPreviewBuffer` | RenderWindow | Read (display) | Write (render) |
| `m_resultImage` | ComputeCore | Read/Write | None |
| `m_primitives` | ComputeCore | None | Read |
| `m_computeMutex` | ComputeCore | Try-lock | Try-lock |
| Preview epoch | RenderWindow | Write | Read |

### Atomic Variables

```cpp
// In RenderWindow
std::atomic<uint64_t> m_asyncPreviewEpoch{0};      // Current preview epoch
std::atomic<bool> m_asyncPreviewJobInFlight{false}; // Preview job active
std::atomic<uint64_t> m_asyncPreviewFrameId{0};     // Latest completed frame
```

### Lock-Free Pattern

The preview system uses a lock-free producer-consumer pattern:

1. **Producer (Worker)**: 
   - Atomically transitions buffer: Idle → Writing → Ready
   - Publishes result metadata to lock-free queue

2. **Consumer (UI)**:
   - Polls result queue (non-blocking)
   - Atomically transitions buffer: Ready → Front
   - Copies pixels to GL texture

## Buffer Memory Layout

### Preview Buffer Pool

```cpp
// In RenderWindow
struct PreviewBufferPool
{
    // Separate from HQ buffers to avoid contention
    std::array<async_rendering::FrameBuffer, 2> buffers;  // Double buffer sufficient
    std::atomic<size_t> frontIndex{0};
    
    // Preview-specific sizing (lower resolution)
    size_t previewWidth{0};
    size_t previewHeight{0};
};
```

### Resolution Calculation

```cpp
// Preview resolution = display size × renderQualityWhileMoving
size_t previewWidth = static_cast<size_t>(
    m_renderWindowSize_px.x * state.renderQualityWhileMoving);
size_t previewHeight = static_cast<size_t>(
    m_renderWindowSize_px.y * state.renderQualityWhileMoving);
```

## Epoch-Based Cancellation

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Epoch Cancellation Flow                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Current Epoch: 42                                                  │
│  In-flight Job: epoch=42, frameId=100                               │
│                                                                     │
│  [Camera moves] ──▶ epoch becomes 43                                │
│                                                                     │
│  Worker thread checks: job.epoch (42) < currentEpoch (43)           │
│       │                                                             │
│       ├─▶ Cancel immediately                                        │
│       ├─▶ Release buffer (Writing → Idle)                           │
│       └─▶ Return early with cancelled=true                          │
│                                                                     │
│  UI thread enqueues new job: epoch=43, frameId=101                  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```
