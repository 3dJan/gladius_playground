# Gladius Rendering Pipeline Overview

## Architecture Summary

Gladius uses a **sphere-tracing (ray marching)** approach on the GPU (OpenCL) to render implicit surfaces defined by Signed Distance Fields (SDFs). The rendering pipeline consists of:

1. **OpenCL Kernels** - GPU compute kernels for SDF evaluation and ray marching
2. **RenderProgram** - C++ class orchestrating kernel execution
3. **ComputeCore** - Central compute management and resource handling
4. **RenderWindow** - UI integration with async rendering support

## OpenCL Kernel Architecture

### Core Kernel Files

| File | Purpose |
|------|---------|
| `renderer.cl` | Main entry point: `renderScene` kernel, `resample` |
| `rendering.cl` | Ray casting (`rayCast`), shading (`shadingMetal`, `determineColor`), normals |
| `sdf.cl` | SDF primitives (box, cylinder, mesh, etc.), CSG ops (union, difference, intersection) |
| `sdf_generator.cl` | SDF precomputation kernel (`preComputeSdf`), marching squares |
| `mesh_sdf.cl` | Mesh-based SDF with BVH traversal and voxel acceleration |

### Ray Marching Implementation

The `rayCast` function in `rendering.cl` implements sphere tracing with:

1. **Adaptive stepping**: Step size = SDF value (distance to nearest surface)
2. **Binary refinement**: When surface crossing is detected, binary search refines intersection
3. **Slope detection**: Detects gradient sign changes near thin features
4. **Early termination**: Exits when close enough (`closeEnough` threshold) or max distance exceeded

Key parameters:
- `maxRaySteps = 2000` - Maximum iterations
- `maxTravelDistance = 100000` - Maximum ray travel
- `closeEnough` - Dynamic threshold based on distance traveled

```c
// Simplified ray march loop structure
for (int i = 0; i < maxRaySteps; ++i) {
    hitObject = mapCached(rayPos, PASS_PAYLOAD_ARGS);
    currentSignedDistance = hitObject.signedDistance;
    
    // Binary refinement on surface crossing
    if (distanceSignChanged || slopeSignChanged || isCloseToSurface) {
        // Binary search to find precise intersection
    }
    
    // Advance ray by SDF value
    nextStep = max(currentAbsDistance * nearRangeFactor, minStepSize);
    traveledDistance += nextStep;
    
    if (currentAbsDistance < closeEnough) {
        hit = true;
        break;
    }
}
```

### SDF Evaluation Modes

The `modelInternal` function supports three modes controlled by `renderingSettings.approximation`:

| Mode | Description |
|------|-------------|
| `AM_FULL_MODEL` | Direct SDF function evaluation (accurate, slower) |
| `AM_HYBRID` | Use precomputed SDF when far from surface, function near surface |
| `AM_ONLY_PRECOMPSDF` | Use only precomputed 3D texture (fast preview) |

### Precomputed SDF

The `preComputeSdf` kernel generates a 3D texture containing cached SDF values:

```c
void kernel preComputeSdf(__write_only image3d_t target, struct BoundingBox bbox, PAYLOAD_ARGS) {
    // Map voxel coordinate to world space
    float3 pos = normalizedPosToBuildVolume(normalizedPos, bbox);
    renderingSettings.approximation = AM_FULL_MODEL;  // Always use full model
    float sdf = modelInternal(pos, PASS_PAYLOAD_ARGS).w;
    write_imagef(target, coord, sdf);
}
```

The cached SDF is sampled in `cachedSdf()` with trilinear interpolation.

## C++ Rendering Classes

### RenderProgram

Wraps OpenCL kernel execution:
- `renderScene()` - Blocking render
- `renderSceneAsync()` - Non-blocking, returns `cl::Event`
- `resample()` / `resampleAsync()` - Scale low-res to high-res image

Kernel arguments passed via `PAYLOAD_ARGS` macro include:
- Build area, primitives buffer, SDF buffer
- Rendering settings, parameter buffer, command buffer
- Precomputed SDF 3D texture and bounding box
- Camera (eye position, model-view-perspective matrix)

### ComputeCore

