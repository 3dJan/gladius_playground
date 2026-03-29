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

4. `MainWindow::updateModel()` on a subsequent frame, once `m_parameterThrottle.shouldRecompile()` fires (100 ms debounce):
   - Calls `Document::updateParameter()`:
     - `updatePayload()` — acquires compute token (`requestComputeToken()` — returns early if background worker holds the lock), calls `updateParameterRegistration()`, regenerates the flat parameter buffer in CPU memory.
     - `ComputeCore::tryToupdateParameter()`:
       - **`m_computeMutex.try_lock()`** — non-blocking; returns `false` if the worker holds the lock, so this frame is skipped.
       - `updateParameterBlocking()` — iterates all registered parameters, fills a `std::vector<float>`, then calls `paramBuf.write()`.
       - **`paramBuf.write()` uses `clEnqueueWriteBuffer(..., CL_TRUE, ...)`** — synchronous GPU upload, blocks the UI thread until complete. Buffer is small (typically < 1 KB), so the block is short.
   - `updateParameter()` returns `bool` to signal success. If it returns `false` (lock held by worker), **`m_parameterDirty` stays `true`** so the push is retried on the next frame.
   - On success: calls `RenderWindow::invalidateViewDueToParameterChange()`:
     - Marks SDF stale (preserves cached bbox with extra margin).
     - Schedules low-res preview on next frame.
     - Bumps async epoch to cancel any outdated HQ render jobs.

### Deferred Parameter Re-application

When a background compilation is running and the user changes parameters:
1. `nodeEditor()` sets `m_parameterDirty = true` as usual.
2. `updateModel()` tries `Document::updateParameter()`, which fails to acquire the compute token → returns `false`.
3. `m_parameterDirty` remains `true` — the push is retried each frame.
4. After matching finishes, the lock is released, and the next `updateModel()` call succeeds — pushing the latest parameter values (read from node objects) through the current parameter mapping.

### Async work triggered

| Work item | Where it runs |
|---|---|
| Low-res preview render | Async coroutine worker (off UI thread) |
| SDF precomputation | Async coroutine worker → OpenCL event → `co_await` |
| HQ progressive render | Async coroutine worker → OpenCL event → `co_await` |

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

#### Phase B — Worker thread (async)

`Document::refreshWorker()` runs entirely off the UI thread:

| Step | Operation | Notes |
|---|---|---|
| 1 | `waitForComputeToken()` | Blocks worker until GPU mutex available |
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
| `SDFPrecomputation` | Precompute SDF grid on GPU | Enqueued when `m_preComputedSdfDirty` is set |
| `BoundingBox` | Compute/refine bbox from SDF | Enqueued when `m_asyncBboxUpdatePending` is set |
| `LowResPreview` | Immediate visual feedback | Scheduled on `m_forceLowResRenderOnNextFrame` |
| `ProgressiveHQ` | Full-resolution progressive render | Scheduled after SDF is valid |

All jobs run on a worker thread via `co_await waitForEvent(clEvent)`. Results are polled and composited on the UI thread in `processAsyncResults()` / `processAsyncPreviewResults()`.

Epoch-based cancellation: each invalidation bumps `m_asyncEpochCounter`. In-flight jobs with stale epochs are discarded on completion.

## Blocking Operations on the UI Thread

| Operation | When | Severity |
|---|---|---|
| `Buffer::write()` — `clEnqueueWriteBuffer(CL_TRUE)` | Every parameter update | Low (buffer is small, ~microseconds) |
| `Model::updateTypes()` | Every parameter change frame | Medium (graph traversal) |
| `Assembly::updateInputsAndOutputs()` | Every parameter + model change | Medium (iterates all functions/nodes) |
| `Document::updateParameterRegistration()` | Every parameter + model change | Medium (iterates all nodes) |
| `Document::validateAssemblyIfDirty()` | Every frame when graph is dirty | Medium (full validation pass) |
| `Assembly` copy for undo | Every parameter change | Medium (deep copy of entire assembly) |

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
│   ├─ Assembly::updateInputsAndOutputs()      ← synchronous
│   ├─ Document::updateParameterRegistration() ← synchronous
│   └─ refreshModel() [if compile requested]
│       └─ refreshModelAsync()  ──────────────────────┐
│                                                      │
├─ MainWindow::updateModel()                           │
│   ├─ Document::updateParameter()                     │
│   │   └─ paramBuf.write(CL_TRUE)            ← sync  │
│   └─ RenderWindow::invalidateViewDue...()            │
│                                                      │
├─ RenderWindow::renderAsync()                         │
│   ├─ processAsyncResults()                           │
│   └─ enqueue SDF / bbox / render jobs                │
│                                                      ▼
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
│                                 (AsyncRenderController thread)
│                                    ├─ co_await renderSceneComputeOnly()
│                                    ├─ co_await precomputeSdfAsync()
│                                    └─ co_await updateBBox()
```
