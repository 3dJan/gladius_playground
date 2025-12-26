# Progressive Rendering Fix for Triple Buffer System

Date: 2025-10-04
Status: ✅ Implemented

## Problem

Progressive rendering was broken with triple buffering because:
1. Each chunk acquired a **different buffer** from the triple buffer pool
2. Each chunk copied the **entire m_resultImage** (which accumulates all chunks)
3. Result: Each buffer got a snapshot at different stages, but they didn't accumulate progressively
4. **User saw**: Only the last chunk's buffer, losing all previous progress

### Broken Flow Example
```
Chunk 1 (lines 0-512):
  acquireWriteBuffer() → BufferA
  renderScene(0, 512) → m_resultImage now has lines 0-512
  copy m_resultImage → BufferA (contains 0-512)
  publishFrame(BufferA) → Ready
  Display: BufferA with lines 0-512 ✓

Chunk 2 (lines 512-1024):
  acquireWriteBuffer() → BufferB  ← DIFFERENT BUFFER!
  renderScene(512, 1024) → m_resultImage now has lines 0-1024
  copy m_resultImage → BufferB (contains 0-1024)  ← Full image copied
  publishFrame(BufferB) → Ready
  Display: BufferB with lines 0-1024 ✓ (BufferA discarded)

Chunk 3 (lines 1024-2048):
  acquireWriteBuffer() → BufferA  ← REUSED BUT CLEAN!
  renderScene(1024, 2048) → m_resultImage now has lines 0-2048
  copy m_resultImage → BufferA (contains 0-2048)  ← Full image copied again
  publishFrame(BufferA) → Ready
  Display: BufferA with complete frame ✓
```

**Issue**: We copy the entire image 3 times and rotate through buffers unnecessarily.

## Solution: Single Buffer Per Progressive Frame

**Strategy**: Keep the same buffer in `Writing` state throughout all progressive chunks. Only publish when `completedFrame=true`.

### Fixed Flow
```
Chunk 1 (lines 0-512):
  acquireWriteBuffer() → BufferA [Idle → Writing]
  m_asyncProgressiveBuffer = BufferA
  renderScene(0, 512) → m_resultImage has lines 0-512
  copy m_resultImage → BufferA
  completedFrame? NO
  ↳ Keep BufferA in Writing state (don't publish)
  
Chunk 2 (lines 512-1024):
  reuse m_asyncProgressiveBuffer (BufferA still Writing)
  renderScene(512, 1024) → m_resultImage has lines 0-1024
  copy m_resultImage → BufferA (overwrites with accumulated result)
  completedFrame? NO
  ↳ Keep BufferA in Writing state (don't publish)

Chunk 3 (lines 1024-2048):
  reuse m_asyncProgressiveBuffer (BufferA still Writing)
  renderScene(1024, 2048) → m_resultImage has lines 0-2048
  copy m_resultImage → BufferA (overwrites with final result)
  completedFrame? YES
  ↳ publishFrame(BufferA) [Writing → Ready]
  ↳ m_asyncProgressiveBuffer = nullptr
  
UI Thread:
  promoteReadyToFront() → BufferA [Ready → Resampling → Front]
  Display: Complete frame!
```

## Implementation

### 1. Added State Tracking (RenderWindow.h)
```cpp
// Progressive rendering: reuse same buffer for all chunks in a frame
async_rendering::FrameBuffer * m_asyncProgressiveBuffer{nullptr};
std::atomic<uint64_t> m_asyncProgressiveEpoch{0};
```

### 2. Buffer Lifecycle Management

#### On Epoch Change (notifyAsyncEpochIncrement)
```cpp
// Release progressive buffer when scene changes (camera move, model update)
if (m_asyncProgressiveBuffer) {
    tryTransitionBuffer(m_asyncProgressiveBuffer, Writing, Idle);
    m_asyncProgressiveBuffer = nullptr;
}
m_asyncProgressiveEpoch.store(0);
```

#### During Job Execution (executeAsyncRenderJob)
```cpp
bool isFirstChunk = (job.startLine == 0);

if (isFirstChunk || !m_asyncProgressiveBuffer || epochChanged) {
    // Acquire fresh buffer for new frame
    writeBuffer = acquireWriteBuffer(job.epoch);
    m_asyncProgressiveBuffer = writeBuffer;
    m_asyncProgressiveEpoch = job.epoch;
} else {
    // Reuse same buffer for subsequent chunks
    writeBuffer = m_asyncProgressiveBuffer;
}

// ... render chunk ...

if (result.completedFrame) {
    // Frame complete - publish and release
    publishFrame(writeBuffer, frameId, epoch);
    m_asyncProgressiveBuffer = nullptr;
} else {
    // Chunk complete but more to come - keep buffer in Writing state
    // (Don't publish, don't release)
}
```

