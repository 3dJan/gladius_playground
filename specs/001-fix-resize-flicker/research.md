# Research: Fix Resize Flicker

**Phase**: 0 - Outline & Research  
**Date**: 2026-01-06  
**Status**: Complete

## Research Tasks

### Task 1: Window Resize Handling Patterns in ImGui/OpenGL Applications

**Question**: What are the established patterns for preventing frame buffer clearing during window resize in ImGui applications with OpenGL/OpenCL interop?

**Findings**:

1. **ImGui Best Practices**:
   - `ImGui::Image()` displays a texture by ID; it does not manage the underlying texture lifecycle
   - Window resize is detected via `ImGui::GetWindowWidth()` and `ImGui::GetContentRegionMax()`
   - The displayed texture persists until explicitly rebound or cleared

2. **OpenGL/OpenCL Interop Pattern**:
   - Shared GL/CL buffers preserve content until explicitly modified
   - Resizing the window does NOT automatically clear the GL texture
   - Clearing only happens when application explicitly calls `glClear()` or modifies buffer content

3. **Current Gladius Behavior** (from codebase analysis):
   - `RenderWindow::renderWindow()` (lines 488-503): Detects window size changes
   - When size changes, calls `invalidateView()` which:
     - Sets `m_dirty = true`
     - Sets `m_renderWindowState.isMoving = true`
     - Resets `currentLine = 0` (progressive rendering state)
     - Forces low-res render on next frame
     - Calls `notifyAsyncEpochIncrement()` (cancels in-flight rendering)
   - The `render()` or `renderAsync()` methods then re-render from scratch
   - **Problem**: The existing render buffer content is not explicitly cleared, but the viewport may show blank frames between the invalidation and the new low-res preview completing

**Decision**: Preserve the existing frame buffer content by avoiding unnecessary clearing during resize. The GL texture will naturally persist and be displayed by ImGui until a new render completes.

**Rationale**: This aligns with modern GPU application best practices where frame buffers are expensive resources that should be reused, not cleared unnecessarily.

---

### Task 2: OpenGL Texture Preservation During Viewport Changes

**Question**: How can we ensure the OpenGL texture remains displayed during window resize while adapting to new dimensions?

**Findings**:

1. **GL Texture Lifecycle**:
   - `ImageRGBA` class (from memory: wraps OpenCL/OpenGL shared image buffer)
   - `bind()` / `unbind()` methods sync CL buffer to GL texture
   - Content persists in GL until overwritten
   - `invalidateContent()` marks buffer as dirty for next sync

2. **ImGui Display Behavior**:
   - `ImGui::Image(textureId, size)` displays whatever is currently in the GL texture
   - If texture dimensions don't match display size, ImGui handles scaling
   - No automatic clearing occurs during resize

3. **Current Issue Analysis**:
   - `invalidateView()` triggers epoch increment, canceling in-flight rendering
   - New low-res preview is scheduled but may take 10-50ms to complete
   - During this gap, the previous render's GL texture should still be displayed
   - **Hypothesis**: Flicker may be caused by explicit clearing or by delay in scheduling preview

4. **Gladius Rendering Pipeline** (from memory):
   - Async rendering writes to `FrameBuffer` objects with triple buffering
   - `m_core->getResultImage()` returns the current display buffer
   - This buffer is bound to GL and displayed via `ImGui::Image()`
   - On resize, if `setScreenResolution()` is called, it may reallocate buffers

**Decision**: 
- Do NOT call `setScreenResolution()` immediately during resize detection
- Keep displaying the existing (old-size) texture while scheduling new render
- Only resize buffers after low-res preview completes to avoid showing blank/uninitialized texture

**Rationale**: Trading a brief "scaled/stretched" old frame for visible flicker is a better UX trade-off.

**Alternatives Considered**:
- **Double buffering with preserved old frame**: Rejected due to memory overhead (2x GPU buffers)
- **Immediate resize with background color fill**: Rejected because this IS the flicker we're trying to eliminate
- **Immediate low-res render synchronously**: Rejected due to UI thread blocking (against async architecture)

---

### Task 3: Progressive Rendering and Resize Interaction

**Question**: How should progressive rendering interact with resize operations to prevent clearing?

