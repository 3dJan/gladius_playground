# Async Rendering Threaded Implementation Plan (No Coroutines)

Date: 2025-10-04
Status: Draft

## 1. Purpose
Provide a detailed plan to implement the async rendering architecture (double / triple buffering, minimal UI blocking, epoch cancellation) using standard C++ threads, atomics, condition variables, and lock-free patterns—without coroutines. Serves as an alternative or baseline path alongside the coroutine plan.

## 2. Design Principles
- Keep UI thread free from heavy compute; only resample + present.
- Avoid busy waiting; use condition variables or futex-like primitives sparingly.
- Minimize lock contention (prefer atomic state transitions; single mutex for job queue).
- Maintain observability (metrics for idle, latency, throughput).
- Straightforward rollback to fully synchronous path.

## 3. Components Overview
| Component | Responsibility | Thread |
|-----------|----------------|--------|
| `RenderJobQueue` | Stores latest or FIFO render jobs | UI → Worker
| `FrameBufferPool` | Manages 2 or 3 frame buffers & states | Shared
| `AsyncRenderWorker` | Dedicated thread executing renderSceneComputeOnly | Worker thread
| `ResampleScheduler` | UI-side logic deciding when to resample Ready buffer | UI thread
| `EpochManager` | Tracks invalidations (camera/model/params) | UI + Worker (atomic)
| `MetricsCollector` | Aggregates timing counters | Shared
| `FailureMonitor` | Tracks consecutive failures; triggers fallback | Shared

## 4. Data Structures
```cpp
enum class FrameState : uint8_t { Idle, Writing, Ready, Resampling, Front };

struct FrameBuffer
{
    ImageRGBA image;                 // Compute output (CL image or host staging)
    std::atomic<FrameState> state{FrameState::Idle};
    std::atomic<uint64_t> frameId{0};
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint64_t> readyTimestampNs{0};
    std::atomic<uint64_t> publishTimestampNs{0};
};

struct RenderJob
{
    uint64_t epoch;
    CameraSnapshot camera;
    RenderingSettings settings;
};

class RenderJobQueue
{
public:
    void pushLatest(RenderJob job);
    bool tryPop(RenderJob & out); // returns false if no job
private:
    std::mutex m_mutex;
    std::optional<RenderJob> m_latest; // last-wins strategy
};
```

## 5. Buffer State Machine
Transitions (all via atomics; CAS where needed):
`Idle -> Writing -> Ready -> (UI CAS) Resampling -> Front -> Idle`

Rules:
- Worker only selects buffers in Idle.
- UI only resamples buffers in Ready.
- Only one Front at a time.
- Triple buffering: at most one Resampling; worker ignores Front & Resampling buffers.

## 6. Job Flow (Double Buffer Baseline)
1. UI: Detect dirty → build `RenderJob` (snapshot) → `pushLatest()`.
2. Worker thread loop: `tryPop()`; if job present:
   - Obtain Idle buffer.
   - Mark Writing.
   - Execute `renderSceneComputeOnly` (slice loop if progressive not yet).
   - On success: set metadata, state = Ready (release).
   - On cancellation (epoch mismatch): state = Idle.
3. UI frame tick:
   - If Ready buffer exists (choose highest frameId), CAS Ready→Resampling.
   - Perform resample/upload.
   - Previous Front (if any) state Front→Idle.
   - New buffer state Resampling→Front.
4. Present.

## 7. Worker Thread Loop Pseudocode
```cpp
void AsyncRenderWorker::run()
{
    setThreadName("RenderWorker");
    uint64_t frameCounter = 0;
    while (!m_shutdown.load())
    {
        RenderJob job;
        if (!m_jobQueue.tryPop(job))
        {
            waitForJobOrShutdown(); // condvar wait with timeout (metrics idle time)
            continue;
        }

        if (job.epoch != m_epoch.load(std::memory_order_acquire))
            continue; // stale job

        FrameBuffer * fb = acquireIdleBuffer(); // spin-short then sleep
        if (!fb) continue; // shutting down

        fb->state.store(FrameState::Writing, std::memory_order_relaxed);
        fb->epoch.store(job.epoch, std::memory_order_relaxed);

        bool cancelled = false;
        cancelled = !renderFrameComputeOnly(job, *fb); // returns false if epoch changed mid-loop
        if (cancelled)
        {
            fb->state.store(FrameState::Idle, std::memory_order_release);
            continue;
        }
        fb->frameId.store(++frameCounter, std::memory_order_release);
        fb->readyTimestampNs.store(nowNs(), std::memory_order_relaxed);
        fb->state.store(FrameState::Ready, std::memory_order_release);
        signalFrameReady();
    }
}
```

