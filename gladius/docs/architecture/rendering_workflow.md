# Gladius Rendering Workflow

This document outlines the rendering pipeline, state machine, and async interactions in the RenderWindow system.

---

## Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              UI Thread (Main Loop)                          │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                   │
│  │ User Input   │───>│ Parameter    │───>│ RenderWindow │                   │
│  │ (DragFloat,  │    │ Update       │    │ ::render()   │                   │
│  │  Camera)     │    │ (Document)   │    │              │                   │
│  └──────────────┘    └──────────────┘    └──────┬───────┘                   │
│                                                  │                           │
│                                    ┌─────────────┴─────────────┐             │
│                                    ▼                           ▼             │
│                          ┌─────────────────┐         ┌─────────────────┐     │
│                          │ renderAsync()   │         │ processAsync    │     │
│                          │ (scheduling)    │         │ Results()       │     │
│                          └────────┬────────┘         └────────┬────────┘     │
│                                   │                           │              │
└───────────────────────────────────┼───────────────────────────┼──────────────┘
                                    │                           │
                                    ▼                           ▲
                    ┌───────────────────────────────┐           │
                    │    AsyncRenderController      │           │
                    │    (Worker Thread)            │           │
                    │  ┌─────────────────────────┐  │           │
                    │  │ Job Queue               │  │           │
                    │  │ - SDF Precomputation    │  │           │
                    │  │ - HQ Render Chunks      │  │           │
                    │  │ - BBox Update           │  │           │
                    │  └─────────────────────────┘  │           │
                    │              │                │           │
                    │              ▼                │           │
                    │  ┌─────────────────────────┐  │           │
                    │  │ Result Queue            │──┼───────────┘
                    │  │ (consumed by UI thread) │  │
                    │  └─────────────────────────┘  │
                    └───────────────────────────────┘
```

---

## State Machine: RenderWindowState

```
                                    ┌───────────────────┐
                                    │      IDLE         │
                                    │  isRendering=false│
                                    │  isMoving=false   │
                                    └─────────┬─────────┘
                                              │
                     ┌────────────────────────┼────────────────────────┐
                     │                        │                        │
                     ▼                        ▼                        ▼
           ┌─────────────────┐     ┌─────────────────┐      ┌─────────────────┐
           │    MOVING       │     │ PARAMETER_DIRTY │      │  1-SEC TIMEOUT  │
           │  isMoving=true  │     │ m_parameterDirty│      │   elapsed       │
           │                 │     │ =true           │      │                 │
           └────────┬────────┘     └────────┬────────┘      └────────┬────────┘
                    │                       │                        │
                    │                       ▼                        │
                    │              ┌─────────────────┐               │
                    │              │ EPOCH INCREMENT │               │
                    │              │ invalidateView()│               │
                    │              └────────┬────────┘               │
                    │                       │                        │
                    ▼                       ▼                        │
           ┌─────────────────────────────────────────┐               │
           │           LOW-RES PREVIEW               │               │
           │  - Synchronous on UI thread             │               │
           │  - Updates m_lastLowResPreviewEpoch     │               │
           │  - Updates m_lastLowResRenderTime       │               │
           │  - Clears m_lowResFeedbackPending       │               │
           └────────────────────┬────────────────────┘               │
                                │                                    │
                                ▼                                    │
                   ┌────────────────────────┐                        │
                   │ previewEpoch ==        │◄───────────────────────┘
                   │ currentEpoch?          │
                   └───────────┬────────────┘
                               │
              ┌────────────────┴────────────────┐
              │ NO                              │ YES
              ▼                                 ▼
   ┌─────────────────────┐           ┌─────────────────────┐
   │ Force low-res again │           │ START HQ RENDERING  │
   │ m_forceLowRes=true  │           │ isRendering=true    │
   │ (retry next frame)  │           │ scheduleAsyncJob()  │
   └─────────────────────┘           └──────────┬──────────┘
                                                │
                                                ▼
                                     ┌─────────────────────┐
                                     │  PROGRESSIVE HQ     │
                                     │  (Async Worker)     │
                                     │                     │
                                     │  Renders in chunks  │
                                     │  currentLine += step│
                                     └──────────┬──────────┘
                                                │
                                     ┌──────────┴──────────┐
                                     │ Chunk completed     │ Frame completed
                                     ▼                     ▼
                          ┌─────────────────┐   ┌─────────────────┐
                          │ Continue chunks │   │ Promote to      │
                          │ isRendering=true│   │ Front Buffer    │
                          │ schedule next   │   │ isRendering=false│
                          └─────────────────┘   └─────────────────┘