**Findings**:

1. **Current Progressive Rendering**:
   - High-quality rendering happens in chunks (controlled by `renderingStepSize`)
   - `currentLine` tracks progress through the frame
   - On completion, `currentLine` resets to 0 and `m_dirty = false`

2. **Resize During Progressive Rendering**:
   - Current behavior: `invalidateView()` resets `currentLine = 0`, abandoning partial progress
   - Epoch increment cancels in-flight async jobs
   - Partial render progress is lost, frame starts over from scratch

3. **User Experience Impact**:
   - During manual resize (dragging window edge), continuous size changes occur
   - Each size change resets rendering, causing constant preview updates but never completing HQ
   - After resize stops, 1-second delay before HQ rendering begins (line 1612-1617 in RenderWindow.cpp)

**Decision**: On resize, preserve the last completed frame (either low-res preview or last completed progressive chunk) while scheduling new preview at new dimensions.

**Rationale**: Showing a slightly outdated but complete frame is better than showing clearing/flickering or incomplete progressive chunks.

---

### Task 4: DPI Changes and Multi-Monitor Scenarios

**Question**: Do DPI changes or multi-monitor transitions require special handling beyond simple resize?

**Findings**:

1. **ImGui DPI Handling**:
   - `ImGui::GetIO().FontGlobalScale` provides DPI scaling factor (used at line 1120)
   - Window size already accounts for DPI (in physical pixels)
   - `ImGui::GetWindowWidth()` returns scaled width

2. **Multi-Monitor Behavior**:
   - When window moves between monitors with different DPI, ImGui reports new size
   - Detected as normal resize by existing `m_renderWindowSize_px` comparison (lines 493-494)
   - No special handling needed beyond standard resize logic

3. **GL Texture Scaling**:
   - ImGui internally handles texture scaling if display size != texture size
   - Uses bilinear filtering by default (acceptable for temporary stretch during resize)

**Decision**: No special handling required for DPI/multi-monitor. Standard resize preservation logic covers these scenarios.

**Rationale**: ImGui and OpenGL abstract away DPI details; from application perspective, it's just a resize event.

---

## Summary of Research Outcomes

### Key Technical Findings

1. **Root Cause of Flicker**: `invalidateView()` on resize triggers epoch increment and rendering restart, potentially showing blank frames during the gap before new preview completes

2. **Preservation Mechanism**: GL textures naturally persist; no explicit clearing occurs unless application initiates it. Current flicker is likely a timing gap, not explicit clearing.

3. **Solution Strategy**: 
   - Continue displaying existing GL texture during resize
   - Schedule low-res preview without blocking
   - Only update display when new preview completes
   - Avoid premature buffer reallocation

### Technology Choices Validated

| Technology | Role | Justification |
|------------|------|---------------|
| OpenGL/ImGui | Display layer | Already in use; no changes needed |
| OpenCL | Rendering backend | Already in use; async architecture suitable for non-blocking preview updates |
| Triple buffering | Frame management | Already implemented in `AsyncRenderController`; supports smooth transitions |
| Epoch-based cancellation | Stale job handling | Already implemented; may need tuning to avoid premature cancellation during resize |

### Implementation Approach

Based on research, the implementation will focus on:

1. **Modify resize detection logic** (RenderWindow.cpp lines 488-503):
   - Keep displaying old texture when resize detected
   - Schedule low-res preview at new dimensions
   - Defer `setScreenResolution()` until preview completes

2. **Tune `invalidateView()` behavior** (lines 612-623):
   - Preserve display content (don't force immediate clear)
   - Schedule preview without epoch increment (or use separate resize-specific path)

3. **Test across scenarios**:
   - Manual edge/corner dragging
   - Maximize/restore
   - Multi-monitor DPI transitions
   - Rapid continuous resize (stress test)

### References

- ImGui source code: ImGui::Image rendering (no clearing on resize)
- OpenGL specification: Texture lifecycle and persistence
- Gladius codebase:
  - `gladius/src/ui/RenderWindow.cpp` (resize handling, invalidation logic)
  - `gladius/src/ui/RenderWindow.h` (class structure, async rendering members)
  - `.specify/memory/rendering_pipeline_overview.md` (async architecture documentation)
