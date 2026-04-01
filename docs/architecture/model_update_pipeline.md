# Model Update Pipeline — Node Editor to GPU

This document describes the update paths triggered when the user interacts with the node editor (changing parameter values, adding/removing nodes, creating/removing links).

## Dirty Flag Taxonomy

Two per-frame flags originate in `NodeView` and are consumed by `ModelEditor::showAndEdit()`:

| Flag | Set by | Meaning |
|---|---|---|
| `m_parameterChanged` | `NodeView` | Scalar/vector/matrix value edit, string change, input-link assignment |
| `m_modelChanged` | `NodeView` | Structural: adding/removing Begin/End arguments, changing resource references |

`ModelEditor` itself sets `m_modelWasModified` for structural changes it handles directly (add/delete node, add/delete link, paste, extract-to-function, undo/redo).

`ModelEditor::showAndEdit()` returns `true` when any parameter was modified. `ModelEditor::modelWasModified()` reports whether a recompile-worthy structural change occurred.

## Update Path 1 — Parameter-Only Change (Fast Path)

**Trigger:** User drags a slider, edits a float/vector/string, or changes a constant.

### Sequence (UI thread unless noted)

1. `NodeView` renders each node via `visit()` / `inputControls()`.  
   ImGui widgets detect value changes → `m_parameterChanged = true`.

2. `ModelEditor::showAndEdit()` observes `NodeView::haveParameterChanged()`:
   - Sets `m_dirty = true`.
   - Calls `m_currentModel->updateTypes()` **(synchronous, UI thread)**.
   - Creates an undo restore point (copies the `Assembly`).

3. `MainWindow::nodeEditor()` callback:
   - `parameterModifiedByModelEditor = true` → `m_parameterThrottle.onParameterChanged()`.
   - `markFileAsChanged()`.

4. `MainWindow::updateModel()` on a subsequent frame, once `m_parameterThrottle.shouldRecompile()` fires (1 ms debounce — effectively immediate; the streaming preview coroutine handles coalescing):
   - If streaming preview is **not** already active:
     - Calls `RenderWindow::invalidateViewDueToParameterChange()`:
       - Marks SDF invalid (`setSdfValid(false)`).
       - Sets `m_dirty`, `m_forceLowResRenderOnNextFrame`, `m_preComputedSdfDirty`, `m_parameterDirty`, `m_lowResFeedbackPending`.
       - Cancels in-progress progressive render.
       - Marks bounding box stale (preserves cached bbox with extra margin).
       - Records `m_lastParameterChangeTime` for debounce.
       - Does **not** bump the async epoch — the epoch is bumped later when the bbox debounce fires after the drag stops.
     - Calls `RenderWindow::startStreamingPreview()` — sets `m_streamingPreviewActive = true` and schedules a `StreamingPreview` job on the async coroutine worker.
   - If streaming preview **is** already active: skips both calls (the running coroutine already picks up the latest values).
   - Sets `m_parameterDirty = false`.

#### Streaming Preview Coroutine (worker thread)

`executeStreamingPreviewJob()` runs a tight loop on the async coroutine worker thread:

1. **Handshake wait** — spins until `m_streamingFrameConsumed` is `true` (set by the UI thread after resampling the previous frame). Times out after 200 ms and retries with `continue`.
2. **Clears handshake** — `m_streamingFrameConsumed = false` (prevents UI from reading mid-render).
3. **Push parameters** — `ComputeCore::tryToupdateParameter(assembly)`:
   - `m_computeMutex.try_lock()` — non-blocking; skips if the background worker holds the lock.
   - `updateParameterBlocking()` — fills `std::vector<float>` from registered parameters, then `paramBuf.write()` (`clEnqueueWriteBuffer(CL_TRUE)`) — blocks the **worker** thread, not the UI.
4. **Render** — `renderLowResPreviewWithDistanceOutputAsync(workerQueue, ...)` — uses a local copy of `RenderingSettings` with `RF_DISABLE_SHADOWS | RF_DISABLE_AO` and `AM_FULL_MODEL` approximation. Returns a `cl::Event`.
5. **Poll GPU completion** — polls the event status with 500 µs sleep intervals, up to 5 s timeout.
6. **Publish** — increments frame counter, writes `PreviewResultMeta` via `setLatestPreviewResult()`.
7. **Loop** — repeats from step 1 until `shouldStop()` (`!m_streamingPreviewActive || cancelCheck() || !isRunning()`).

