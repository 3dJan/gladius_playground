# Rendering Coupling Analysis

Date: 2025-10-04  
Author: Planning Assistant  
Status: Draft (v1.0)

---
## 1. Executive Summary
The current Gladius preview rendering pipeline is **tightly coupled** to the main UI thread. Rendering, OpenCL computation, OpenGL interop, UI layout and frame timing all execute serially inside the GLFW main loop. While mitigation strategies (progressive rendering, adaptive quality, non-blocking compute token attempts) reduce hard stalls, the architecture still:

- Blocks the UI thread for significant portions of a frame (5–15ms+ per render increment)
- Performs synchronous GPU fences (`glFinish`) and texture transfers on the UI thread
- Uses a single recursive mutex (`m_computeMutex`) to guard *all* compute state (compilation, primitives, SDF precomputation, rendering)
- Interleaves *render scheduling*, *render execution*, and *UI composition* without a decoupling boundary

This document maps the coupling points, quantifies impact, and proposes a phased decoupling roadmap (Async Compute → Shared GL Context → (Optional) Modern GPU API Migration). The recommended immediate action is **Phase 1: Async Render Manager (compute off main thread, UI-only texture upload)**—low risk with clear responsiveness benefits.

---
## 2. Current Rendering Flow Overview
```
GLFW Main Loop (GLView::startMainLoop)
  ├─ Event polling (glfwPollEvents / WaitEvents)
  ├─ glClear(...)
  ├─ GLView::render()
  │    ├─ glfwMakeContextCurrent()
  │    ├─ m_render()  --> MainWindow::render()
  │    │     ├─ renderWindow() --> RenderWindow::renderWindow()
  │    │     │      ├─ Try compute token
  │    │     │      ├─ If token: RenderWindow::render(state)
  │    │     │      │       ├─ Progressive / Low-res logic
  │    │     │      │       └─ Calls ComputeCore::renderScene() / renderLowResPreview()
  │    │     └─ Other UI panels
  │    ├─ glFlush / glFinish (blocking)
  │    ├─ displayUI() (ImGui draw submission)
  │    └─ glfwSwapBuffers() (may vsync-block)
  └─ Frame pacing sleep
```

---
## 3. Key Coupling Points
| # | Location | Mechanism | Impact |
|---|----------|-----------|--------|
| 1 | `GLView::startMainLoop` | Single-thread serial loop | No pipeline overlap |
| 2 | `GLView::render` | Synchronous `m_render()` + GPU fences | Forces CPU idle on GPU completion |
| 3 | `MainWindow::render` | Intermixed UI + resource validation + compute token | UI logic complexity & timing jitter |
| 4 | `RenderWindow::renderWindow` | Compute token gating inside UI pass | Frame variance (skipped draws) |
| 5 | `RenderWindow::render` | Progressive & adaptive quality executed inline | Frame time tied to compute complexity |
| 6 | `ComputeCore::renderScene` | `try_lock` + blocking kernels + `glFinish` | Latent stalls; monolithic lock |
| 7 | OpenCL–OpenGL interop | Shared GPU resources tied to UI thread context | Limits thread relocation of rendering |

---
## 4. Detailed Analysis
### 4.1 GL Main Loop (Coupling Root)
- Fixed ordering: Events → Clear → Full Render/Compute → UI finalize → Swap.
- No asynchronous staging or double buffering of GPU work.
- Frame pacing uses sleep, but render duration variance propagates directly to perceived responsiveness.

### 4.2 Context Binding and GPU Fences
- `glfwMakeContextCurrent` each frame asserts main thread ownership.
- `glFinish` guarantees GPU completion before UI draw phase—prevents overlapped CPU preparation.
- Potential optimization: Replace unconditional `glFinish` with finer sync (e.g., fence objects only where interop requires data integrity).

### 4.3 Compute Token Pattern
- `requestComputeToken()` uses `try_lock` to avoid blocking UI.
- When contested (e.g., compilation or slicing), preview rendering silently skips a frame—causing inconsistent update cadence.
- Single recursive mutex protects: program (re)compilation, primitives, SDF precomputation, line rendering, preview resampling—high contention domain.

