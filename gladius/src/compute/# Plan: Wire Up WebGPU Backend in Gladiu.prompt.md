# Plan: Wire Up WebGPU Backend in Gladius

## TL;DR
`OpenCLComputeRenderer`, `RenderBackendSession`, and `WebGPUComputeRenderer` are fully implemented but **never instantiated in production code**. The rendering pipeline goes directly through `ComputeCore` → OpenCL kernels. This plan wires the new API-neutral renderer contract into the existing `RenderWindow` path, then adds runtime backend selection (Phase 6 from the architecture plan).

## Current State
- **Production rendering**: `Application` → `MainWindow` → `RenderWindow::render()` → `m_core->renderScene()` / `AsyncRenderController` → OpenCL kernels directly
- **New infrastructure** (all in `gladius/src/`):
  - `IComputeRenderer`, `IRenderSubmission`, `RenderBackendSession` — API-neutral contracts
  - `OpenCLComputeRenderer` — adapter over retained `RenderSession` / `RenderProgram` path
  - `WebGPUComputeRenderer` — materializes analytic snapshots, submits via Dawn
  - `NeutralRenderScheduler` — bridge from coordinator tasks through any `IComputeRenderer`
  - `OpenGLFramePresenter` — CPU RGBA → OpenGL texture upload (UI-thread only)
- **Problem**: None of these are connected to production. `NeutralRenderScheduler` is constructed with a no-op factory that always returns `std::nullopt`.

## Steps

### Phase A — Connect OpenCL through the new renderer contract
*Goal: Verify the adapter path works without changing visual output.*

1. **Add `OpenCLComputeRenderer` member to `RenderWindow`** (or pass via constructor)
   - File: `gladius/src/ui/RenderWindow.h`, add `std::unique_ptr<compute::IComputeRenderer> m_openCLRenderer;`
   - File: `gladius/src/ui/RenderWindow.cpp`, instantiate in `initialize()`: `m_openCLRenderer = std::make_unique<compute::OpenCLComputeRenderer>(core->createRenderSession());`

2. **Wire `NeutralRenderScheduler` with a real request factory**
   - File: `gladius/src/ui/RenderWindow.h`, replace no-op factory with one that reads from `m_core`:
     ```cpp
     m_neutralRenderScheduler = NeutralRenderScheduler{[this](auto& req) {
         // Convert ComputeCore state → compute::RenderRequest
         return convertToRenderRequest(req);
     }};
     ```

3. **Route one render path through `NeutralRenderScheduler`** (e.g., sync-only first)
   - In `RenderWindow::renderSync()`, call `m_neutralRenderScheduler.submit()` instead of direct `m_core->renderScene()`
   - Connect `OpenGLFramePresenter` to upload completed frames

4. **Verify**: All existing OpenCL viewport tests pass, visual output unchanged.

### Phase B — Wire WebGPU through the same contract
*Goal: Make WebGPU selectable and functional for analytic models.*

5. **Add backend selection to Application** (Phase 6 from architecture plan)
   - File: `gladius/src/Application.cpp`, add config/CLI parsing for `compute.backend` (values: `auto`, `opencl`, `webgpu`)
   - In constructor, after compute initialization:
     ```cpp
     auto backend = selectBackend(config); // auto | opencl | webgpu
     if (backend == ComputeBackendKind::WebGPU && webgpu::WebGPUComputeRenderer().isAvailable()) {
         m_rendererSession = std::make_unique<RenderBackendSession>(
             std::make_unique<webgpu::WebGPUComputeRenderer>());
     } else {
         // OpenCL path from Phase A
     }
     ```

6. **Pass `RenderBackendSession` to RenderWindow**
   - File: `gladius/src/ui/RenderWindow.h`, replace `m_openCLRenderer` with `std::unique_ptr<compute::RenderBackendSession> m_backendSession;`
   - In `initialize()`, accept the session and wire it into `NeutralRenderScheduler`

7. **Route all rendering through `NeutralRenderScheduler`** (both backends)
   - Replace direct `m_core->renderScene()` calls with scheduler submissions
   - `OpenGLFramePresenter` handles frame upload for both backends

8. **Add capability diagnostics**: If user selects WebGPU but document requires unsupported features, show error dialog instead of silently falling back to OpenCL.

### Phase C — Full migration and cleanup
*Goal: Remove direct ComputeCore rendering calls.*

9. Migrate async path (`renderAsync`) through `NeutralRenderScheduler`
10. Migrate progressive HQ rendering through the new contract
11. Route thumbnail/MCP rendering through renderer contract
12. Remove direct OpenCL image buffer access from UI thread (except via presenter)

## Relevant Files

| File | What to modify |
|------|---------------|
| `gladius/src/ui/RenderWindow.h` | Add `m_backendSession`, update constructor/signature |
| `gladius/src/ui/RenderWindow.cpp` | Wire instantiation, route render calls through scheduler |
| `gladius/src/Application.cpp` | Backend selection logic in constructor/init |
| `gladius/src/compute/OpenCLComputeRenderer.cpp` | Verify adapter works (no changes expected) |
| `gladius/src/webgpu/WebGPUComputeRenderer.cpp` | Verify availability check works |
| `gladius/src/ui/render/NeutralRenderScheduler.h/cpp` | May need request factory improvements |
| `gladius/src/ui/render/OpenGLFramePresenter.*` | Wire into RenderWindow for frame upload |

## Verification
1. **Unit tests**: Run `Run Gladius Tests (ReleaseWithDebug)` — all existing tests pass
2. **Visual regression**: Load a test document, render with OpenCL and WebGPU selected, compare output frames within tolerance
3. **Backend selection CLI**: Launch with `--backend opencl` and `--backend webgpu`, verify correct backend activates
4. **Capability error**: Select WebGPU for a document requiring unsupported features → see actionable error dialog

## Decisions
- **OpenCL first**: Phase A connects OpenCL through the new contract to validate the adapter path before touching WebGPU
- **No breaking changes**: Direct `ComputeCore` rendering calls remain until Phase C; gradual migration avoids regressions
- **WebGPU only for analytic models initially**: Resource-backed features (mesh SDF, VDB, etc.) remain OpenCL-only per architecture plan

## Further Considerations
1. **Backend selection UI**: Add a dropdown in settings vs. CLI flag? Recommend settings UI for discoverability.
2. **Fallback policy**: Architecture plan allows automatic fallback before work starts, but explicit WebGPU selection must not silently execute OpenCL. Implement as hard error with clear message.
3. **Platform testing**: Linux/Vulkan, Windows/D3D12, macOS/Metal — each needs smoke test per Phase 6 acceptance criteria.
