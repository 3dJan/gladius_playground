# Async Model Update Pipeline Plan

Date: 2025-10-24  
Status: Planning Phase  
Related: `async_rendering_coroutines_plan.md`

## 1. Executive Summary

When a user modifies the model (parameter changes, geometry edits, node graph updates), the UI currently blocks on three potentially expensive operations before rendering can begin:

1. **OpenCL Program Recompilation** (100ms - 5000ms depending on model complexity)
2. **Bounding Box Computation** (10ms - 500ms)
3. **Precomputed SDF Generation** (50ms - 2000ms)

This plan outlines a strategy to move these operations to worker threads using the existing async rendering coroutine infrastructure, ensuring a responsive UI even during complex model updates.

## 2. Problem Analysis

### 2.1 Current Blocking Pipeline

```
Model Change Event
    ↓
refreshProgram()                    [Generates OpenCL source]
    ↓
recompileIfRequired()              [UI BLOCKS 100-5000ms]
    ↓  (holds m_computeMutex)
updateBBox()                       [UI BLOCKS 10-500ms]
    ↓  (requires compiled program)
precomputeSdfForWholeBuildPlatform() [UI BLOCKS 50-2000ms]
    ↓  (requires bounding box)
renderScene()                       [Now async, see async_rendering_coroutines_plan.md]
```

### 2.2 Blocking Points Identified

| Operation | Location | Duration | Mutex Held | Dependencies |
|-----------|----------|----------|------------|--------------|
| `recompileNonBlocking()` | `ProgramBase.cpp:56` | 100-5000ms | `m_computeMutex` | Model source code |
| `updateBBox()` | `ComputeCore.cpp:640` | 10-500ms | `m_computeMutex` | Compiled OpenCL program |
| `precomputeSdf()` | `SlicerProgram.cpp:140` | 50-2000ms | `m_computeMutex` + `m_queueMutex` | Compiled program + bounding box |
| `updateParameterBlocking()` | `ComputeCore.cpp:185` | 5-50ms | `m_computeMutex` | Assembly data |

### 2.3 Current State Machine

```cpp
// In ComputeCore.cpp
class ModelState {
    enum class State {
        OutOfDate,           // Model changed, needs recompile
        Compiling,          // OpenCL build in progress
        UpToDate            // Ready to render
    };
};
```

**Issue**: State machine is synchronous and doesn't distinguish between compilation, bbox, and SDF stages.

## 3. Architecture Goals

1. **Non-blocking UI**: Model updates never hold `m_computeMutex` on UI thread
2. **Progressive Availability**: Show low-res preview immediately, upgrade to HQ when ready
3. **Cancellation Safety**: New edits cancel in-flight expensive operations
4. **State Visibility**: UI shows progress (Compiling → Computing BBox → Precomputing SDF → Ready)
5. **Fallback Graceful**: Render with best available state (old SDF, no SDF, full model)
6. **Ordering Guarantees**: Operations execute in correct dependency order

## 4. Proposed Architecture

### 4.1 Extended State Machine

```cpp
namespace gladius::async_model_update {

enum class ModelUpdateStage {
    Idle,                    // No update needed
    PendingCompilation,      // Source generated, awaiting build
    Compiling,               // OpenCL build in progress
    CompilationFailed,       // Build error (display to user)
    PendingBBoxUpdate,       // Program ready, bbox needed
    ComputingBBox,           // BBox kernel running
    BBoxFailed,              // BBox computation error
    PendingSDFUpdate,        // BBox ready, SDF needed
    ComputingSDF,            // SDF precomputation running
    SDFFailed,               // SDF computation error
    Ready                    // All resources up-to-date
};

struct ModelUpdateState {
    std::atomic<ModelUpdateStage> stage{ModelUpdateStage::Idle};
    std::atomic<uint64_t> epoch{0};          // Increments on each model change
    std::atomic<bool> cancellationRequested{false};
    
    // Progress tracking
    std::atomic<float> compilationProgress{0.0f};    // 0.0 - 1.0
    std::atomic<float> bboxProgress{0.0f};
    std::atomic<float> sdfProgress{0.0f};
    
    // Error state
    std::string lastError;
    std::mutex errorMutex;
};

}
```

### 4.2 Job Types

Extend existing `RenderJobType` enum:

```cpp
enum class RenderJobType {
    HighQuality,
    LowResPreview,
    BoundingBoxUpdate,
    ParameterUpdate,         // NEW: Fast path, no recompilation
    ProgramCompilation,      // NEW: Slow path, full recompile
    SDFPrecomputation       // NEW: SDF regeneration
};
```

### 4.3 Pipeline Coroutine