### 4.4 Progressive Rendering Strategy
- Renders vertical bands (`startLine` → `endLine`) adaptively adjusting `renderingStepSize` based on measured duration.
- PID tuning for quality while moving vs static ensures interactive rotation/zoom at low fidelity.
- Limitations: Still synchronous; cannot utilize spare cores or overlap with UI input sampling.

### 4.5 Low-Res Preview Path
- Low-res buffer rendered then upsampled to primary result image.
- Executed inline on UI thread; upsampling & precompute gating add latency if invoked mid-interaction.

### 4.6 OpenCL/OpenGL Interop Constraints
- Interop buffers (textures) require GL context validity; presently only main thread context is active.
- Moving rendering off-thread needs either:
  - Shared context creation (platform-dependent reliability), or
  - Off-thread pure OpenCL + CPU staging + main-thread GL upload (higher bandwidth cost), or
  - Alternative API supporting multi-queue concurrency (e.g., Vulkan compute + graphics).

### 4.7 Monolithic Lock Scope
- `m_computeMutex` covers large swaths of unrelated operations (rendering, compilation, bounding box updates).
- Prevents parallelization of light queries (e.g., fetching bounding box) during heavy kernels.
- Re-entrancy via `std::recursive_mutex` increases risk of accidental nested acquisition patterns hiding performance pitfalls.

---
## 5. Performance Impact Outline
Approximate main-thread time slices (scenario-dependent):
| Component | Typical | Worst | Notes |
|-----------|---------|-------|-------|
| Event Poll / Dispatch | 0.2–1 ms | 2 ms | Input + windowing |
| UI Logic & Layout | 1–3 ms | 6 ms | ImGui docking, panels |
| Progressive Render Chunk | 4–12 ms | 18 ms | Depends on step size & GPU | 
| Low-Res Preview (moving) | 1–4 ms | 6 ms | Includes resample |
| GPU Sync (`glFinish`) | 0–5 ms | 8 ms | Driver & workload dependent |
| Texture Invalidate/Bind | 0.3–1 ms | 2 ms | GL-CL interop mapping |
| ImGui Draw Submission | 0.5–1.5 ms | 3 ms | Command generation |
| Swap (vsync) | 0–16 ms | 16 ms | If early finish pre-vsync |

Observations:
- Jitter arises when progressive chunk overruns nominal budget.
- Quality adaptation reduces resolution aggressively (e.g., 0.02f factor) but still executes synchronously.
- True frame parallelism is absent.

---
## 6. Root Causes of Tight Coupling
| Cause | Description | Consequence |
|-------|-------------|-------------|
| Single-thread loop | No task decomposition | Coarse latency spikes |
| Monolithic compute lock | Over-serialization of heterogeneous tasks | Lost concurrency potential |
| Immediate interop reliance | Requires GL context-located compute | Hard to offload |
| Blocking GPU fences | Forces CPU stalls | Wasted CPU cycles |
| Mixed responsibilities | UI + scheduling + compute decisions interleaved | Hard to refactor incrementally |

---
## 7. Decoupling Strategy (Phased)
### Phase 1: Async Compute (CPU Staging)
**Goal:** Offload OpenCL kernel execution to a dedicated compute thread. UI thread only uploads finished frames.

**Key Elements:**
- Add `AsyncRenderManager` with request queue & worker thread.
- Worker acquires compute lock (blocking permitted off-UI) and renders into a CPU buffer (e.g., mapped staging or host-readable CL buffer).
- UI thread checks ready frames, uploads via `glTexSubImage2D`.

**Pros:** Low risk, isolates render time from UI.  
**Cons:** Extra memory bandwidth (GPU→CPU→GPU).  
**Mitigation:** Use pinned memory to reduce transfer overhead.

### Phase 2: Shared GL Context + Double Buffering
**Goal:** Eliminate CPU round-trip; render directly into GL textures off-thread.