The UI thread consumes frames in `processAsyncPreviewResults()`: resamples `lowResImage` → `resultImage`, syncs GL texture, then sets `m_streamingFrameConsumed = true`.

#### HQ Suppression During Streaming

While streaming preview is active, `m_suppressHQDisplay` is `true` — the UI thread skips compositing HQ progressive frames. This prevents a stale HQ buffer (rendered with old parameters) from overwriting the live streaming preview. The flag is cleared when the bbox debounce fires after the drag stops.

#### Streaming Termination (Bbox Debounce)

When `m_lastParameterChangeTime` is older than `kBboxDebounceDelay` (1000 ms) and bbox is stale:
1. `stopStreamingPreview()` — sets `m_streamingPreviewActive = false`, causing the coroutine loop to exit.
2. `recomputeStaleBoundingBox()` — clears cached bbox for fresh recompute.
3. Marks SDF invalid → `m_preComputedSdfDirty = true`.
4. `notifyAsyncEpochIncrement()` — invalidates stale HQ front buffer.
5. `m_suppressHQDisplay = false` — re-enables HQ compositing.
6. Normal SDF → bbox → HQ pipeline resumes.

### Deferred Parameter Re-application

When a background compilation is running and the user changes parameters:
1. `nodeEditor()` sets `m_parameterDirty = true` as usual.
2. `updateModel()` sees `m_parameterDirty` and the throttle fires, but streaming preview may already be active → no-op (coroutine keeps running).
3. Inside the streaming coroutine, `tryToupdateParameter()` fails to acquire `m_computeMutex` → parameter push is silently skipped for that iteration. The loop retries on the next iteration.
4. After the background worker finishes and releases the lock, the next coroutine iteration succeeds — pushing the latest parameter values (read from node objects) through the current parameter mapping.
5. If streaming is **not** active (e.g., throttle bypassed after the coroutine exited), `updateModel()` starts a new streaming preview session.

### Async work triggered

| Work item | Where it runs |
|---|---|
| Streaming preview (push + render loop) | Async coroutine worker (off UI thread) |
| SDF precomputation | Async coroutine worker → OpenCL event → `co_await` |
| Bounding box update | Async coroutine worker (after debounce) |
| HQ progressive render | Async coroutine worker → OpenCL event → `co_await` |

### SDF Debounce During Parameter Drag

During rapid slider changes, SDF is repeatedly invalidated. Recomputing it every tick wastes GPU time since the result is immediately stale. SDF scheduling is debounced: it only proceeds when either:
- Bounding box is **not** stale (drag has stopped and bbox debounce fired), or
- `kBboxDebounceDelay` has elapsed since `m_lastParameterChangeTime`.

Low-res preview works via direct function evaluation (`AM_FULL_MODEL`), so SDF is not needed for interactive feedback.

## Update Path 2 — Structural Change (Full Recompile)

**Trigger:** Adding/removing nodes, creating/deleting links, paste, extract-to-function, undo/redo, Begin/End argument changes.

### Sequence

#### Phase A — UI thread (immediate)

1. `ModelEditor` handler (e.g. `onCreateNode`, `onDeleteNode`, link accept) calls `markModelAsModified()`:
   - Sets `m_modelWasModified = true`.
   - Sets `m_primitiveDataDirty = true`.

2. `MainWindow::nodeEditor()` callback:
   - `modelWasModified = true` → calls `m_doc->getAssembly()->updateInputsAndOutputs()` **(synchronous)**.
   - Calls `m_doc->updateParameterRegistration()` **(synchronous)**.
   - `compileRequested` is `true` (when `m_autoCompile` is enabled) → calls `MainWindow::refreshModel()`.