## 8. UI Resample Logic Pseudocode
```cpp
void ResampleScheduler::tick()
{
    FrameBuffer * ready = pickNewestReady();
    if (!ready) return;
    if (!tryTransition(ready, FrameState::Ready, FrameState::Resampling)) return;

    resampleToDisplay(ready->image); // GL thread

    if (m_front)
        m_front->state.store(FrameState::Idle, std::memory_order_release);

    ready->state.store(FrameState::Front, std::memory_order_release);
    m_front = ready;
}
```

## 9. Triple Buffer Adaptation
- Increase buffer pool to 3 entries.
- No change to worker logic (still acquire Idle).
- UI may leave Front intact while Resampling a Ready buffer; worker continues writing to remaining Idle buffer.
- Metric: worker idle % before vs after enabling triple buffering.

## 10. Progressive Rendering (Future)
Chunk loop inside `renderFrameComputeOnly`:
```cpp
for (auto range : lineRanges(stepLines))
{
    if (epochMismatch()) { cancelled=true; break; }
    launchKernel(range);
    finishOrEvent();
    // Optional partial publish: update fb->publishedHeight (atomic)
}
```
UI optional partial resample of new region (requires extra state: lastResampledHeight).

## 11. Synchronization Primitives
| Purpose | Primitive | Notes |
|---------|-----------|-------|
| Job arrival | mutex + condvar | Last-wins avoids large queue |
| Idle buffer wait | short spin + condvar fallback | Minimizes latency |
| Frame ready notify | atomic state + condvar or eventfd | Condvar reduces polling |
| Shutdown signal | atomic<bool> | Checked in loops |

Memory ordering: state transitions to Ready / Front use `release`; acquisitions by UI / worker use `acquire`.

## 12. Cancellation & Epoch Handling
- `std::atomic<uint64_t> m_epoch` increments on invalidation.
- Worker reads epoch at job start; inside progressive loop re-check each slice batch.
- If mismatch: mark buffer Idle, discard partial.

## 13. Metrics
| Metric | Collection Point |
|--------|------------------|
| frameComputeMs | Around full render loop |
| resampleMs | UI after buffer transition |
| frameLatencyMs | Ready timestamp → Front promotion |
| workerIdlePct | Idle wait time / total |
| cancellations | Count epoch mismatch aborts |
| bufferSwapTimeNs | CAS + transitions timing |

## 14. Testing Strategy
| Test | Focus |
|------|-------|
| BasicFrame | Single frame path Ready→Front |
| RapidCamera | Cancellation & no crash |
| ResizeStress | Frequent resolution changes |
| TripleBufferLatency | Idle% improvement |
| FailureRecovery | Simulated kernel exception fallback |
| ProgressivePartial (future) | Partial resample correctness |

## 15. Failure Handling & Fallback
- Wrap kernel calls in try/catch; on exception increment failure counter.
- If N consecutive failures, disable async: set mode to `disabled`, log warning, revert to synchronous path.

## 16. Rollout Phases
| Phase | Deliverable |
|-------|------------|
| T1 | Job queue + worker thread skeleton (no rendering) |
| T2 | Integrate renderSceneComputeOnly + double buffer state machine |
| T3 | UI resample path + metrics instrumentation |
| T4 | Epoch cancellation + cancellation metrics |
| T5 | Triple buffering (config gated) |
| T6 | Performance tuning (spin thresholds, condvar fallback) |
| T7 | Progressive publication prototype |
| T8 | Hardening & documentation |

## 17. Risks & Mitigations
| Risk | Impact | Mitigation |
|------|--------|------------|
| Deadlock (condvar misuse) | High | Single responsibility locks; lock-order review |
| Busy spin CPU burn | Medium | Spin threshold then condvar sleep |
| Race in state transitions | High | CAS with explicit expected states |
| Excess latency (missed Ready) | Medium | Wake UI via lightweight event; poll fallback per-frame |
| Memory bloat triple buffers + staging | Medium | Conditional allocation; fallback to double |
| Metrics overhead | Low | Sample with coarse timing when disabled |

## 18. Acceptance Criteria
- Async double buffer mode reduces average UI frame blocking (compute) by ≥50% vs sync baseline.
- No deadlocks in 24h stress test.
- Triple buffering reduces worker idle time ≥30% when resample >3ms median.
- Cancellation path average stale frame latency < one frame interval under rapid camera churn.
- All tests pass; fallback path recovers from injected failures.

## 19. Comparison vs Coroutine Plan
| Aspect | Threaded | Coroutine |
|--------|----------|-----------|
| Complexity of control flow | Manual loops | Structured suspension |
| Tooling familiarity | High (standard) | Medium (needs coroutine expertise) |
| Latency overhead | Minimal if carefully tuned | Potentially slightly higher due to resumptions |
| Progressive extension | Extra bookkeeping | Natural with generators |
| Debugging | Conventional (gdb) | Needs coroutine-awareness |

## 20. Next Steps
Implement Phase T1–T2 behind feature flag `asyncRendering.impl = threaded` vs `coroutines` for side-by-side evaluation.

---
*Prepared as a non-coroutine alternative implementation plan for async rendering.*
