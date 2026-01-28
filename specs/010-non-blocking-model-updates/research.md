# Research: Non-Blocking Model Updates

**Feature**: 010-non-blocking-model-updates  
**Date**: 2026-01-09

## Executive Summary

This feature builds on the existing async rendering infrastructure from spec 003. The codebase already has robust async patterns; the remaining work is:

1. **Extend busy indicator** to cover SDF precomputation and bounding box computation
2. **Verify no blocking** on the main thread during model updates
3. **Confirm existing patterns** work correctly for the model editing use case

---

## Existing Async Infrastructure

### 1. Background Model Refresh (Document.cpp)

```cpp
bool Document::refreshModelAsync()
{
    // Validation...
    m_futureModelRefresh = std::async(std::launch::async, [&]() { refreshWorker(); });
    return true;
}
```

**Worker performs**:
- `loadAllMeshResources()`
- `updateParameterRegistration()`  
- `updateParameter()`
- `m_core->refreshProgram(m_flatAssembly)`
- `m_core->recompileIfRequired()`
- `m_core->precomputeSdfAsync(queue)` + wait
- `m_core->updateBBox()`

✅ **Status**: This runs entirely on background thread - UI is not blocked.

### 2. Parameter Updates (Document.cpp + ComputeCore.cpp)

```cpp
void Document::updateParameter()
{
    // Uses tryToupdateParameter() which is non-blocking
    if (!m_core->tryToupdateParameter(*m_assembly))
    {
        return false;  // Will retry next frame
    }
}
```

**Fast path** (parameter signature compatible):
- `tryToupdateParameter()` uses `try_lock()` - non-blocking
- If mutex busy, returns false and sets `m_parameterDirty` for retry

✅ **Status**: Non-blocking by design.

### 3. Preview Rendering (RenderWindow.cpp)

From spec 003, async preview is implemented:
- `renderLowResPreviewAsync()` returns `cl::Event`
- Triple buffering with `AsyncRenderController`
- Result polling via `tryConsumePreviewResult()`

✅ **Status**: Phases 1-3 of spec 003 complete.

### 4. Busy Indicator (RenderWindow.cpp:580-608)

```cpp
if (!m_core->isRendererReady() || m_core->isAnyCompilationInProgress())
{
    // Show red spinning indicator
    ui::loadingIndicatorCircle("compiling", ...);
}
```

⚠️ **Gap**: Does not cover:
- SDF precomputation in progress
- Bounding box computation in progress

---

## Blocking Points Analysis

### RenderWindow.cpp:268 - `waitForComputeToken()`

```cpp
auto token = m_core->waitForComputeToken();
```

**Context**: Called at start of `renderWindow()` main render path.

**Analysis**: This IS blocking, but analysis needed to determine if the async preview path bypasses it. Looking at the flow:

1. If async preview is active → preview frames come from `AsyncRenderController` front buffer
2. If synchronous → this blocks

**Risk**: Medium - may block during transition or fallback cases.

### RenderWindow.cpp:2387 - `waitForComputeToken()` in Coroutine

```cpp
auto const commitSdfSuccess = [this]()
{
    auto computeToken = m_core->waitForComputeToken();  // BLOCKS
    m_core->setSdfValid(true);
    m_core->updateBBox();
};
```

**Context**: Inside `sdfPrecomputeCoroutine()` after SDF completes.

**Analysis**: This is called from coroutine context. The coroutine runs on the UI thread's task scheduler, but the blocking lambda is executed synchronously.

**Risk**: Low - `setSdfValid()` and `updateBBox()` are fast once SDF is done.

### ComputeCore::updateBoundingBoxFast() - `queue.finish()`

```cpp
CL_ERROR(m_ComputeContext->GetQueue().finish());
```

**Context**: GPU sync inside mutex-protected method.

**Analysis**: Always called from background thread (inside `refreshWorker()` or coroutine worker).

**Risk**: None - not on UI thread.

---

## State Tracking Gaps

### Current State Queries

| Query | Method | Returns |
|-------|--------|---------|
| Renderer ready | `isRendererReady()` | Checks mesh state + program state |
| Compilation in progress | `isAnyCompilationInProgress()` | Checks ProgramManager state |
| SDF valid | `isSdfValid()` | Returns `m_precompSdfIsValid` |
| Busy | `isBusy()` | Inverse of SDF valid + compilation + ready |

### Missing State Queries

| Needed Query | Purpose | Implementation |
|--------------|---------|----------------|
| SDF computation in progress | Show busy during async SDF | Track via atomic flag |
| Bounding box computation in progress | Show busy during bbox calc | Track via atomic flag |

---

## Decisions

### Decision 1: Add State Tracking for Busy Indicator

**Chosen approach**: Add atomic flags to track ongoing operations

```cpp
// ComputeCore.h
std::atomic<bool> m_sdfComputationInProgress{false};
std::atomic<bool> m_boundingBoxComputationInProgress{false};

[[nodiscard]] bool isSdfComputationInProgress() const;
[[nodiscard]] bool isBoundingBoxComputationInProgress() const;
```

**Rationale**: Minimal intrusion, thread-safe, follows existing patterns.

### Decision 2: Investigate RenderWindow:268 Blocking

**Chosen approach**: Analyze async path to determine if this is dead code in async mode

If not dead code:
- Option A: Replace with `requestComputeToken()` + skip frame
- Option B: Guard with `if (!asyncMode)` check

### Decision 3: Keep Existing Infrastructure

**Chosen approach**: Do not modify `refreshModelAsync()`, `tryToupdateParameter()`, or async preview

**Rationale**: These already work correctly. Focus on:
1. Adding missing busy indicator triggers
2. Verifying no regressions

---

## Test Plan

### Manual Tests

1. **Parameter slider responsiveness**
   - Open model with sliders
   - Drag slider rapidly
   - Verify: No UI lag, preview updates progressively

2. **Graph edit responsiveness**
   - Add/remove nodes
   - Verify: Busy indicator appears
   - Verify: Graph editor remains responsive

3. **Busy indicator visibility**
   - Make graph change triggering recompile
   - Verify: Red spinner appears over preview
   - Verify: Spinner disappears when complete

### Automated Tests

1. Unit test for state tracking atomics
2. Integration test for busy indicator trigger conditions

---

## Alternatives Considered

### Alternative 1: Full Async Render Path Rewrite

**Rejected**: Spec 003 already implemented async preview. No need for rewrite.

### Alternative 2: Lock-Free Parameter Updates

**Rejected**: Current `try_lock()` pattern is sufficient and simpler.

### Alternative 3: Dedicated Render Thread

**Rejected**: Would require significant architecture changes. Current coroutine + async pattern is adequate.

---

## References

- [spec.md](spec.md) - Feature specification
- [specs/003-async-preview-rendering/tasks.md](../003-async-preview-rendering/tasks.md) - Related async work
- [review.md](/review.md) - Code review identifying blocking patterns
- [docs/architecture/rendering_pipeline.md](/docs/architecture/rendering_pipeline.md) - Render architecture