3. `MainWindow::refreshModel()` → `Document::refreshModelIfNoCompilationIsRunning()`:
   - **Guard (non-blocking):** Returns `false` if any OpenCL program is already compiling. The dirty flags survive, so the next frame retries.
   - Otherwise calls `Document::refreshModelAsync()` → **launches `std::async(std::launch::async, refreshWorker)`**.
   - **Note:** Assembly validation has been moved to the worker thread (step 1b below) to avoid blocking the UI.

#### Phase B — Worker thread (async)

`Document::refreshWorker()` runs entirely off the UI thread:

| Step | Operation | Notes |
|---|---|---|
| 1 | `waitForComputeToken()` | Blocks worker until GPU mutex available |
| 1b | `validateAssembly()` | Early exit if model is invalid (e.g. missing connections). Signals `compilationFinished` before returning. |
| 2 | `updateInputsAndOutputs()` | Re-registers I/O ports for all functions |
| 3 | `loadAllMeshResources()` | Loads any unloaded mesh resources from 3MF |
| 4 | `updateParameterRegistration()` | Iterates all nodes to register parameters |
| 5 | `updateParameter()` | Writes parameter buffer to GPU (blocking on worker) |
| 6 | `rebuildResourceDependencyGraph()` | Rebuilds dependency graph for resource ordering |
| 7 | `updateFlatAssembly()` | Flattens assembly: lowers gradients, normalizes distance fields, optimizes outputs |
| 8 | `ComputeCore::refreshProgram()` | Generates OpenCL C kernel source (code gen or command stream) and captures parameter signature |
| 9 | `ComputeCore::recompileIfRequired()` | Dispatches non-blocking OpenCL compilations (see below) |
| 10 | Polls `isCompilationInProgress()` | Worker sleeps in 10 ms intervals until all programs finish compiling |
| 11 | `precomputeSdfAsync()` | Launches async SDF precompute via OpenCL event, then `sdfEvent.wait()` — blocks worker, not UI |
| 12 | `updateBBox()` | Computes bounding box from the new SDF — blocks worker, not UI |

**OpenCL compilation dispatch (step 9):**

`ProgramManager::compileRenderProgram()` calls `ProgramBase::recompileNonBlocking()`:
- On the **first build** (`m_isFirstBuild`): compilation is **blocking** (synchronous `compile()` on the worker thread).
- On subsequent builds: `CLProgram::compileNonBlocking()` launches a **nested `std::async`** that calls `compile()`, so OpenCL compilation runs on a third thread.

The same pattern applies to the slicer program, hierarchical DC program, and manifold DC program — all via `recompileNonBlocking()`.

#### Phase C — UI thread (completion)

On the next frame after the async worker finishes:

1. `Document::refreshModelIfNoCompilationIsRunning()` returns `true`.
2. `MainWindow::refreshModel()`:
   - Clears logger events.
   - `RenderWindow::invalidateViewDuetoModelUpdate()` — full invalidation: SDF invalid, bbox reset, schedules async preview.
   - `m_modelEditor.markModelAsUpToDate()` — clears `m_modelWasModified` and `m_isManualCompileRequested`.

## Update Path 3 — Manual Compile

Same as Path 2, but triggered via `m_isManualCompileRequested = true` (the "Compile" button when auto-compile is disabled) rather than the auto-compile flag.

## Async Rendering Pipeline

`RenderWindow` uses a coroutine-based async renderer (`AsyncRenderController`) for all GPU-bound rendering:

| Job type | Purpose | Scheduling |
|---|---|---|
| `SDFPrecomputation` | Precompute SDF grid on GPU | Enqueued when `m_preComputedSdfDirty` is set (debounced during parameter drag) |
| `StreamingPreview` | Continuous low-res preview during parameter drag | Enqueued by `startStreamingPreview()`, runs until `stopStreamingPreview()` |
| `BoundingBoxUpdate` | Compute/refine bbox from SDF | Enqueued when `m_asyncBboxUpdatePending` is set |
| `LowResPreview` | One-shot visual feedback (non-streaming) | Scheduled on `m_forceLowResRenderOnNextFrame` |
| `ProgressiveHQ` | Full-resolution progressive render | Scheduled after SDF is valid |

