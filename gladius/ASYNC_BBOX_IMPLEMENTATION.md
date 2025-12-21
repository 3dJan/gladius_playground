# Async Bounding Box Implementation

## Overview
Bounding box computation is now asynchronous and non-blocking, preventing UI freezes during model parameter changes. The implementation ensures that the final bbox state is always computed even during rapid parameter changes.

## Key Features

### 1. Non-Blocking Computation
- Bbox updates run on worker thread
- UI remains responsive during computation
- No freezing during parameter slider movement

### 2. Guaranteed Final Update
- **Pending Update System**: If parameters change while bbox is computing, the system tracks that another update is needed
- **Automatic Rescheduling**: When a bbox job completes, it checks if another update is pending and reschedules automatically
- **No Missed Updates**: The final parameter state is always computed, even if intermediate updates are skipped

## Implementation Details

### Job Type
- Added `BoundingBoxUpdate` to `RenderJobType` enum in `AsyncRenderTypes.h`
- Bbox jobs are scheduled through the existing async rendering infrastructure

### Key Components

#### 1. RenderWindow.h
- `m_asyncBboxJobInFlight` - Tracks if a bbox job is currently executing
- `m_asyncBboxUpdatePending` - **NEW**: Tracks if another update is needed after current job completes
- Method declarations:
  - `scheduleAsyncBboxUpdate()` - Schedule async bbox computation
  - `executeAsyncBboxUpdate()` - Worker thread executor for bbox jobs

#### 2. RenderWindow.cpp

**Job Dispatcher** (line ~68):
```cpp
m_asyncController->setJobExecutor([this](RenderJob const & job, ...) {
    if (job.type == RenderJobType::BoundingBoxUpdate)
        return executeAsyncBboxUpdate(job, cancelCheck);
    else
        return executeAsyncRenderJob(job, cancelCheck);
});
```

**Scheduling** (`scheduleAsyncBboxUpdate()`):
```cpp
if (m_asyncBboxJobInFlight.load(...)) {
    // Job already running - set pending flag instead of blocking
    m_asyncBboxUpdatePending.store(true, ...);
    return;
}
// Clear pending flag and start new job
m_asyncBboxUpdatePending.store(false, ...);
m_asyncBboxJobInFlight.store(true, ...);
// Enqueue job...
```

**Execution** (`executeAsyncBboxUpdate()`):
- Runs on worker thread (non-blocking)
- Calls `m_core->updateBBox()` which is already OpenCL-based and thread-safe
- Clears `m_asyncBboxJobInFlight` flag when complete
- Returns result with `jobType` field set for proper handling

**Result Processing** (`processAsyncResults()`):
```cpp
if (result.jobType == RenderJobType::BoundingBoxUpdate) {
    // Check if another update is pending
    if (m_asyncBboxUpdatePending.load(...)) {
        scheduleAsyncBboxUpdate();  // Reschedule for final state
    }
    continue;
}
```

**Automatic Triggering** (`invalidateViewDuetoModelUpdate()`):
- Called when model parameters change
- Resets bounding box via `m_core->resetBoundingBox()`
- Schedules async bbox update if auto-update is enabled
- If job already in flight, sets pending flag

### Handling Rapid Parameter Changes

**Scenario**: User moves slider rapidly, triggering 10 parameter changes in quick succession

**Old Problem**:
1. First bbox update starts
2. Second parameter change cancels it via epoch bump
3. Third parameter change cancels second update
4. ...result: bbox never converges to final state

**New Solution**:
1. First bbox update starts (`m_asyncBboxJobInFlight = true`)
2. Second parameter change: job in flight → set `m_asyncBboxUpdatePending = true`
3. Third parameter change: job still in flight → `m_asyncBboxUpdatePending` already true
4. ...more changes: pending flag remains true
5. First job completes → checks pending flag → reschedules new job
6. New job runs with current (final) parameter state
7. Job completes → pending flag is false → done!

**Result**: Only 2 bbox computations (initial + final), not 10. Final state always computed.

### Thread Safety

The implementation is thread-safe because:
1. `updateBBox()` uses `m_computeMutex` internally
2. OpenCL operations in bbox computation are queue-based
3. Atomic flags prevent race conditions (`m_asyncBboxJobInFlight`, `m_asyncBboxUpdatePending`)
4. Same worker thread infrastructure as async rendering
5. Result processing happens on UI thread

### User-Initiated Actions

For explicit user actions (e.g., "Center View" menu item), we keep synchronous bbox updates since users expect immediate feedback.

### Result Metadata

Added `jobType` field to `FrameResultMeta`:
- Allows `processAsyncResults()` to distinguish bbox results from render results
- Enables proper handling of pending updates
- No impact on existing render job processing

## Benefits

1. **No UI Freezes**: Moving parameter sliders is smooth even during expensive bbox computation
2. **Guaranteed Convergence**: Final parameter state is always computed, never missed
3. **Efficient**: Skips intermediate states during rapid changes, only computes final state
4. **Background Computation**: Bbox updates happen on worker thread
5. **Reuses Infrastructure**: Same async system as progressive rendering
6. **Automatic Updates**: Bbox recomputes automatically when model changes
7. **Cancellable**: Jobs respect epoch system and can be cancelled if needed

## Testing

To verify async bbox updates:
1. Open a model with parameters
2. Move parameter sliders rapidly
3. Observe smooth UI (no freezing)
4. Console shows:
   - `[scheduleAsyncBboxUpdate] Job already in flight, setting pending flag` (during rapid changes)
   - `[executeAsyncBboxUpdate] Completed in Xms`
   - `[UI THREAD] Pending bbox update detected, rescheduling` (final update)
5. Verify bbox is correct for final parameter value

## Performance

Typical bbox computation time varies by model complexity:
- Simple models: 1-10ms
- Complex models: 50-200ms
- Very complex models: 200-500ms

With async implementation:
- These computations no longer block the UI thread
- Intermediate updates are skipped during rapid parameter changes
- Final state is always computed after changes stop