```cpp
coro::task<void> modelUpdatePipeline(
    ModelUpdateJob job,
    AsyncRenderController::CancelCheck cancelCheck)
{
    // Stage 1: Compilation
    co_await compileProgram(job, cancelCheck);
    if (cancelCheck()) co_return;
    
    // Stage 2: Bounding Box
    co_await computeBoundingBox(job, cancelCheck);
    if (cancelCheck()) co_return;
    
    // Stage 3: Precomputed SDF
    co_await precomputeSDF(job, cancelCheck);
    if (cancelCheck()) co_return;
    
    // Mark as ready - rendering can proceed with HQ path
    notifyModelReady(job.epoch);
}
```

## 5. Detailed Design

### 5.1 OpenCL Compilation (Async)

**Current State**: `recompileNonBlocking()` already spawns a background thread but holds `m_computeMutex`.

**Changes**:
1. Move compilation status check out of critical section
2. Use atomic completion flag instead of mutex-protected state
3. Add progress callback for UI feedback

```cpp
// In ProgramBase.h
struct CompilationProgress {
    std::atomic<bool> inProgress{false};
    std::atomic<float> progress{0.0f};  // 0.0 = started, 1.0 = complete
    std::atomic<bool> succeeded{false};
    std::string errorMessage;  // Protected by separate mutex
};

// In ProgramBase.cpp
void ProgramBase::recompileAsync(
    std::function<void(float)> progressCallback,
    std::function<void(bool, std::string)> completionCallback)
{
    m_compilationProgress.inProgress.store(true, std::memory_order_release);
    
    // Spawn OpenCL build on background thread (already done)
    m_buildFinishedCallBack = [=, this]() {
        m_programSwapRequired = true;
        m_compilationProgress.inProgress.store(false, std::memory_order_release);
        m_compilationProgress.succeeded.store(true, std::memory_order_release);
        m_compilationProgress.progress.store(1.0f, std::memory_order_release);
        
        if (completionCallback) {
            completionCallback(true, "");
        }
    };
    
    // ... existing buildFromSourceAndLinkWithLibNonBlocking call ...
}
```

**Integration with Coroutines**:

```cpp
coro::task<bool> compileProgram(
    ModelUpdateJob const & job,
    AsyncRenderController::CancelCheck const & cancelCheck)
{
    ZoneScopedN("AsyncCompileProgram");
    
    // Launch async compilation
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    
    m_programBase->recompileAsync(
        [](float progress) { /* update UI */ },
        [promise](bool success, std::string error) {
            promise->set_value(success);
        }
    );
    
    // Poll for completion (could be optimized with event awaitable)
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (cancelCheck && cancelCheck()) {
            // Cancellation: OpenCL builds can't be aborted mid-flight,
            // but we can mark result as stale
            co_return false;
        }
        co_await coro::yield();  // Let other coroutines run
    }
    
    bool const success = future.get();
    co_return success;
}
```

### 5.2 Bounding Box Computation (Async)

**Current State**: `updateBBox()` runs synchronously on calling thread, holds `m_computeMutex`.

**Changes**:
1. Already partially async (used in `executeAsyncBboxUpdate()`)
2. Extend to use dedicated command queue for compute-only path
3. Use OpenCL events for completion tracking

```cpp
// In ComputeCore.h
bool updateBBoxAsync(
    cl::CommandQueue & workerQueue,
    std::function<void(bool, BoundingBox)> completionCallback);

// In ComputeCore.cpp
bool ComputeCore::updateBBoxAsync(
    cl::CommandQueue & workerQueue,
    std::function<void(bool, BoundingBox)> completionCallback)
{
    ProfileFunction
    
    // Lock only for resource access, not for computation
    if (!m_computeMutex.try_lock()) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_computeMutex, std::adopt_lock);
    
    // Quick return if already valid
    if (m_boundingBox && !std::isinf(m_boundingBox->min.x)) {
        if (completionCallback) {
            completionCallback(true, *m_boundingBox);
        }
        return true;
    }
    
    // Ensure program is compiled
    if (!m_programs.getSlicerState().isModelUpToDate()) {
        return false;
    }
    
    m_resources->initConvexHullVertices();
    
    // Launch bbox computation kernel (async)
    cl::Event bboxEvent;
    m_programs.getSlicerProgram()->computeBoundingBoxAsync(
        workerQueue, *m_primitives, m_resources->getConvexHullVertices(), &bboxEvent);
    
    // Release lock - bbox computation continues on GPU
    lock.unlock();
    
    // Set callback for event completion
    bboxEvent.setCallback(CL_COMPLETE, 
        [this, completionCallback](cl_event, cl_int status, void*) {
            if (status == CL_COMPLETE) {
                // Read back bbox result
                auto bbox = readBoundingBoxResult();
                m_boundingBox = bbox;
                if (completionCallback) {
                    completionCallback(true, bbox);
                }
            } else {
                if (completionCallback) {
                    completionCallback(false, {});
                }
            }
        }, nullptr);
    
    return true;
}
```

