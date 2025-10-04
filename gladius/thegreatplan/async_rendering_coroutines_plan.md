# Async Rendering Coroutine Integration Plan

Date: 2025-10-04
Status: Draft

## 1. Purpose
Leverage existing libcoro (C++20 coroutine) infrastructure to implement the async rendering architecture (double/triple buffering, worker offload, minimal UI blocking) using structured, composable coroutine primitives instead of ad-hoc threads, condition variables, and manual state polling.

## 2. High-Level Goals
- Express the render pipeline (Job enqueue → Compute → Publish → Resample → Present) as an explicit coroutine pipeline.
- Reduce manual state machine complexity by using awaitable events (buffer ready, epoch change, cancellation).
- Enable natural cancellation & restart on camera/model changes via coroutine cancellation tokens.
- Facilitate future progressive publication (chunked rendering) with an async generator pattern.
- Preserve zero-overhead path for synchronous fallback (feature flag).

## 3. Architectural Mapping (Concept → Coroutine Pattern)
| Concept | Coroutine Pattern | Rationale |
|---------|-------------------|-----------|
| Worker loop rendering frames | Long-lived task (co_await events) | Natural suspension awaiting new job or free buffer |
| Frame job submission | Fire-and-forget task / scheduler post | Non-blocking enqueue into channel |
| Buffer readiness publication | Event awaitable / promise completion | Resample coroutine awakens when buffer Ready |
| Resample + present | UI-thread bound coroutine (dispatcher) | Guarantees GL affinity |
| Epoch cancellation | Cancellation token propagation | Abort in-flight frame swiftly |
| Progressive chunk notifications (future) | Async generator (co_yield partial progress) | UI can co_await next chunk |
| Triple buffering rotation | Awaitable selecting next Idle buffer | Encodes buffer availability without busy wait |
| Metrics collection | RAII scoped timing inside suspension points | Clean separation of compute vs wait time |

## 4. Key Building Blocks
### 4.1 Coroutine Types
`coro::task<void>` (or project-specific alias) for fire-and-forget tasks.
`coro::generator<Chunk>` (future progressive lines/tiles) for chunk emission.
`coro::shared_task<Result>` if we need multi-consumer access to published frame metadata.

### 4.2 Custom Awaitables
| Awaitable | Purpose | Trigger |
|-----------|---------|---------|
| `await_next_render_job()` | Suspend worker until a new job or cancellation | Job channel push or cancel token |
| `await_idle_buffer()` | Suspend until at least one buffer Idle | Buffer state transition signal |
| `await_frame_ready()` | UI resample coroutine waits for Ready buffer | Worker publishes Ready |
| `await_epoch_change(currentEpoch)` | Detect invalidation while idle | Epoch atomic increment |
| `await_chunk_available()` (future) | Progressive updates | Worker co_yield or set event |

### 4.3 Scheduling Model
- One dedicated worker scheduler (single-thread) executing worker coroutines.
- UI thread uses its event loop; integrates a small trampoline to resume UI-bound coroutines (resample/presentation).
- Optionally unify with existing task executor if available in project.

### 4.4 Cancellation
- `RenderContextCancellationSource` issues cancellation on camera/model change.
- Worker coroutine checks `co_await cancellation_point()` between slice batches.
- Buffer in Writing state on cancel transitions to Idle (discard) before re-loop.

## 5. Data Structures
```cpp
enum class FrameState { Idle, Writing, Ready, Resampling, Front };

struct FrameBuffer
{
    ImageRGBA image;                 // CL-only or staging variant
    std::atomic<FrameState> state{FrameState::Idle};
    std::atomic<uint64_t> frameId{0};
    std::atomic<uint64_t> epoch{0};
    // Metrics
    std::atomic<uint64_t> readyTimestampNs{0};
    std::atomic<uint64_t> publishTimestampNs{0};
};

struct RenderJob
{
    uint64_t epoch; // snapshot
    CameraSnapshot camera;
    RenderingSettings settings;
    // maybe resolution, quality tier
};

struct FrameResultMeta
{
    uint64_t frameId;
    uint64_t epoch;
    uint32_t width;
    uint32_t height;
    bool cancelled{false};
};
```