**Requirements:**
- Create hidden shared GLFW window / context with `share` parameter.
- Introduce `DoubleBufferedTexture` abstraction (front = displayed, back = written).
- Use GL sync objects (`glFenceSync`, `glClientWaitSync`) or atomic state with `GL_ARB_sync`.

**Pros:** Retains existing OpenCL/OpenGL interop assets; high performance.  
**Cons:** Driver variability, more complex lifecycle management.

### Phase 3 (Optional): Modern API Migration (Vulkan or OpenCL + EGL Offscreen)
**Goal:** Gain explicit queue control and true async pipelines.  
**Pros:** Fine-grained synchronization, timeline semaphores, unified memory mgmt.  
**Cons:** Large refactor scope; toolchain changes.

---
## 8. Proposed Async Interface (Phase 1)
```cpp
struct RenderRequest {
    uint64_t frameId;
    bool lowRes;
    float qualityScale;
    CameraState camera;
};

struct RenderResult {
    uint64_t frameId;
    bool isLowRes;
    std::vector<uint8_t> rgba; // width*height*4
    int width;
    int height;
};

class AsyncRenderManager {
public:
    void start();
    void stop();
    void submit(RenderRequest req); // replaces direct progressive invocation
    std::optional<RenderResult> tryAcquireLatest();
private:
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::deque<RenderRequest> m_requests;
    std::atomic<bool> m_running{false};
    std::mutex m_resultMutex;
    std::optional<RenderResult> m_latest; // overwrite-only
};
```

**UI Thread Adaptation:**
- Replace `RenderWindow::render(state)` with:
  1. Submit new request on invalidation or movement.
  2. Poll `tryAcquireLatest()` before drawing preview panel.

**Progressive Continuation:**
- Each background render can still implement line-chunk stepping internally; only finished partial frames that improved the image are published.

---
## 9. Lock Decomposition Plan
| Current Lock | Issue | Action |
|--------------|-------|--------|
| `m_computeMutex` | Over-wide critical sections | Split into: `programMutex`, `primitivesMutex`, `renderMutex` |
| Recursive usage | Masks re-entrancy | Convert to plain `std::mutex` after scoping cleanup |
| Token pattern conflates responsibilities | UI query vs heavy compute | Introduce lightweight read caches for metadata (bb, readiness flags) |

---
## 10. Risk & Mitigation
| Risk | Phase | Mitigation |
|------|-------|-----------|
| Frame tearing / partial frame display | 1 & 2 | Publish only complete frames (atomic swap) |
| Increased latency (CPU staging) | 1 | Limit staged resolution while moving |
| Driver incompatibility (shared contexts) | 2 | Capability probe + fallback to Phase 1 path |
| Deadlocks due to new threads | 1–2 | Strict lock ordering; watchdog logging for blocked > N ms |
| Memory spikes (double buffers) | 2 | Adaptive resolution cap; reuse pools |

---
## 11. Metrics & Instrumentation
Add Tracy (or equivalent) zones for:
- `ComputeCore::renderScene`
- `RenderWindow::render` (pre/post compute decision)
- `AsyncRenderManager` queue wait time
- Mutex contention (custom counters) 

Expose runtime stats in a new "Performance" debug panel:
| Metric | Source |
|--------|--------|
| Frame time (avg/p99) | Main loop timestamps |
| Render queue depth | Async manager |
| Last compute duration | Measured per frame |
| Skipped frames (token fail) | Counter increment |
| Resolution scalars | Render state |

---
## 12. Testing Strategy
### 12.1 Functional
- Ensure identical visual output (static scene) between synchronous and async modes.
- Validate low-res preview transitions to high-res within expected latency bounds.

### 12.2 Performance
- Benchmark median & tail frame times pre/post Phase 1.
- Measure throughput under camera orbit stress (continuous movement for 10s).