**Coroutine Integration**:

```cpp
coro::task<std::optional<BoundingBox>> computeBoundingBox(
    ModelUpdateJob const & job,
    cl::CommandQueue & workerQueue,
    AsyncRenderController::CancelCheck const & cancelCheck)
{
    ZoneScopedN("AsyncComputeBBox");
    
    std::optional<BoundingBox> result;
    auto promise = std::make_shared<std::promise<std::optional<BoundingBox>>>();
    auto future = promise->get_future();
    
    bool const launched = m_core->updateBBoxAsync(
        workerQueue,
        [promise](bool success, BoundingBox bbox) {
            if (success) {
                promise->set_value(bbox);
            } else {
                promise->set_value(std::nullopt);
            }
        }
    );
    
    if (!launched) {
        co_return std::nullopt;
    }
    
    // Await completion
    while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
        if (cancelCheck && cancelCheck()) {
            co_return std::nullopt;
        }
        co_await coro::yield();
    }
    
    co_return future.get();
}
```

### 5.3 Precomputed SDF (Async)

**Current State**: `precomputeSdf()` is synchronous, blocks until kernel completes.

**Changes**:
1. Use OpenCL events for async completion
2. Already GPU-side, just need to avoid blocking `clFinish()`
3. Support cancellation via event

```cpp
// In SlicerProgram.h
void precomputeSdfAsync(
    const Primitives & lines,
    BoundingBox boundingBox,
    cl::CommandQueue & queue,
    cl::Event * completionEvent);

// In SlicerProgram.cpp
void SlicerProgram::precomputeSdfAsync(
    const Primitives & lines,
    BoundingBox boundingBox,
    cl::CommandQueue & queue,
    cl::Event * completionEvent)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    ProfileFunction;
    swapProgramsIfNeeded();
    
    auto & target = m_resoures->getPrecompSdfBuffer();
    m_resoures->getRenderingSettings().approximation = AM_FULL_MODEL;
    
    const cl::NDRange origin = {0, 0, 0};
    const cl::NDRange range = {target.getWidth(), target.getHeight(), target.getDepth()};
    
    cl::Event sdfEvent;
    m_programFront->runAsync(
        queue,
        "preComputeSdf", 
        origin, 
        range, 
        &sdfEvent,
        target.getBuffer(), 
        boundingBox, 
        PAYLOAD_ARGUMENTS);
    
    if (completionEvent) {
        *completionEvent = sdfEvent;
    }
    
    queue.flush();  // Don't wait - let it run async
}
```

**Coroutine Integration** (already compatible with existing async render pattern):

```cpp
coro::task<bool> precomputeSDF(
    ModelUpdateJob const & job,
    cl::CommandQueue & workerQueue,
    BoundingBox const & bbox,
    AsyncRenderController::CancelCheck const & cancelCheck)
{
    ZoneScopedN("AsyncPrecomputeSDF");
    
    // Allocate SDF buffer
    m_core->getResourceContext()->allocatePreComputedSdf(
        job.sdfResolution, job.sdfResolution, job.sdfResolution);
    m_core->getResourceContext()->setPreCompSdfBBox(bbox);
    
    // Launch async SDF computation
    cl::Event sdfEvent;
    m_core->getSlicerProgram()->precomputeSdfAsync(
        *m_core->getPrimitives(), bbox, workerQueue, &sdfEvent);
    
    // Await completion
    co_await waitForEvent(sdfEvent, cancelCheck);
    
    if (cancelCheck && cancelCheck()) {
        co_return false;
    }
    
    // Mark SDF as valid
    m_core->setPrecompSdfValid(true);
    co_return true;
}
```

## 6. Integration with Existing Async Rendering

### 6.1 Extended Job Structure

```cpp
struct ModelUpdateJob {
    uint64_t epoch;
    RenderJobType type;
    
    // Parameter update stage (fast path)
    nodes::SharedAssembly assembly;  // For parameter extraction
    bool parameterOnly{false};       // True if no recompilation needed
    
    // Compilation stage
    std::string modelSource;
    bool enableVdb;
    
    // BBox stage
    bool needsBBoxUpdate;
    
    // SDF stage
    bool needsSdfUpdate;
    size_t sdfResolution{256};
    
    // Coordination
    std::shared_ptr<ModelUpdateState> updateState;
};
```

