# Async Rendering Plan (Attempt 2)

## 1. Objectives
- Execute the heavy preview rendering work (`ComputeCore::renderScene`) off the UI thread.
- Keep the lightweight finishing step (`RenderProgram::resample`) on the UI thread to avoid OpenGL context conflicts.
- Preserve visual parity with the current synchronous renderer while improving UI responsiveness.
- Provide a feature flag and clean rollback path back to the existing synchronous path.

## 2. Constraints & Lessons Learned
- OpenGL resources are thread-affine. Only the UI thread currently owns a valid GL context, so any off-thread work must avoid GL calls unless we explicitly create and manage a shared context.
- `renderScene` today mixes OpenCL and OpenGL interop. We must isolate the pure compute part before moving it to a worker thread.
- Documentation and telemetry must accurately reflect behaviour; we cannot advertise async benefits unless they are measurable.
- Maintainability matters: avoid complex state machines that are hard to debug under contention.

## 3. High-Level Architecture
```
UI Thread                                          Worker Thread
---------                                          --------------
RenderWindow::renderAsync()                       AsyncRenderWorker loop
   │                                                   │
   ├─ Submit RenderJob (camera, settings) -------------→│ (queue latest only)
   │                                                   │
   ├─ Poll worker state / check Ready buffers          │
   │                                                   ├─ Acquire compute slot (no UI blocking)
   │                                                   ├─ Invoke ComputeCore::renderScene()
   │                                                   ├─ (Kernels write Back buffer)
   │                                                   ├─ Finish / event callback
   │                                                   └─ Publish Ready (frameId, epoch)
   │
   ├─ When Ready buffer available:
   │     ├─ (Optional short atomic swap only – no heavy token)
   │     ├─ Map/copy staging (if used)
   │     ├─ Run RenderProgram::resample() (UI thread)
   │     └─ Present frame & update metrics
   │
   └─ If worker busy: reuse last Front / show low-res preview

Triple buffering (if enabled) adds an intermediate Mid state so worker can start next frame while resample runs.
```

### Optional Extension: Triple Buffering for Overlap
To maximize overlap between compute (`renderScene`), resample, and display, we can extend the double-buffer plan to a triple-buffer rotation:

Buffer Roles:
- Front: currently displayed (stable) frame.
- Mid: fully rendered, currently being resampled / uploaded.
- Back: being written by worker (in-progress).

State Machine (per buffer): `Idle → Writing → Ready → Resampling → Front → Idle` (cyclic reuse).

Benefits:
- Eliminates worker idle time while UI is resampling a finished frame.
- Smoother frame cadence (reduced UI jitter if resample occasionally spikes).
- Foundation for optional progressive chunk publication later (line/tile granularity).

Trade-offs / When to Defer:
- +1 extra frame of GPU memory (~size_of_frame buffer).
- Slightly higher complexity (buffer index and state management).
- Progressive chunking still optional; start with whole-frame publication.

Adoption Strategy:
1. Implement double buffering first (Front/Back).
2. Add third buffer and promote Ready→Resampling without blocking worker.
3. Instrument idle time & frame latency; enable triple buffering only if it yields measurable benefit.

Configuration:
- `asyncRendering.buffering`: `double` (default) | `triple`.
- Fallback automatically to `double` if memory allocation fails.

Metrics to Track:
- Worker idle percent.
- Resample duration vs compute duration.
- Frame staleness (displayedFrameId difference to latest Ready frameId).

Epoch Handling:
- On camera/model invalidation, increment `epoch`; discard buffers with stale epoch before promotion.

This extension is optional for the initial async release but documented here to avoid architectural dead-ends.

## 4. Implementation Phases

### Phase 0 – Preparation (0.5 day)
1. **Stabilise baseline**: ensure synchronous renderer remains untouched and behind feature flag.
2. **Instrument**: add Tracy zones or simple timers for `renderScene` and `resample` to compare before/after.
3. **Design doc review**: circulate this plan, collect feedback, lock scope.

### Phase 1 – Compute/GL Separation (1.5 days)
1. **Refactor `ComputeCore::renderScene`**
   - Extract the OpenCL portion into `renderSceneComputeOnly(job, RenderBuffers &out)` that does not touch GL.
   - Guard existing GL interop path with feature flag; reuse for synchronous mode.
2. **Introduce `RenderBuffers` abstraction**
   - Contains CL image + optional mapped host staging buffer.
   - Provides `copyToHost()` for worker thread and `blitToTexture(GLTexture&)` for UI thread.
3. **Implement staging path**
   - Use CL/GL shared image when on UI thread.
   - For worker path, read into pinned host memory (to avoid GL context usage).
4. **Unit tests / smoke tests**: confirm synchronous path still works using staging code paths (run feature flag in "sync-using-staging" mode).

### Phase 2 – Worker Thread Integration (2 days)
1. **Async infrastructure**
   - Create `AsyncRenderWorker` with thread lifecycle, queue, and metrics (new files `AsyncRenderWorker.{h,cpp}`).
   - Worker receives `RenderJob` (camera, slice range, quality flags) and outputs `RenderJobResult` (host buffer handle + metadata).