All jobs run on a worker thread via `co_await waitForEvent(clEvent)`. Results are polled and composited on the UI thread in `processAsyncResults()` / `processAsyncPreviewResults()`.

Epoch-based cancellation: each invalidation bumps `m_asyncEpochCounter`. In-flight jobs with stale epochs are discarded on completion.

## Dynamic Resolution Adaptation

The low-res preview resolution is not fixed — it adapts to maintain interactive frame rates during camera movement and parameter drags.

### Resolution Computation

The low-res preview dimensions are derived from the viewport size scaled by `state.renderQualityWhileMoving`:

```
newWidth  = m_renderWindowSize_px.x × renderQualityWhileMoving   (clamped to [1, 16000])
newHeight = m_renderWindowSize_px.y × renderQualityWhileMoving   (clamped to [1, 16000])
```

`renderQualityWhileMoving` starts at `renderQuality × 0.5` (typically `0.6`) and is reset to `0.1` when compilation starts. It is adjusted each frame by the PID controller (see below).

### Hysteresis Gate

To avoid constant image reallocation, the low-res preview buffer is only reallocated when:
- **Width or height changes by more than 20%**, or
- **Aspect ratio changes by more than 0.01**

This prevents GPU memory churn from small viewport adjustments.

### PID Controller (Sync Path)

In `renderSync()`, after each synchronous render, a PID controller adjusts `renderQualityWhileMoving` to converge on a **25 ms target frame time** (~40 FPS):

| Parameter | Value | Role |
|---|---|---|
| `kp` | 0.001 | Proportional gain — primary response to frame time error |
| `ki` | 0.00001 | Integral gain — eliminates steady-state error (low to prevent windup) |
| `kd` | 0.000001 | Derivative gain — dampens oscillation |
| Target | 25 ms | Target frame time during interaction |

The controller only runs when the camera is moving or a compilation is in progress. The integral term decays by `0.8×` each frame to prevent windup. The quality factor is clamped to `[0.05, renderQuality]`.

### Progressive Step Size Adaptation (HQ Path)

For HQ progressive rendering, `state.renderingStepSize` (lines per async chunk) is adjusted after each completed chunk by `adjustProgressFromDuration()`:

- **Chunk time > 100 ms target:** Reduce step size by 50% (or 90% if SDF is dirty).
- **Chunk time < 100 ms target:** Grow step size by 1.5× + 1.

This keeps each progressive chunk near 100 ms to avoid long GPU stalls while maximizing throughput.

### `cancelAllAsyncWork()`

Called before file load/new model operations to ensure no coroutines are in flight when CL resources are rebuilt:

1. `stopStreamingPreview()` — signals the streaming loop to exit.
2. `notifyAsyncEpochIncrement()` — invalidates all in-flight job epochs.
3. Busy-waits up to 500 ms for `m_streamingJobInFlight` and `m_asyncSdfJobInFlight` to clear.

## Blocking Operations on the UI Thread

| Operation | When | Severity |
|---|---|---|
| `Model::updateTypes()` | Every parameter change frame | Medium (graph traversal) |
| `Assembly::updateInputsAndOutputs()` | Structural model changes only | Medium (iterates all functions/nodes) |
| `Document::updateParameterRegistration()` | Structural model changes only | Medium (iterates all nodes) |
| `Document::validateAssemblyIfDirty()` | Every frame when graph is dirty | Medium (full validation pass) |
| `Assembly` copy for undo | Every parameter change | Medium (deep copy of entire assembly) |
| `processAsyncPreviewResults()` — resample + GL sync | Every streaming frame consumed | Low (single resample + texture upload) |

**No longer on UI thread:** `Buffer::write()` (`paramBuf.write()`) and `Document::updateParameter()` — moved to the streaming preview worker coroutine.
| `Model::updateTypes()` | Structural changes (skipped when `m_typesRequireUpdate` is `false`) | Medium (graph traversal) |
| `Assembly::updateInputsAndOutputs()` | Structural model changes only | Medium (iterates all functions/nodes) |
| `Document::updateParameterRegistration()` | Structural model changes only | Medium (iterates all nodes) |
| `Assembly` copy for undo | Every structural change (parameter changes pass by const ref — single copy) | Medium (deep copy of entire assembly) |
| `processAsyncPreviewResults()` — resample + GL sync | Every streaming frame consumed | Low (single resample + texture upload) |