### 6.2 Modified RenderWindow Flow

```cpp
void RenderWindow::invalidateViewDuetoModelUpdate()
{
    // Don't mark bounding box/SDF as dirty immediately
    // Instead, schedule async update job
    
    auto currentEpoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_asyncCurrentEpoch.store(currentEpoch, std::memory_order_release);
    
    ModelUpdateJob job{};
    job.epoch = currentEpoch;
    job.type = RenderJobType::ProgramCompilation;  // First stage
    job.modelSource = m_core->generateModelSource();  // Non-blocking
    job.enableVdb = m_core->isVdbRequired();
    job.needsBBoxUpdate = true;
    job.needsSdfUpdate = true;
    
    m_asyncController->enqueueJob(job);
    
    // Immediately render with low-res preview using old SDF/program
    m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
    m_dirty = true;
}
```

### 6.3 Worker Job Executor

```cpp
coro::task<FrameResultMeta> executeModelUpdateJob(
    ModelUpdateJob const & job,
    AsyncRenderController::CancelCheck const & cancelCheck)
{
    ZoneScopedN("ModelUpdateJob");
    
    FrameResultMeta result{};
    result.epoch = job.epoch;
    result.jobType = job.type;
    
    auto * workerQueue = m_asyncController->workerQueue();
    if (!workerQueue) {
        result.cancelled = true;
        co_return result;
    }
    
    try {
        // Stage 1: Compile program
        if (job.type == RenderJobType::ProgramCompilation) {
            bool success = co_await compileProgram(job, cancelCheck);
            if (!success || (cancelCheck && cancelCheck())) {
                result.cancelled = true;
                co_return result;
            }
            
            // Chain to next stage
            ModelUpdateJob bboxJob = job;
            bboxJob.type = RenderJobType::BoundingBoxUpdate;
            m_asyncController->enqueueJob(bboxJob);
        }
        
        // Stage 2: Compute bounding box
        else if (job.type == RenderJobType::BoundingBoxUpdate) {
            auto bbox = co_await computeBoundingBox(job, *workerQueue, cancelCheck);
            if (!bbox.has_value() || (cancelCheck && cancelCheck())) {
                result.cancelled = true;
                co_return result;
            }
            
            // Chain to SDF stage
            if (job.needsSdfUpdate) {
                ModelUpdateJob sdfJob = job;
                sdfJob.type = RenderJobType::SDFPrecomputation;
                m_asyncController->enqueueJob(sdfJob);
            } else {
                // Model update complete - ready to render HQ
                notifyModelUpdateComplete(job.epoch);
            }
        }
        
        // Stage 3: Precompute SDF
        else if (job.type == RenderJobType::SDFPrecomputation) {
            auto bbox = m_core->getBoundingBox();
            if (!bbox.has_value()) {
                result.cancelled = true;
                co_return result;
            }
            
            bool success = co_await precomputeSDF(job, *workerQueue, *bbox, cancelCheck);
            if (!success || (cancelCheck && cancelCheck())) {
                result.cancelled = true;
                co_return result;
            }
            
            // Model update complete
            notifyModelUpdateComplete(job.epoch);
        }
    }
    catch (std::exception const & e) {
        logError(fmt::format("Model update failed: {}", e.what()));
        result.cancelled = true;
    }
    
    co_return result;
}
```

## 7. Parameter-Only Updates (No Recompilation)

### 7.1 Fast Path Detection

Parameter changes don't require OpenCL recompilation if:
- ✅ Number of parameters unchanged
- ✅ Parameter types unchanged (float, float3, matrix4x4)
- ✅ Parameter lookup indices unchanged
- ✅ Only parameter **values** changed

**Current Implementation**:
```cpp
// In Document.cpp:327
void Document::updateParameter() {
    m_parameterDirty = m_core->tryToupdateParameter(*m_assembly);
    
    if (m_parameterDirty) {
        m_parameterDirty = !m_core->precomputeSdfForWholeBuildPlatform();  // BLOCKS UI
    }
}

// In ComputeCore.cpp:183
bool ComputeCore::updateParameterBlocking(nodes::Assembly & assembly) {
    std::lock_guard<std::recursive_mutex> lock(m_computeMutex);  // BLOCKS
    
    if (isAutoUpdateBoundingBoxEnabled()) {
        resetBoundingBox();  // Forces bbox recomputation
    }
    
    auto & paramBuf = getResourceContext()->getParameterBuffer();
    auto & parameter = paramBuf.getData();
    parameter.clear();
    
    int currentIndex = 0;
    // Iterate parameters, assign lookup indices, flatten to buffer
    for (auto & model : assembly.getFunctions()) {
        for (auto [id, param] : model.second->getParameterRegistry()) {
            param->setLookUpIndex(currentIndex);
            // Flatten float/int/float3/matrix4x4 to buffer
            currentIndex += paramSize;
        }
    }
    
    paramBuf.write();  // Upload to GPU (fast, async possible)
    invalidatePreCompSdf();  // Marks SDF as stale
    return true;
}
```