Central coordinator providing:
- `waitForComputeToken()` - Mutex for serializing GPU access
- `renderScene()` / `renderSceneComputeOnly()` - High-level render calls
- `renderLowResPreview()` / `renderLowResPreviewAsync()` - Fast preview renders
- `precomputeSdfForBBox()` / `precomputeSdfAsync()` - SDF generation
- `setSdfValid()` / `isSdfValid()` - SDF validity tracking

## Async Rendering System

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      UI Thread                          │
│  RenderWindow::renderWindow() → render() → renderAsync()│
│                         │                               │
│         ┌───────────────┼───────────────┐               │
│         ▼               ▼               ▼               │
│  processAsyncResults  scheduleJob  promoteFrontBuffer   │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│              AsyncRenderController                       │
│  - Coroutine thread pool                                │
│  - Job queue (enqueue/dequeue)                          │
│  - Triple buffering (Idle → Writing → Ready → Front)   │
│  - Separate worker CommandQueue (no GL interop)         │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│           Worker Coroutines (executeAsyncXxxJob)        │
│  - executeAsyncRenderJob() - HQ progressive rendering   │
│  - executeAsyncPreviewJob() - Low-res camera feedback   │
│  - executeAsyncSdfPrecomputation() - Async SDF gen      │
│  - executeAsyncBboxUpdate() - Bounding box computation  │
└─────────────────────────────────────────────────────────┘
```

### Job Types (RenderJobType)

| Type | Purpose |
|------|---------|
| `HighQuality` | Progressive HQ rendering (row-by-row chunks) |
| `LowResPreview` | Fast preview during camera movement |
| `BoundingBoxUpdate` | Compute model bounding box |
| `SDFPrecomputation` | Generate precomputed 3D SDF texture |
| `ParameterUpdate` | Fast parameter value updates |
| `ProgramCompilation` | Async OpenCL kernel compilation |

### Frame Buffer States (FrameState)

```
                    ┌─────────────────────────────┐
                    │                             │
                    ▼                             │
     ┌──────┐    ┌─────────┐    ┌───────┐    ┌───────┐
     │ Idle │───▶│ Writing │───▶│ Ready │───▶│ Front │
     └──────┘    └─────────┘    └───────┘    └───────┘
        ▲                                        │
        └────────────────────────────────────────┘
                    (epoch bump / release)
```

- **Idle**: Available for worker to acquire
- **Writing**: Worker is rendering into this buffer
- **Ready**: Worker finished, awaiting UI promotion
- **Front**: Currently displayed buffer

### Epoch-Based Cancellation

- Each model update increments `m_asyncCurrentEpoch`
- Jobs carry their epoch; if `job.epoch < currentEpoch`, job is cancelled
- `notifyAsyncEpochIncrement()` releases stale buffers

### Progressive Rendering

High-quality rendering happens in chunks (row bands):

```cpp
job.startLine = state.currentLine;
job.stepSize = state.renderingStepSize;  // Adaptive based on timing

// After completion, advance:
state.currentLine += result.completedLine;
```

Chunk size adapts to maintain ~100ms target per chunk.

### Low-Res Preview Path

During camera movement:
1. Schedule `LowResPreview` job (lower resolution)
2. Render to `m_lowResPreviewImage` 
3. On completion, resample to `m_resultImage` on UI thread
4. Display immediately for smooth interaction

## Key Data Flow

### Model Update → Render

1. Parameter/node change triggers `invalidateViewDuetoModelUpdate()`
2. `Document::refreshModelAsync()` recompiles OpenCL kernels
3. `notifyAsyncEpochIncrement()` cancels stale jobs
4. `precomputeSdfAsync()` regenerates 3D SDF cache
5. `scheduleAsyncRenderJob()` queues progressive HQ render
6. Worker executes `executeAsyncRenderJob()` calling `RenderProgram::renderSceneAsync()`
7. `processAsyncResults()` promotes finished buffers
8. UI displays front buffer

### Camera Movement → Preview

1. Mouse drag detected → `m_renderWindowState.isMoving = true`
2. `renderAsync()` schedules `LowResPreview` job
3. Worker renders at low resolution using `renderLowResPreviewAsync()`
4. Result resampled to display size
5. UI shows low-res image for smooth feedback
6. On camera stop, progressive HQ render begins