**No longer on UI thread:**
- `Buffer::write()` (`paramBuf.write()`) and `Document::updateParameter()` — moved to the streaming preview worker coroutine.
- `Document::validateAssembly()` — moved to `refreshWorker()` to avoid blocking the UI with full graph validation.

### Optimizations

- **`m_typesRequireUpdate` flag:** `Model::updateTypes()` short-circuits when types haven't been invalidated (no node/link changes since last update). Set alongside `m_graphRequiresUpdate` on structural changes. Eliminates redundant full-graph type inference passes.
- **Deferred graph rebuild in `Model::remove()`:** The post-removal graph rebuild is deferred — callers like `updateInputsAndOutputs()` trigger it via `updateGraphAndOrderIfNeeded()` when they need it, saving one O(N+E) rebuild per deletion.
- **Single Assembly copy for undo:** Parameter change undo snapshots pass `*m_assembly` by const reference directly to `History::storeState()`, avoiding an intermediate deep copy.

## Thread Architecture Diagram

```
UI Thread (main loop)
│
├─ ModelEditor::showAndEdit()
│   ├─ NodeView renders + detects changes
│   ├─ Model::updateTypes()                    ← synchronous
│   └─ Assembly copy for undo                  ← synchronous
│
├─ MainWindow::nodeEditor() callback
│   ├─ [structural only] Assembly::updateInputsAndOutputs()  ← synchronous
│   ├─ [structural only] Document::updateParameterRegistration()
│   └─ refreshModel() [if compile requested]
│       └─ refreshModelAsync()  ──────────────────────┐
│                                                      │
├─ MainWindow::updateModel()                           │
│   ├─ [if !streaming] invalidateViewDueToParameterChange()
│   ├─ [if !streaming] startStreamingPreview()  ───────┼──────────┐
│   └─ m_parameterDirty = false                        │          │
│                                                      │          │
├─ RenderWindow::renderAsync()                         │          │
│   ├─ processAsyncResults()                           │          │
│   ├─ processAsyncPreviewResults()                    │          │
│   │   ├─ resample(lowRes → result)           ← sync │          │
│   │   ├─ GL texture bind/unbind              ← sync │          │
│   │   └─ m_streamingFrameConsumed = true             │          │
│   ├─ [after debounce] stopStreamingPreview()         │          │
│   ├─ [after debounce] recomputeStaleBoundingBox()    │          │
│   └─ enqueue SDF / bbox / render jobs                │          │
│                                                      ▼          │
│                                          Worker Thread (std::async)
│                                          │ Document::refreshWorker()
│                                          │  ├─ waitForComputeToken()
│                                          │  ├─ updateInputsAndOutputs()
│                                          │  ├─ updateParameter()
│                                          │  ├─ updateFlatAssembly()
│                                          │  ├─ refreshProgram() [code gen]
│                                          │  ├─ recompileIfRequired()
│                                          │  │   └─ compileNonBlocking() ──┐
│                                          │  │                             │
│                                          │  ├─ [polls until complete]     │
│                                          │  ├─ precomputeSdfAsync()       │
│                                          │  └─ updateBBox()              │
│                                          │                               ▼
│                                          │              OpenCL Compile Thread
│                                          │              (nested std::async)
│                                          │                └─ CLProgram::compile()
│                                          │
│                                 Async Render Coroutine Worker
│                                 (AsyncRenderController thread pool)
│                                    │
│                                    ├─ StreamingPreview coroutine  ◄────────┘
│                                    │   ├─ tryToupdateParameter()       [worker queue]
│                                    │   ├─ renderLowResPreview()        [worker queue]
│                                    │   ├─ publish PreviewResultMeta
│                                    │   └─ loop until !m_streamingPreviewActive
│                                    │
│                                    ├─ co_await renderSceneComputeOnly()
│                                    ├─ co_await precomputeSdfAsync()
│                                    └─ co_await updateBBox()
```