### 7.2 Parameter Signature Validation

**Problem**: If parameter count/types change but compilation is skipped, OpenCL kernel will access invalid indices → crash/corruption.

**Solution**: Track expected parameter signature and validate before fast path.

```cpp
// In ResourceContext.h
struct ParameterSignature {
    size_t totalFloatCount{0};          // Total flattened parameter count
    std::vector<size_t> parameterSizes; // Size of each parameter (1, 3, 16, etc.)
    uint64_t signatureHash{0};          // Fast comparison
    
    bool matches(ParameterSignature const & other) const {
        return totalFloatCount == other.totalFloatCount &&
               signatureHash == other.signatureHash;
    }
    
    static ParameterSignature compute(nodes::Assembly const & assembly);
};

// In ProgramManager.h
class ProgramManager {
    ParameterSignature m_compiledParameterSignature;  // Set after successful compilation
    
public:
    bool isParameterSignatureCompatible(nodes::Assembly const & assembly) const {
        auto currentSig = ParameterSignature::compute(assembly);
        return currentSig.matches(m_compiledParameterSignature);
    }
};
```

### 7.3 Fast Parameter Update Path

```cpp
// In ComputeCore.h
enum class ParameterUpdateResult {
    Success,                  // Parameters updated, no compilation needed
    RequiresRecompilation,   // Signature changed, must recompile
    InProgress               // Another update in flight, skip
};

ParameterUpdateResult tryUpdateParameterAsync(
    nodes::Assembly & assembly,
    std::function<void(ParameterUpdateResult)> callback);
```

**Async Parameter Update**:
```cpp
coro::task<FrameResultMeta> executeParameterUpdateJob(
    ParameterUpdateJob const & job,
    AsyncRenderController::CancelCheck const & cancelCheck)
{
    ZoneScopedN("AsyncParameterUpdate");
    
    FrameResultMeta result{};
    result.epoch = job.epoch;
    result.jobType = RenderJobType::ParameterUpdate;
    
    // 1. Validate signature compatibility
    auto currentSig = ParameterSignature::compute(*job.assembly);
    if (!currentSig.matches(m_compiledParameterSignature)) {
        // Must trigger full recompilation
        result.requiresRecompilation = true;
        co_return result;
    }
    
    // 2. Fast path: Update parameter buffer (GPU upload, ~1ms)
    {
        std::lock_guard<std::mutex> lock(m_parameterMutex);  // Lightweight lock
        
        auto & paramBuf = m_resources->getParameterBuffer();
        auto & parameter = paramBuf.getData();
        parameter.clear();
        
        int currentIndex = 0;
        for (auto & model : job.assembly->getFunctions()) {
            for (auto [id, param] : model.second->getParameterRegistry()) {
                // Flatten parameter to buffer
                // (same logic as updateParameterBlocking)
            }
        }
        
        paramBuf.write();  // GPU upload (non-blocking)
    }
    
    // 3. Invalidate dependent resources
    m_precompSdfIsValid = false;
    if (isAutoUpdateBoundingBoxEnabled()) {
        m_boundingBox.reset();
    }
    
    result.parameterUpdated = true;
    
    // 4. Chain to bbox/SDF updates if needed
    if (job.updateBBox) {
        ParameterUpdateJob bboxJob = job;
        bboxJob.type = RenderJobType::BoundingBoxUpdate;
        m_asyncController->enqueueJob(bboxJob);
    }
    
    co_return result;
}
```

### 7.4 Signature Change Detection

```cpp
void Document::updateParameter() {
    ProfileFunction;
    
    if (!m_assembly || !m_core) return;
    
    updatePayload();
    
    // Check if parameter signature changed
    auto currentSig = ParameterSignature::compute(*m_assembly);
    auto compiledSig = m_core->getCompiledParameterSignature();
    
    if (!currentSig.matches(compiledSig)) {
        // Signature changed - need full recompilation
        logInfo("Parameter signature changed, triggering recompilation");
        refreshModelAsync();  // Calls full async pipeline
        return;
    }
    
    // Fast path: parameters compatible, update asynchronously
    ParameterUpdateJob job{};
    job.epoch = m_asyncEpochCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
    job.type = RenderJobType::ParameterUpdate;
    job.assembly = m_assembly;
    job.updateBBox = isAutoUpdateBoundingBoxEnabled();
    job.updateSdf = true;
    
    m_asyncController->enqueueJob(job);
    
    // Continue rendering with old parameters while update happens
    m_forceLowResRenderOnNextFrame.store(true, std::memory_order_release);
}
```