## 6. Coroutine Flow (Double Buffer Baseline)
```text
submit_job() ─▶ job_channel ─▶ worker_render_loop() ─▶ publish_ready_event
                                       │                      │
                                       │ (await_next_render_job)
UI_resample_loop() ◀─ await_frame_ready() ◀───────────────────┘
```

### 6.1 Worker Coroutine Pseudocode
```cpp
coro::task<void> worker_render_loop(JobChannel & jobs,
                                    Buffers & buffers,
                                    CancellationToken token)
{
    uint64_t nextFrameId = 0;
    while (!token.cancelled())
    {
        auto job = co_await await_next_render_job(jobs, token);
        if (!job) continue; // cancelled

        auto * fb = co_await await_idle_buffer(buffers, job->epoch, token);
        if (!fb) continue; // cancellation during wait

        fb->state.store(FrameState::Writing, std::memory_order_relaxed);

        // Render (could be chunked loop with cancellation points)
        bool cancelled = false;
        cancelled = co_await render_frame_compute_only(*fb, *job, token);
        if (cancelled)
        {
            fb->state.store(FrameState::Idle, std::memory_order_release);
            continue;
        }

        fb->frameId.store(++nextFrameId, std::memory_order_release);
        fb->readyTimestampNs.store(nowNs(), std::memory_order_relaxed);
        fb->state.store(FrameState::Ready, std::memory_order_release);
        signal_frame_ready();
    }
}
```

### 6.2 Resample / Present Coroutine
```cpp
coro::task<void> ui_resample_loop(Buffers & buffers,
                                  CancellationToken token)
{
    FrameBuffer * currentFront = nullptr;
    while (!token.cancelled())
    {
        auto * ready = co_await await_frame_ready(buffers, token);
        if (!ready) continue;

        // Acquire (state Ready -> Resampling)
        if (!try_transition(ready, FrameState::Ready, FrameState::Resampling))
            continue; // lost race, loop again

        // Resample (GL thread)
        resample_to_display(ready->image);

        if (currentFront)
            currentFront->state.store(FrameState::Idle, std::memory_order_release);

        ready->state.store(FrameState::Front, std::memory_order_release);
        currentFront = ready;
    }
}
```

### 6.3 Job Submission (UI Thread)
```cpp
void enqueue_render(CameraSnapshot cam, RenderingSettings s)
{
    RenderJob job{currentEpoch.load(), cam, s};
    jobChannel.push(std::move(job));  // wakes worker coroutine
}
```

## 7. Triple Buffer Extension
- `await_idle_buffer` selects among ≥1 Idle buffers; with triple buffering worker almost never waits.
- Resample loop unchanged; simply more states in rotation.
- Add metric: time between `Ready` publish and `Front` promotion (latency) to evaluate need.

## 8. Progressive Chunk Coroutine (Future)
Implement `coro::generator<Chunk>` inside `render_frame_compute_only`:
```cpp
coro::generator<Chunk> render_frame_chunks(FrameBuffer & fb, RenderJob const & job, CancellationToken t)
{
    for (auto lines : chunked_line_ranges(job))
    {
        if (t.cancelled()) co_return;
        launch_render_kernel(lines);
        clFinish(queue);
        co_yield Chunk{lines.start, lines.end};
    }
}
```
UI could co_await next `co_yield` via adapter that bridges generator to an event.

## 9. Integration Strategy
| Phase | Description | Deliverable |
|-------|-------------|-------------|
| C1 | Introduce coroutine primitives & job channel | Worker coroutine compiling, no rendering yet |
| C2 | Wrap existing synchronous renderScene in coroutine | Equivalent behavior to thread version |
| C3 | Double buffer state machine with coroutine events | Non-blocking UI resample path |
| C4 | Cancellation (epoch) integration | Smooth camera churn handling |
| C5 | Metrics + tracing await points | Idle %, latency charts |
| C6 | Optional triple buffering | Config flag + metrics delta |
| C7 | Progressive chunk prototype (behind flag) | Generator-based partial refresh |