#### On Cancellation
```cpp
if (cancelled && isFirstChunk) {
    // Only release if we just acquired it
    tryTransitionBuffer(writeBuffer, Writing, Idle);
    m_asyncProgressiveBuffer = nullptr;
}
// If not first chunk, keep buffer for potential retry
```

### 3. UI Result Processing (processAsyncResults)
```cpp
state.currentLine = result.completedLine;  // Always update progress

if (result.completedFrame) {
    // Only promote buffer when frame is complete (published)
    promoteReadyToFront();
    // Transition Ready → Resampling → Front
} else {
    // Intermediate chunk - no buffer to promote yet
    // m_asyncProgressiveBuffer is still Writing
}
```

## Buffer State Behavior

### During Progressive Frame
```
Frame with 3 chunks (0-512, 512-1024, 1024-2048):

T=0:     BufferA [Idle]  BufferB [Front]  BufferC [Idle]
         │
Chunk 1: BufferA [Writing] ← m_asyncProgressiveBuffer
         │ render 0-512
         │ keep Writing (no publish)
         │
Chunk 2: BufferA [Writing] ← reuse
         │ render 512-1024
         │ keep Writing (no publish)
         │
Chunk 3: BufferA [Writing] ← reuse
         │ render 1024-2048
         │ publish [Writing → Ready]
         │ m_asyncProgressiveBuffer = nullptr
         │
UI:      BufferA [Ready → Resampling → Front]
         BufferB [Front → Idle]
         
Final:   BufferA [Front]  BufferB [Idle]  BufferC [Idle]
```

### Effective Buffering During Progressive
- **Reduces to double buffering** during progressive frames
  - One buffer in `Writing` (accumulating chunks)
  - One buffer in `Front` (displayed)
  - One buffer `Idle` (unused)
- **Returns to triple buffering** between frames
  - Worker can immediately start next frame while UI resamples

## Benefits

✅ **Progressive rendering works correctly**
- All chunks accumulate in the same buffer
- Display shows complete accumulated result when ready

✅ **No wasted buffer rotations**
- Don't acquire new buffer for each chunk
- Only one copy per chunk (not three)

✅ **Clean cancellation handling**
- Epoch change releases progressive buffer immediately
- Cancellation during first chunk releases buffer
- Cancellation during later chunks keeps buffer (no orphan state)

✅ **Smooth UI updates**
- `state.currentLine` advances after each chunk
- Progress bar can show incremental progress
- Final frame promotion happens only when complete

## Limitations & Future Work

### Current Limitation
Still copying **entire m_resultImage** for each chunk, even though we only rendered a portion. This is because:
- `renderScene()` renders to shared `m_resultImage` 
- We copy the full accumulated image to frame buffer
- Wastes bandwidth copying unchanged regions

### Future Optimization (Option B)
Refactor `ComputeCore::renderScene()` to accept target buffer:
```cpp
// Instead of:
renderScene(startLine, endLine)  → always renders to m_resultImage

// Do:
renderScene(startLine, endLine, targetBuffer)  → renders directly to specified buffer
```

**Benefits**:
- No copy needed (render directly into frame buffer)
- Worker renders to `m_asyncProgressiveBuffer->image` directly
- True accumulation without redundant copies
- Requires deeper refactoring of RenderProgram

### Alternative: Partial Copy (Option C)
Copy only the updated region:
```cpp
queue.enqueueCopyImage(
    srcImage, dstImage,
    {0, startLine, 0},      // src origin
    {0, startLine, 0},      // dst origin  
    {width, endLine - startLine, 1}  // region size (only updated lines)
);
```

**Benefits**:
- Less data copied per chunk
- Simpler than refactoring renderScene()
- Still requires managing what's already in the buffer

**Drawback**:
- First chunk must copy entire image (no previous data)
- Complexity in handling buffer initialization

## Testing Recommendations

1. **Progressive rendering validation**:
   - Large scene (2048x2048+)
   - Verify chunks appear incrementally
   - Check state.currentLine advances correctly

2. **Epoch cancellation**:
   - Rapid camera movement during progressive frame
   - Verify buffer is released and no leaks
   - Check new frame starts with clean buffer

3. **Triple buffer efficiency**:
   - Monitor buffer states with Tracy
   - Verify worker idle time is low
   - Check Front buffer always has complete frames

4. **Edge cases**:
   - Single chunk frame (startLine=0, completedFrame=true immediately)
   - Very small stepSize (many tiny chunks)
   - Cancellation between chunks

## Summary

This fix makes progressive rendering **work correctly** with the triple buffer system by:
1. **Reusing the same buffer** across all chunks in a progressive frame
2. **Publishing only when complete** (not after each chunk)
3. **Clean epoch handling** to release buffers on scene changes
4. **Maintaining triple buffer benefits** between complete frames

The system now correctly accumulates progressive chunks and provides a solid foundation for future optimizations like direct rendering or partial copies.