```

---

## Key Flags and Their Meanings

| Flag | Type | Purpose |
|------|------|---------|
| `m_dirty` | atomic<bool> | Scene needs re-rendering |
| `state.isMoving` | bool | Camera is actively being manipulated |
| `state.isRendering` | bool | Progressive HQ render in progress |
| `m_parameterDirty` | atomic<bool> | Model parameters changed |
| `m_preComputedSdfDirty` | atomic<bool> | SDF needs recomputation |
| `m_lowResFeedbackPending` | atomic<bool> | Low-res preview requested but not yet shown |
| `m_forceLowResRenderOnNextFrame` | atomic<bool> | Force immediate low-res render |
| `m_asyncJobInFlight` | atomic<bool> | Async HQ job currently running |
| `m_asyncSdfJobInFlight` | atomic<bool> | Async SDF job currently running |

---

## Epoch System

The epoch system prevents displaying stale frames and ensures proper job sequencing.

```
┌─────────────────────────────────────────────────────────────────┐
│                        EPOCH LIFECYCLE                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Parameter Change / Camera Move / Model Update                  │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────────────────────┐                            │
│  │ notifyAsyncEpochIncrement()     │                            │
│  │ - m_asyncCurrentEpoch++         │                            │
│  │ - Clear in-flight flags         │                            │
│  │ - Release stale buffers         │                            │
│  │ - Cancel progressive buffer     │                            │
│  └─────────────────────────────────┘                            │
│           │                                                     │
│           ▼                                                     │
│  Jobs tagged with new epoch                                     │
│           │                                                     │
│           ▼                                                     │
│  ┌─────────────────────────────────┐                            │
│  │ processAsyncResults()           │                            │
│  │ - Check result.epoch vs current │                            │
│  │ - isOutdated = epoch < current  │                            │
│  │                                 │                            │
│  │ If outdated:                    │                            │
│  │   - Still display frame (GL)    │                            │
│  │   - DON'T update scheduling     │                            │
│  │   - Clear in-flight if matches  │                            │
│  │                                 │                            │
│  │ If current:                     │                            │
│  │   - Update scheduling state     │                            │
│  │   - Mark dirty=false when done  │                            │
│  └─────────────────────────────────┘                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                     EPOCH TRACKING VARIABLES                    │
├─────────────────────────────────────────────────────────────────┤
│ m_asyncCurrentEpoch      │ Current valid epoch                  │
│ m_asyncInFlightEpoch     │ Epoch of currently running HQ job    │
│ m_asyncSdfInFlightEpoch  │ Epoch of currently running SDF job   │
│ m_lastLowResPreviewEpoch │ Epoch when low-res was last rendered │
│ frontBuf->epoch          │ Epoch of front buffer (HQ result)    │
└─────────────────────────────────────────────────────────────────┘
```

---

## Display Selection Logic

```
renderWindow() - Choose what to display:
─────────────────────────────────────────

┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  if (async backend active):                                 │
│      frontBuf = asyncController->frontBuffer()             │
│                                                             │
│      ┌─────────────────────────────────────────────────┐    │
│      │ useFrontBuffer = frontBuf exists                │    │
│      │              AND frontBuf->epoch == currentEpoch│    │
│      │              AND NOT isRendering (progressive)  │    │
│      │              AND NOT isMoving (camera)          │    │
│      └─────────────────────────────────────────────────┘    │
│                                                             │
│      if (useFrontBuffer):                                   │
│          displayImage = frontBuf->image   ◄── HQ result     │
│      else:                                                  │
│          displayImage = m_resultImage     ◄── Low-res/prog  │
│                                                             │
│  else (sync backend):                                       │
│      displayImage = m_resultImage                           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## Parameter Update Flow (DragFloat Interaction)

```
User drags slider continuously:
────────────────────────────────

Frame 1: value=1.0
    │
    ├─► Document::updateParameter()
    ├─► invalidateViewDuetoModelUpdate()
    │       └─► m_lowResFeedbackPending = true
    │       └─► epoch++
    │
    ├─► renderAsync()
    │       └─► isMoving || lowResPending → render low-res preview
    │       └─► m_lastLowResPreviewEpoch = currentEpoch
    │
    └─► Display: low-res preview (epoch N)

Frame 2: value=1.1 (user still dragging)
    │
    ├─► epoch++ again
    │
    ├─► renderAsync()
    │       └─► lowResPending=true → render low-res preview
    │       └─► m_lastLowResPreviewEpoch = currentEpoch (N+1)
    │
    └─► Display: low-res preview (epoch N+1)

    ... (continues while dragging) ...

User releases slider:
    │
    ├─► No more epoch increments
    │
    ├─► 1 second timeout elapses
    │
    ├─► previewEpoch == currentEpoch? YES
    │
    ├─► scheduleAsyncRenderJob() → HQ progressive starts
    │
    └─► Display: progressive chunks → final HQ (epoch matches)
```

---

## Async Job Types

| Job Type | Trigger | Execution | Result Handling |
|----------|---------|-----------|-----------------|
| `HighQuality` | 1-sec timeout after low-res | Worker thread, chunks | Updates front buffer |
| `SDFPrecomputation` | `m_preComputedSdfDirty=true` | Worker thread | Sets `m_preComputedSdfDirty=false` |
| `BoundingBoxUpdate` | Model change | Worker thread | Updates core bbox |
| `ParameterUpdate` | Reserved for future | - | - |

---

## Triple Buffer System

```
┌─────────────────────────────────────────────────────────────────┐
│                     FRAME BUFFER STATES                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐     │
│   │  Idle   │───>│ Writing │───>│  Ready  │───>│  Front  │     │
│   └─────────┘    └─────────┘    └─────────┘    └─────────┘     │
│        ▲                                            │          │
│        └────────────────────────────────────────────┘          │
│                      (after display)                           │
│                                                                 │
│   Idle:    Available for new render job                        │
│   Writing: Worker thread is rendering into this buffer         │
│   Ready:   Render complete, waiting to be promoted             │
│   Front:   Currently being displayed                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Summary: Key Invariants

1. **Low-res preview is always synchronous** on the UI thread for immediate feedback
2. **HQ rendering only starts when `previewEpoch == currentEpoch`** to avoid rendering with stale parameters
3. **Front buffer display requires epoch match** to avoid showing stale HQ frames
4. **Outdated async results still update GL textures** but don't affect scheduling state
5. **Epoch increment cancels/invalidates in-flight work** to prevent stale results from blocking new work
6. **Camera movement (`isMoving`) always uses low-res preview**, not front buffer