### 7.5 Parameter Buffer Synchronization

**Challenge**: Rendering may be reading parameter buffer while update writes to it.

**Solution**: Double-buffering parameter updates (similar to frame buffers).

```cpp
class ParameterBuffer {
    Buffer<cl_float> m_frontBuffer;  // Currently used for rendering
    Buffer<cl_float> m_backBuffer;   // Being updated
    std::atomic<bool> m_swapPending{false};
    
public:
    // Worker thread updates back buffer
    void updateAsync(std::vector<cl_float> const & newParams) {
        m_backBuffer.getData() = newParams;
        m_backBuffer.write();  // Upload to GPU
        m_swapPending.store(true, std::memory_order_release);
    }
    
    // UI thread swaps buffers before rendering
    void swapIfPending() {
        if (m_swapPending.exchange(false, std::memory_order_acq_rel)) {
            std::swap(m_frontBuffer, m_backBuffer);
        }
    }
    
    cl::Buffer & getBuffer() {
        return m_frontBuffer.getBuffer();
    }
};
```

**Integration**:
```cpp
// In RenderWindow::renderAsync()
void RenderWindow::renderAsync(RenderWindowState & state) {
    // Swap parameter buffers if update completed
    m_core->getResourceContext()->getParameterBuffer().swapIfPending();
    
    // Continue with rendering...
}
```

### 7.6 Signature Computation Example

```cpp
ParameterSignature ParameterSignature::compute(nodes::Assembly const & assembly) {
    ParameterSignature sig{};
    std::hash<std::string> hasher;
    std::string signatureString;
    
    for (auto const & model : assembly.getFunctions()) {
        if (!model.second) continue;
        
        for (auto [id, param] : model.second->getParameterRegistry()) {
            auto const varParam = dynamic_cast<nodes::VariantParameter*>(param);
            if (!varParam) continue;
            
            auto const & variant = varParam->Value();
            
            if (std::holds_alternative<float>(variant)) {
                sig.parameterSizes.push_back(1);
                sig.totalFloatCount += 1;
                signatureString += "f1;";
            }
            else if (std::holds_alternative<int>(variant)) {
                sig.parameterSizes.push_back(1);
                sig.totalFloatCount += 1;
                signatureString += "i1;";
            }
            else if (std::holds_alternative<nodes::float3>(variant)) {
                sig.parameterSizes.push_back(3);
                sig.totalFloatCount += 3;
                signatureString += "f3;";
            }
            else if (std::holds_alternative<nodes::Matrix4x4>(variant)) {
                sig.parameterSizes.push_back(16);
                sig.totalFloatCount += 16;
                signatureString += "m44;";
            }
        }
    }
    
    sig.signatureHash = hasher(signatureString);
    return sig;
}
```

### 7.7 Update Decision Flow

```
Parameter Change Event
    ↓
Compute Parameter Signature
    ↓
Compare with Compiled Signature
    ↓
    ├─ MATCH → Fast Path (Parameter Update Only)
    │     ↓
    │   Update Parameter Buffer (async, ~1ms)
    │     ↓
    │   Schedule BBox Update (if enabled)
    │     ↓
    │   Schedule SDF Update
    │     ↓
    │   Render with updated parameters
    │
    └─ MISMATCH → Slow Path (Full Recompilation)
          ↓
        Trigger async model update pipeline
          (see Section 4)
```

### 7.8 Error Cases

| Case | Detection | Recovery |
|------|-----------|----------|
| Signature computed incorrectly | Parameter access out of bounds in kernel | Validation pass before GPU upload, bounds checking |
| Concurrent parameter + model change | Epoch mismatch | Cancel parameter update, use new epoch |
| Parameter buffer upload fails | OpenCL error on write() | Retry once, fallback to blocking update |
| Lookup index corruption | Index >= buffer size | Recompute indices, validate before upload |

### 7.9 Performance Characteristics

| Operation | Synchronous | Async (This Plan) | Speedup |
|-----------|-------------|-------------------|---------|
| Parameter buffer update | 1-2ms (blocks UI) | 1-2ms (background) | ∞ (non-blocking) |
| BBox recompute (param change) | 10-50ms (blocks UI) | 10-50ms (background) | ∞ (non-blocking) |
| SDF recompute (param change) | 50-500ms (blocks UI) | 50-500ms (background) | ∞ (non-blocking) |
| **Total time-to-interactive** | **61-552ms** | **<1ms** | **60-550x** |

