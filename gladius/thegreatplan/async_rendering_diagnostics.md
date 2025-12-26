# Async Rendering Diagnostics Guide

Date: 2025-10-04
Issue: Black screen - AsyncRenderWorker thread shows in Tracy but has no activity

## Added Tracy Instrumentation

### 1. RenderAsync Flow (RenderWindow.cpp)

#### Entry Point Diagnostics
```cpp
renderAsync()
├─ "AsyncControllerNotRunning" - Falls back to sync if controller not initialized
├─ "NothingToDo" - No dirty flag, no rendering in progress, no job in flight
├─ "Dirty" / "NotDirty" - Shows dirty flag state
```

#### Scheduling Diagnostics
```cpp
renderAsync() → scheduleAsyncRenderJob()
├─ "WaitingForLowResTimeout" - Blocked by 1-second low-res preview timeout
├─ "SchedulingAsyncJob" - About to schedule job
├─ "ScheduleFailed" - scheduleAsyncRenderJob returned false
├─ "ScheduleSuccess" - Job successfully scheduled
└─ "JobAlreadyInFlight" - Another job is already running
```

### 2. Job Scheduling (scheduleAsyncRenderJob)

#### Validation Checks
```cpp
scheduleAsyncRenderJob()
├─ "ControllerNotRunning" - AsyncController not active
├─ "NoResultImage" - m_core->getResultImage() is null
├─ "InvalidHeight" - Height is 0 or currentLine >= height
├─ ZoneValue(job.startLine) - Starting line for chunk
├─ ZoneValue(job.stepSize) - Number of lines per chunk
├─ ZoneValue(job.epoch) - Epoch number (for cancellation)
└─ "JobEnqueued" - Successfully enqueued to worker
```

### 3. Worker Execution (executeAsyncRenderJob)

#### Job Processing
```cpp
executeAsyncRenderJob()
├─ ZoneValue(job.startLine) - Which chunk is being rendered
├─ ZoneValue(job.stepSize) - How many lines
├─ ZoneValue(job.epoch) - Current epoch
├─ "CancelledEarly" - Cancellation check failed before rendering
└─ "HQDisabled" - High-quality rendering is disabled (m_enableHQRendering=false)
```

## Diagnostic Flow Chart

```
renderWindow()
    ↓
render()
    ↓
renderAsync() ───→ "AsyncControllerNotRunning"? → renderSync()
    ↓
processAsyncResults()  (check for completed jobs)
    ↓
!m_dirty && !state.isRendering && !jobInFlight? ───→ "NothingToDo" → return
    ↓
state.isMoving? ───→ renderLowResPreview() → return
    ↓
timeSinceLastLowResRender < 1s? ───→ "WaitingForLowResTimeout" → return
    ↓
!m_asyncJobInFlight?
    ├─ Yes: "SchedulingAsyncJob" → scheduleAsyncRenderJob()
    │         ↓
    │     ControllerNotRunning? → "ControllerNotRunning" → return false
    │         ↓
    │     NoResultImage? → "NoResultImage" → return false
    │         ↓
    │     InvalidHeight? → "InvalidHeight" → return false
    │         ↓
    │     enqueueJob() → "JobEnqueued" → return true
    │                      ↓
    │                  "ScheduleSuccess"
    │
    └─ No: "JobAlreadyInFlight" → return

Worker Thread (AsyncRenderWorker):
    ↓
workerLoop() → co_await jobQueue.pop()
    ↓
executeAsyncRenderJob()
    ├─ cancelCheck()? → "CancelledEarly" → return cancelled
    ├─ !enableHighQuality? → "HQDisabled" → return cancelled
    ↓
acquireWriteBuffer()
    ↓
renderScene(startLine, endLine)
    ↓
copy to frame buffer
    ↓
publishFrame() (if completedFrame=true)
```

## Common Issues to Look For

### Issue 1: Worker Thread Not Executing
**Symptoms**: AsyncRenderWorker thread visible but no zones appearing  
**Check for**:
- "JobEnqueued" appears in UI thread
- No corresponding "AsyncRenderJob" in worker thread
- Possible causes:
  * Worker thread not started (`isRunning()=false`)
  * Job queue stalled
  * Exception in worker loop

**Tracy Check**: Look for "WorkerLoop_Iteration" zones in AsyncRenderWorker thread

### Issue 2: Jobs Not Being Scheduled
**Symptoms**: Black screen, no "JobEnqueued" zones  
**Check for**:
- "NothingToDo" - m_dirty flag not set
- "WaitingForLowResTimeout" - Blocked by 1-second timeout after low-res preview
- "ControllerNotRunning" - AsyncController not initialized
- "NoResultImage" - m_resultImage is null
- "InvalidHeight" - Resolution is 0 or currentLine >= height