### 12.3 Concurrency
- Intentional contention test: Force program recompilation while continuously rotating camera.
- Re-entrancy probes: Simulate rapid invalidate signals (100 Hz) while rendering.

### 12.4 Fallback Robustness
- Simulate failure to start worker thread → gracefully revert to synchronous path.
- Simulate buffer allocation failure → degrade to lower resolution.

---
## 13. Incremental Migration Checklist
| Step | Description | Done Criteria |
|------|-------------|---------------|
| 1 | Introduce `AsyncRenderManager` skeleton | Compiles, no effect when disabled |
| 2 | Add CPU staging path & publish frames | Preview still updates; correctness diff tested |
| 3 | Integrate invalidation & movement triggers | Movement continuously updates low-res |
| 4 | Remove direct calls to `renderWindow().render()` compute path | All compute via async manager |
| 5 | Add perf panel & metrics | Panel shows live data |
| 6 | Optional: Shared context POC | Renders with same visual output; benchmark win |

---
## 14. Future Optimizations (Post-Decoupling)
| Idea | Benefit |
|------|---------|
| Tile-based importance rendering (center-first) | Faster perceived refinement |
| Temporal reprojection of low-res frames | Hide latency during HQ convergence |
| Adaptive kernel scheduling (dynamic chunk size off-thread) | Reduces quality oscillation |
| Multi-queue OpenCL (preview queue vs HQ queue) | Prioritized responsiveness |
| Partial SDF region invalidation tracking | Fewer full-frame recomputations |

---
## 15. Decision Summary
| Option | Adopt? | Rationale |
|--------|--------|-----------|
| Phase 1 Async (CPU staging) | YES (immediate) | Low risk, unlocks responsiveness gains |
| Shared GL Context | CONDITIONAL | Implement if perf gap remains significant |
| Vulkan Migration | DEFERRED | High cost; revisit after stability of async pipeline |

---
## 16. Actionable Next Steps (Week-Level)
| Week | Focus |
|------|-------|
| 1 | Implement & integrate `AsyncRenderManager` (flag-gated) |
| 2 | Remove direct synchronous progressive path / add metrics panel |
| 3 | Optimize chunk scheduling & add lock decomposition groundwork |
| 4 | Prototype shared context (measure vs baseline) |

---
## 17. Open Questions
| Question | Notes |
|----------|-------|
| Is CPU staging bandwidth acceptable at high resolutions (e.g., 4K)? | Gather empirical metrics |
| Are there driver issues with GL sharing on target deployment environments? | Build compatibility matrix |
| Should progressive line rendering logic move entirely into async thread or be replaced by tile/region strategy? | Evaluate after Phase 1 |

---
## 18. Glossary
| Term | Definition |
|------|------------|
| Progressive Rendering | Incremental image refinement across frames |
| Low-Res Preview | Fast approximate pass used during interaction |
| Token Pattern | Non-blocking lock acquisition using optional RAII guard |
| Interop | Shared resource model between OpenCL and OpenGL |

---
## 19. References
- `GLView.cpp` (main loop & render orchestration)
- `MainWindow.cpp`, `RenderWindow.cpp` (UI + render trigger logic)
- `ComputeCore.cpp` (renderScene, renderLowResPreview, token methods)
- `Rendering.cpp/h` (resource ownership and interop logic)
- Planning docs: `concurrent_architecture_plan.md`, `implementation_guide.md`
- Khronos specs: OpenCL/GL sharing & sync primitives

---
## 20. Conclusion
The synchronous, single-threaded rendering architecture has reached diminishing returns from incremental optimizations (progressive sampling, adaptive quality). Strategic decoupling—beginning with an asynchronous compute execution layer—will measurably improve UI responsiveness, reduce frame time jitter, and establish a scalable foundation for future GPU pipeline evolution. The recommended approach is intentionally incremental, preserving correctness while creating clear seams for subsequent performance investments.

> Proceed with Phase 1 implementation behind a feature flag (e.g., `EnableAsyncPreviewRendering`) to allow A/B validation and rapid rollback.

---
*End of Document*