**Note**: Async doesn't make operations faster, but makes UI responsive during updates.

## 8. Fallback Rendering Strategy

While model update is in progress, render with best available resources:

| Update Stage | Rendering Strategy | Approximation Mode |
|--------------|-------------------|-------------------|
| ParameterUpdate | Use current program + old params | AM_HYBRID (may be inaccurate) |
| Compiling | Use old program + old SDF | AM_ONLY_PRECOMPSDF (low-res) |
| ComputingBBox | Use new program + old SDF | AM_HYBRID (medium quality) |
| ComputingSDF | Use new program + no SDF | AM_FULL_MODEL (slower but correct) |
| Ready | Use new program + new SDF | AM_HYBRID (full quality) |

```cpp
ApproximationMode determineBestApproximation() {
    auto stage = m_modelUpdateState->stage.load(std::memory_order_acquire);
    
    switch (stage) {
        case ModelUpdateStage::Compiling:
        case ModelUpdateStage::PendingCompilation:
            return AM_ONLY_PRECOMPSDF;  // Fast preview with old SDF
            
        case ModelUpdateStage::ComputingBBox:
        case ModelUpdateStage::PendingBBoxUpdate:
            return m_precompSdfIsValid ? AM_HYBRID : AM_FULL_MODEL;
            
        case ModelUpdateStage::ComputingSDF:
        case ModelUpdateStage::PendingSDFUpdate:
            return AM_FULL_MODEL;  // Accurate but slower
            
        case ModelUpdateStage::Ready:
            return AM_HYBRID;  // Best quality
            
        default:
            return AM_FULL_MODEL;
    }
}
```

## 8. UI Progress Indication

Add visual feedback for long-running operations:

```cpp
void RenderWindow::renderProgressOverlay() {
    auto stage = m_modelUpdateState->stage.load(std::memory_order_acquire);
    
    if (stage == ModelUpdateStage::Idle || stage == ModelUpdateStage::Ready) {
        return;  // No overlay needed
    }
    
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("Model Update Progress", nullptr, 
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    
    switch (stage) {
        case ModelUpdateStage::Compiling:
        case ModelUpdateStage::PendingCompilation: {
            ImGui::Text("Compiling OpenCL program...");
            float progress = m_modelUpdateState->compilationProgress.load();
            ImGui::ProgressBar(progress);
            break;
        }
        
        case ModelUpdateStage::ComputingBBox:
        case ModelUpdateStage::PendingBBoxUpdate: {
            ImGui::Text("Computing bounding box...");
            ui::loadingIndicatorCircle("bbox", 20, ...);
            break;
        }
        
        case ModelUpdateStage::ComputingSDF:
        case ModelUpdateStage::PendingSDFUpdate: {
            ImGui::Text("Precomputing distance field...");
            float progress = m_modelUpdateState->sdfProgress.load();
            ImGui::ProgressBar(progress);
            break;
        }
        
        default:
            break;
    }
    
    ImGui::End();
}
```

## 9. Error Handling & Recovery

### 9.1 Compilation Failures

```cpp
if (compilationFailed) {
    // Keep using old program
    m_modelUpdateState->stage.store(ModelUpdateStage::CompilationFailed);
    
    // Show error to user
    m_eventLogger->logError(fmt::format(
        "OpenCL compilation failed: {}", errorMessage));
    
    // Render with old program (graceful degradation)
    m_core->renderWithFallbackProgram();
}
```

### 9.2 BBox Computation Failures

```cpp
if (bboxFailed) {
    // Use previous bounding box or default
    m_modelUpdateState->stage.store(ModelUpdateStage::BBoxFailed);
    
    // Continue with SDF update using old/default bbox
    // Or skip SDF update entirely
}
```

### 9.3 SDF Precomputation Failures

```cpp
if (sdfFailed) {
    // Render without SDF acceleration (slower but functional)
    m_modelUpdateState->stage.store(ModelUpdateStage::SDFFailed);
    m_precompSdfIsValid = false;
    
    // Use AM_FULL_MODEL approximation mode
}
```

## 10. Implementation Phases