**Tracy Check**: Look at "renderAsync" zones for diagnostic text

### Issue 3: Jobs Cancelled Immediately
**Symptoms**: "AsyncRenderJob" appears but returns immediately  
**Check for**:
- "CancelledEarly" - Epoch mismatch or shutdown
- "HQDisabled" - m_enableHQRendering is false

**Tracy Check**: Look at ZoneValue for job.epoch and compare with latestEpoch

### Issue 4: Low-Res Timeout Blocking
**Symptoms**: "WaitingForLowResTimeout" appears repeatedly  
**Root Cause**: After renderLowResPreview(), must wait 1 second before HQ rendering  
**Solution**: 
- Move camera/object to trigger invalidateView()
- Or wait 1 second after startup

### Issue 5: Dirty Flag Not Set
**Symptoms**: "NotDirty" text in renderAsync, "NothingToDo"  
**Check for**:
- invalidateView() called on startup?
- Model loaded successfully?
- Camera initialized?

**Tracy Check**: Look for "Dirty" vs "NotDirty" text

### Issue 6: Progressive Buffer Not Released
**Symptoms**: "No available buffer" in acquireWriteBuffer  
**Root Cause**: m_asyncProgressiveBuffer still held in Writing state  
**Check for**:
- Epoch changed but buffer not released in notifyAsyncEpochIncrement()
- Exception during rendering leaving buffer in Writing state

## Tracy Timeline Analysis

### Healthy Async Rendering Timeline
```
Frame N:
UI Thread:
  ├─ renderAsync [Dirty]
  ├─ scheduleAsyncRenderJob [JobEnqueued]
  └─ ... (50-100ms later)
     └─ processAsyncResults
         └─ PromoteReadyToFront

AsyncRenderWorker:
  ├─ WorkerLoop_Iteration
  ├─ AsyncRenderJob [startLine=0, stepSize=512, epoch=1]
  │   ├─ acquireWriteBuffer
  │   ├─ RenderSceneToStagingBuffer
  │   ├─ CopyToFrameBuffer
  │   └─ publishFrame (if completedFrame=true)
  └─ (result published)
```

### Problematic Timeline (No Worker Activity)
```
Frame N:
UI Thread:
  ├─ renderAsync [NotDirty]
  └─ (returns early with "NothingToDo")

AsyncRenderWorker:
  └─ (idle, no zones)
```

### Problematic Timeline (Timeout Blocking)
```
Frame N:
UI Thread:
  ├─ renderAsync [Dirty]
  └─ (returns early with "WaitingForLowResTimeout")

AsyncRenderWorker:
  └─ (idle, waiting for timeout)
```

## Quick Diagnostic Checklist

Run the application and check Tracy for:

1. ☐ Is "renderAsync" being called? (should appear every frame)
2. ☐ What text appears in renderAsync zones?
   - "Dirty" vs "NotDirty"
   - "NothingToDo"
   - "WaitingForLowResTimeout"
3. ☐ Is "scheduleAsyncRenderJob" being called?
4. ☐ Does "JobEnqueued" appear?
5. ☐ Is "AsyncRenderJob" appearing in worker thread?
6. ☐ What are the ZoneValue numbers?
   - startLine (should be 0 for first chunk)
   - stepSize (should be > 0, e.g. 512)
   - epoch (should increment on scene changes)
7. ☐ Is "WorkerLoop_Iteration" showing activity?
8. ☐ Does "PromoteReadyToFront" appear after job completes?

## Next Steps Based on Findings

### If worker thread has no activity:
1. Check if controller is started: Look for "ControllerNotRunning"
2. Check if jobs are being enqueued: Look for "JobEnqueued"
3. Check worker loop: Add breakpoint in workerLoop() or check for exceptions

### If jobs are cancelled immediately:
1. Check "CancelledEarly" or "HQDisabled" text
2. Verify m_enableHQRendering is true
3. Check epoch numbers match between UI and worker

### If no jobs scheduled:
1. Check dirty flag: Look for "Dirty" vs "NotDirty"
2. Check timeout: Look for "WaitingForLowResTimeout"
3. Trigger invalidation: invalidateView() or move camera

### If black screen persists:
1. Check if buffers are being promoted: "PromoteReadyToFront"
2. Check frontBuffer texture ID is non-zero
3. Verify GL texture binding in renderWindow()