## 10. Channels / Awaitable Implementation Details
### 10.1 Job Channel
Ring buffer + atomic head/tail; waiter coroutine suspends with a promise pointer added to a waitlist; push signals by resuming one waiter.

### 10.2 Frame Ready Event
Single-producer (worker) / single-consumer (UI) event can be an atomic pointer to a suspended coroutine handle; `signal_frame_ready()` swaps and resumes if non-null.

### 10.3 Idle Buffer Awaitable
Poll-free design: worker tries `find_idle()`. If none, it registers itself on a waitlist associated with buffer state transitions; UI / resample sets buffer Idle and resumes one waiter.

### 10.4 Cancellation Points
Lightweight check macro:
```cpp
#define CORO_CANCEL_POINT(tok) if (tok.cancelled()) co_return true;
```
Used after each chunk / major kernel.

## 11. Memory Ordering Considerations
| Operation | Memory Order | Notes |
|-----------|--------------|-------|
| Publish frameId/state Ready | release | Ensure pixel writes + metadata visible |
| Acquire Ready in UI | acquire | Reads consistent image + metadata |
| Transition Ready→Resampling→Front | acq_rel | Atomic CAS ensures exclusive ownership |
| Idle store after Front replaced | release | Worker sees Idle reliably |

## 12. Error Handling
Wrap kernel launches; on exception:
1. Mark buffer Idle.
2. Increment failure counter.
3. If threshold exceeded, signal fallback (disable async mode) via coroutine that posts a UI notification.

## 13. Metrics & Instrumentation Points
| Metric | Collection Point |
|--------|------------------|
| renderComputeMs | Around `render_frame_compute_only` body |
| frameLatencyMs | Ready publish vs Front promotion |
| workerIdlePct | Time awaiting job or idle buffer vs elapsed |
| cancellations | Count epoch-based aborts |
| resampleMs | Wrap resample_to_display |
| chunkIntervalMs (future) | Time between successive chunk yields |

## 14. Testing Strategy
| Test | Focus | Notes |
|------|-------|-------|
| Ctor/Shutdown | Leak & deadlock free | Repeated init/teardown loops |
| Single Frame | Basic flow Ready→Front | Deterministic camera |
| Rapid Epoch | Cancellation robustness | Flood camera changes |
| Buffer Rotation | Triple buffer correctness | Force large resample delay |
| Fallback Path | Disable async after failures | Inject synthetic kernel error |
| Progressive (future) | Partial updates order | Compare final frame hash |

## 15. Rollback / Feature Flags
- `asyncRendering.coroutines.enabled` (overall).
- `asyncRendering.buffering = double|triple`.
- `asyncRendering.progressive = off|lines|tiles`.
- Safe rollback: disable coroutines → revert to legacy threaded path.

## 16. Risks & Mitigations (Coroutine-Specific)
| Risk | Impact | Mitigation |
|------|--------|------------|
| Hidden latency in await loops | Medium | Instrument all await points with tracing |
| Coroutine handle leaks | High | RAII scope guards / tests with sanitizer |
| Excess context switching | Low/Med | Combine small awaits; tune chunk size |
| Starvation (single waiter resumed repeatedly) | Low | Fair waitlist ordering |
| Complex debugging | Medium | Structured logging: frameId, epoch, state transitions |

## 17. Acceptance Criteria
- Baseline (C3) shows functional parity with thread version; metrics identical or improved idle time.
- Cancellation under rapid camera movement: no crashes, average stale frame latency < 1 frame interval.
- No memory leaks (ASan clean) after 100 repeated start/stop cycles.
- Triple buffering reduces worker idle % by ≥ 30% relative to double when resample > 3ms median.

## 18. Next Steps
1. Implement C1–C3 branches behind feature flag.
2. Add metrics dashboard overlay (optional).
3. Evaluate whether progressive chunking justified by user experience.

---
*Prepared as coroutine-focused augmentation of async rendering plan.*