2. **Thread safety**
   - Worker obtains compute token (blocking). Ensure token acquisition is re-entrant safe (no UI thread re-entry).
   - Main thread only acquires token briefly during resample phase.
3. **RenderWindow integration**
   - On dirty state, enqueue job if worker idle (double buffer baseline).
   - When result available: map buffer → create/update `GLImageBuffer`, run `resample`, release (Front/Back swap).
   - Maintain progressive line counters (`currentLine`, `renderingStepSize`).
   - (Optional later) If triple buffering enabled, introduce Mid buffer state to overlap resample with next compute cycle.
4. **Feature flag**
   - Config key `asyncRendering.mode`: `disabled` | `sync-staged` | `async`.
   - CLI override / environment variable for testing.

### Phase 3 – Resample & Presentation Path (1 day)
1. Ensure `RenderProgram::resample` always executes on UI thread.
2. Optimize buffer upload: reuse texture objects, avoid reallocations.
3. Add fallbacks: if resample fails, mark frame as dropped and keep last good frame.
4. UI metrics display: show separate compute time vs resample time.

### Phase 4 – Validation & Hardening (1.5 days)
1. **Performance benchmarks**
   - Compare frame time distribution sync vs async on reference scenes.
   - Measure worker throughput (ms per slice) and UI stall time.
2. **Stress tests**
   - Rapid camera movement, window resize, model changes, concurrent compilation.
   - Simulate worker failure (exceptions) and verify graceful fallback.
3. **Documentation**
   - Update user manual, developer notes, diagrams.
4. **Telemetry gates**
   - Disable async mode automatically if worker crashes more than N times per session.

## 5. Work Breakdown Structure (WBS)
| ID | Task | Owner | Est. |
|----|------|-------|------|
| P0.1 | Capture baseline metrics & tracepoints | Dev | 0.5d |
| P1.1 | Extract compute-only renderScene core | Dev | 1d |
| P1.2 | Introduce staging buffer abstraction | Dev | 0.5d |
| P2.1 | Implement `AsyncRenderWorker` infrastructure | Dev | 1d |
| P2.2 | Integrate worker with `RenderWindow` | Dev | 1d |
| P3.1 | Main-thread resample/upload path | Dev | 0.5d |
| P4.1 | Performance + stress validation | QA | 1d |
| P4.2 | Documentation & release notes | Dev | 0.5d |

## 6. Risks & Mitigations
| Risk | Impact | Mitigation |
|------|--------|------------|
| Host staging too slow | Medium | Use pinned memory, align buffers, allow configurable slice height |
| Token contention causes regressions | High | Measure token acquisition time; if UI stalls > threshold, drop to sync mode |
| Memory footprint increases | Medium | Pool staging buffers, release when idle |
| Worker exceptions crash app | High | Wrap worker loop in try/catch; signal failure and fall back |
| GL texture upload still expensive | Medium | Batch uploads, reuse textures, reduce resample resolution while moving |
| Triple buffering adds complexity without gain | Low/Medium | Instrument worker idle % & staleness; only enable if measurable improvement; feature flag (`asyncRendering.buffering`) |

## 7. Success Criteria
- Async mode reduces UI thread render time (excluding resample) by ≥50% versus sync baseline on benchmark scenes.
- Visual output matches synchronous path (diff tests/spots).
- No new crashes reported during stress testing.
- Feature flag defaults to disabled but can be shipped to beta testers with documented benefits.

## 8. Rollback Plan
- Keep original synchronous code path intact and selectable via config.
- If async staging fails at runtime, automatically revert to synchronous path and notify user via status banner/log.
- Maintain per-session metrics to track fallback frequency.

## 9. Follow-Up Enhancements (Post Plan)
- Investigate shared OpenGL context to eliminate CPU staging copy (Phase 2 of long-term roadmap).
- Explore tile-based rendering in worker to improve progressive refinement order.
- Integrate Tracy or ETW capture for long-term telemetry.
- Optional: Enable triple buffering & progressive chunk publication if metrics show benefit.

## 10. Render Pass Execution & Data Dependencies

### 10.1 Goals
Provide a precise step-by-step description of: (a) how a frame (progressive or full) is produced off the UI thread, (b) how the UI consumes it, and (c) how we avoid conflicting memory access with minimal blocking.