| Phase | Description | Deliverable | Estimated Effort |
|-------|-------------|-------------|------------------|
| P0 | Parameter signature tracking | Detect recompilation necessity, fast path validation | 1-2 days |
| P1 | Async parameter updates | Non-blocking parameter buffer uploads, double-buffering | 2 days |
| P2 | Async compilation tracking | Non-blocking program builds, UI shows progress | 2-3 days |
| P3 | Async bbox computation | BBox updates don't block UI, use worker queue | 1-2 days |
| P4 | Async SDF precomputation | SDF generation off main thread, event-based | 1-2 days |
| P5 | Job chaining & state machine | Proper stage transitions, cancellation | 2-3 days |
| P6 | Fallback rendering modes | Graceful degradation, best-available rendering | 1-2 days |
| P7 | UI progress indication | Visual feedback for all stages | 1 day |
| P8 | Error handling & recovery | Robust error states, user notifications | 1-2 days |
| P9 | Testing & optimization | Stress tests, race condition fixes, perf tuning | 2-3 days |

**Total: 13-21 days**

## 11. Testing Strategy

### 11.1 Unit Tests

- [ ] Parameter signature computation (various types)
- [ ] Signature change detection (add/remove/reorder params)
- [ ] Fast path parameter updates (no recompilation)
- [ ] Slow path triggering (signature mismatch)
- [ ] Parameter buffer double-buffering
- [ ] Compilation cancellation (rapid model edits)
- [ ] BBox computation with invalid program state
- [ ] SDF precomputation with changing bounding box
- [ ] Job chaining (compile → bbox → sdf)
- [ ] Job chaining (parameter → bbox → sdf)
- [ ] Error recovery from each stage

### 11.2 Integration Tests

- [ ] Rapid parameter changes (stress test cancellation)
- [ ] Mixed parameter + structural changes
- [ ] Parameter change during compilation
- [ ] Parameter signature mismatch handling
- [ ] Model load during in-flight update
- [ ] Render quality verification at each stage
- [ ] Memory leak detection (especially OpenCL events)
- [ ] Multi-document scenarios
- [ ] Parameter buffer corruption detection

### 11.3 Performance Tests

- [ ] Baseline: synchronous update time (params vs full)
- [ ] Async: UI responsiveness during update
- [ ] Fast path parameter update latency
- [ ] Signature computation overhead
- [ ] Parameter buffer swap overhead
- [ ] Measure time-to-first-render vs time-to-HQ-render
- [ ] Worker thread utilization
- [ ] GPU queue saturation avoidance

## 12. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Parameter signature mismatch not detected | High | Extensive validation, unit tests for all param types |
| Race between parameter update & rendering | High | Double-buffering, atomic swap flags |
| Incorrect signature causes kernel crash | Critical | Pre-upload validation, bounds checking |
| Fast path incorrectly chosen (signature collision) | Medium | Use cryptographic hash or full comparison |
| OpenCL build can't be cancelled | Medium | Mark stale builds, don't swap program |
| Race between model source generation & compilation | High | Atomic epoch checks, version model source |
| GPU memory exhaustion (double buffering SDF + params) | Medium | Lazy allocation, reuse buffers |
| Deadlock between render & update jobs | High | Separate worker queues, careful mutex ordering |
| Incorrect rendering with stale SDF | Medium | Epoch-based validation, clear fallback modes |

## 13. Acceptance Criteria

- [ ] UI remains responsive (<16ms frame time) during any model update
- [ ] Parameter-only changes appear within 50ms (fast path)
- [ ] Low-res preview appears within 100ms of model change
- [ ] High-quality render completes within 2x of synchronous baseline
- [ ] Fast path correctly chosen 100% of time (no false positives)
- [ ] Signature mismatch detected 100% of time (no false negatives)
- [ ] Rapid edits (>10 per second) handled gracefully without crashes
- [ ] No visible artifacts from stale SDF/program/parameter usage
- [ ] Memory usage stable across 100+ rapid model changes
- [ ] Error messages shown to user for compilation failures
- [ ] Parameter buffer corruption never occurs (validated by tests)

## 14. Future Enhancements

1. **Smart Parameter Grouping**: Batch parameter updates that don't affect bbox/SDF
2. **Incremental Compilation**: Recompile only changed functions
3. **Cached BBox**: Analytical bbox for primitives (skip GPU kernel)
4. **Dirty Parameter Tracking**: Only recompute SDF for params that affect geometry
5. **Progressive SDF**: Coarse-to-fine SDF refinement with co_yield
6. **Adaptive SDF Resolution**: Dynamic resolution based on model complexity
7. **SDF Reuse**: Partial SDF updates for localized model changes

## 15. References

- `async_rendering_coroutines_plan.md` - Base async rendering architecture
- `ProgramBase.cpp:56` - Current async compilation implementation
- `ComputeCore.cpp:640` - BBox computation logic
- `SlicerProgram.cpp:140` - SDF precomputation kernel

---

**Next Steps**: 
1. Review plan with team
2. Implement P1 (async compilation tracking) behind feature flag
3. Integrate with existing `AsyncRenderController`
4. Validate UI responsiveness with large models
