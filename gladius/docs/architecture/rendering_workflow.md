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

## WebGPU Callbacks: Native Pumping vs. Browser Async

WebGPU completion callbacks (`MapAsync`, `PopErrorScope`) are delivered by the host event loop.
The two builds reach that loop differently, so the waiting strategy must differ:

| Build | Callback delivery | Correct wait |
|-------|-------------------|--------------|
| Native (Dawn) | `Instance::ProcessEvents()` on the calling thread | `co_await compute::WaitUntil(...)` driven by `compute::runBlocking(task, pump)`, or a `processEvents()` pump loop |
| Browser (emdawnwebgpu) | JS promise continuation only | `co_await compute::WaitUntil(...)`, task stepped by `AsyncTask::step()` from the render/progress path |

Rule for code compiled with `__EMSCRIPTEN__`: never call `processEvents()` (directly or through
`compute::runBlocking`) to wait for a WebGPU callback. emdawnwebgpu registers only `WaitAnyOnly` and
`AllowProcessEvents` events with its `EventManager` and `ProcessEvents()` ignores spontaneous events,
so an `AllowSpontaneous` callback such as `PopErrorScope` can only land after wasm returns control to
JS. A pump loop there spins at 100% CPU on the browser main thread and freezes the whole UI.

Blocking WebGPU entry points (`WebGPUThumbnailRenderer::renderPng`, the mesh exporters, the colour
sampler, `WebGPUProgramValidator::validateShaderSource`, the blocking `WebGPUSdfEvaluator` overloads,
`Document::exportWebGpuMesh`) throw `webgpu::makeBrowserBlockingUnavailableError(...)` in the browser
build; browser flows use the `*Async` coroutine variants instead. The regression guard
`tests/unittests/WebGPUErrorScopeHygiene_tests.cpp` fails if a pump or `runBlocking` appears outside a
native-only preprocessor branch.

---

## Summary: Key Invariants

1. **Low-res preview is always synchronous** on the UI thread for immediate feedback
2. **HQ rendering only starts when `previewEpoch == currentEpoch`** to avoid rendering with stale parameters
3. **Front buffer display requires epoch match** to avoid showing stale HQ frames
4. **Outdated async results still update GL textures** but don't affect scheduling state
5. **Epoch increment cancels/invalidates in-flight work** to prevent stale results from blocking new work
6. **Camera movement (`isMoving`) always uses low-res preview**, not front buffer
7. **Browser builds never pump WebGPU callbacks synchronously** - they await them as coroutine suspensions (see above)