### 10.2 Resource Classification
| Resource | Type | Access Pattern | Thread Ownership | Hazard Notes |
|----------|------|----------------|------------------|--------------|
| Geometry primitives / parameter buffers | Read-only per frame after upload | R | Worker + UI (indirect) | Immutable while rendering a frame |
| Rendering settings snapshot | Read-mostly | R/W (atomic copy at frame start) | Worker copies from UI | Copy on job enqueue |
| Precomputed SDF buffer | Potentially updating between frames | R (worker) / R (UI query) | Worker updates outside render, guarded by token (legacy) | Avoid mutation during renderScene or stage completion |
| Compute image buffers (Front/Back[/Mid]) | Frame output | W (worker), then R (UI resample) | Ownership rotates | Must not read while Writing |
| GL display texture | Presentation target | W (UI) | UI thread only | No off-thread GL calls |
| Staging host buffer (optional) | Intermediate | W (worker), R (UI) | Shared | Visibility only after kernel completion |
| Frame metadata (frameId, epoch, timings) | Control | W (worker publish), R (UI) | Shared (atomics) | Use release/acquire ordering |

Legend: R = read, W = write.

### 10.3 Frame Epoch & Cancellation
- `epoch` increments on camera/model/parameter invalidation.
- Worker captures `localEpoch` at job start; if mismatch detected mid-render (e.g. per-chunk), it aborts buffer (state → Idle) and restarts with fresh snapshot.

### 10.4 Double Buffer Render Pass (Baseline)
1. UI detects dirty state and enqueues `RenderJob` with camera + settings snapshot.
2. Worker selects Back buffer (state Idle) → state = Writing.
3. Worker enqueues `renderSceneComputeOnly()` kernels into its command queue.
4. On completion (queue.finish or final event callback):
   - Write frame metadata (frameId, epoch, timings) (relaxed writes allowed before final publish).
   - Atomically set buffer state = Ready (store release).
5. UI each frame:
   - Acquire Ready buffer with latest frameId (load acquire) if not currently resampling.
   - Map/copy (if staging) and invoke `RenderProgram::resample()` into GL texture.
   - Swap: previous Front → Idle; Ready → Front.
6. Display uses GL texture; Back buffer becomes available for next job.

Blocking points:
- Worker blocks only at kernel completion (optionally replaced by event callback later).
- UI does not block on compute; only performs quick atomic loads/stores and resample.

### 10.5 Triple Buffer Variant (If Enabled)
Adds Mid buffer for overlap:
1–4 same as double buffering (Writing → Ready).
5. UI promotes newest Ready buffer to Resampling (CAS Ready→Resampling) while Front remains displayed.
6. After resample completes: Front → Idle, Resampling → Front.
7. Worker can start Writing on any Idle buffer immediately (often the one just vacated by Front) without waiting for resample.

### 10.6 Progressive (Optional Future)
For chunked publication (lines/tiles):
- Worker increments `publishedHeight` (atomic) after finishing a monotonic Y-range.
- UI may resample only new region `[prevHeight, publishedHeight)` if buffer state is Writing but epoch matches and partial updates are enabled.
- Risk: Frequent synchronization; start with whole-frame publication.

### 10.7 Synchronization & Memory Ordering
| Action | Ordering | Rationale |
|--------|----------|-----------|
| Worker writes pixels | (normal CL completion) | GPU ensures memory after kernel finish |
| Worker sets frame metadata | Before state publish | Ensure metadata visible with frame |
| Worker publishes Ready (state store) | `memory_order_release` | Guarantees prior metadata visible to UI |
| UI loads state | `memory_order_acquire` | Sees metadata + finished pixels |
| UI state transition Ready→Front | CAS with acquire-release | Prevents torn transitions |
| UI marks old Front Idle | release | Next worker cycle sees Idle |

### 10.8 Data Hazards & Avoidance
| Hazard | Scenario | Prevention |
|--------|----------|------------|
| Read-after-write race | UI resamples while worker still writing | UI only uses buffers in Ready/Resampling/Front states; worker transitions Writing→Ready after completion |
| Write-after-read | Worker overwrites Front before UI finished resample | UI holds Front; worker never picks Front (state not Idle) |
| Stale frame display | Object changes mid-render | Epoch check aborts Writing buffer; Ready with stale epoch skipped |
| Memory blow-up | Triple buffering + staging | Configurable mode; allocation fallback to double; pooling |
| Token stall (legacy) | UI waiting for compute token during resample | Remove token need for resample once buffers are isolated |

### 10.9 Minimal Blocking Strategy
- Replace coarse compute token with fine-grained buffer state machine.
- Use per-thread OpenCL queues (already present) to avoid contention.
- Only blocking call in v1: `queue.finish()` inside worker on frame completion (optimizable via event callback).
- No UI thread blocking except normal GL work.

### 10.10 Metrics for Validation
- `workerIdlePct = idleTime / totalTime` (target < 5% with triple buffering, <15% double).
- `uiResampleTimeMs` (track spikes; consider downscaling on spikes > threshold).
- `frameLatency = displayTime - readyTime`.
- `staleFramesSkipped` (should drop with epoch logic).

### 10.11 Rollout Sequence
1. Implement double buffering state machine & metrics.
2. Verify zero visual regressions.
3. Add epoch cancellation.
4. Add optional triple buffering behind config flag.
5. Evaluate metrics; enable triple buffering by default only if idle reduction is significant.

This section formalizes execution semantics to guide implementation and testing rigorously.

---
*Prepared: 2025-10-04*
