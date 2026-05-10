# Feature Specification: Asynchronous Preview Rendering

**Feature Branch**: `003-async-preview-rendering`  
**Created**: 2026-01-02  
**Status**: Draft  
**Input**: User description: "Make preview rendering asynchronous to keep UI fluid at 55+ FPS during camera movement"

## Problem Statement

Currently, when users move the camera in the 3D preview window, the UI frame rate drops significantly below the target 60 FPS. This creates a jarring, unresponsive user experience during one of the most common interactions in the application.

**Root Cause Analysis**:
- Preview rendering (low-resolution with precomputed SDF) is executed synchronously on the UI thread
- The `renderLowResPreview()` function contains blocking GPU synchronization calls (`glFinish()`)
- Texture transfers between GPU and CPU block the UI thread
- In contrast, high-quality progressive rendering is already asynchronous and maintains 60 FPS

The existing async rendering infrastructure already supports a `LowResPreview` job type, suggesting this was the intended architecture but was not fully implemented.

### Main Thread Blocking Points (to be eliminated)

The following operations currently execute on the main/UI thread and block it:

1. **`glFinish()` in `renderLowResPreview()`**: Forces CPU to wait for all GPU commands to complete before continuing
2. **`m_core->waitForComputeToken()`**: Blocking mutex acquisition at the start of `renderWindow()` that waits for compute resources
3. **`RenderProgram::renderScene()` call**: Synchronous GPU kernel dispatch for the preview render pass
4. **`RenderProgram::resample()` call**: Synchronous upscaling from low-res to display resolution
5. **`m_resultImage->bind()`**: In `readpixel` mode, triggers synchronous `clFinish()` and pixel transfer from GPU to CPU
6. **`glTexImage2D()` in `transferPixels()`**: Synchronous upload of pixel data to GL texture

Each of these operations can individually block the UI thread for several milliseconds. Combined, they cause the observed FPS drops during camera movement.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Fluid Camera Navigation (Priority: P1)

As a 3D modeler, I want the preview to remain responsive while I orbit, pan, and zoom around my model, so that I can explore my design without frustrating stutters or lag.

**Why this priority**: Camera navigation is the most frequently used interaction in the 3D viewport. Poor responsiveness here directly impacts every user session and creates a perception of low-quality software.

**Independent Test**: Can be tested by loading any model, moving the camera continuously for 10 seconds, and observing the FPS counter in the status bar.

**Acceptance Scenarios**:

1. **Given** a model is loaded in the preview window, **When** the user continuously orbits the camera using mouse drag, **Then** the UI frame rate remains at or above 55 FPS throughout the interaction
2. **Given** a model is loaded in the preview window, **When** the user pans the camera using middle-mouse drag, **Then** the preview updates smoothly without visible frame drops
3. **Given** a model is loaded in the preview window, **When** the user zooms using mouse wheel, **Then** each zoom step feels instantaneous with no perceptible delay

---

### User Story 2 - Visual Feedback During Camera Movement (Priority: P1)

As a 3D modeler, I want to see an updated preview image while moving the camera (even if low-resolution), so that I can understand where I am navigating in the scene.

**Why this priority**: Users need visual feedback to navigate effectively. A fluid but static image would be disorienting.

**Independent Test**: Can be tested by moving the camera and verifying the preview image updates to reflect the new viewpoint.

**Acceptance Scenarios**:

1. **Given** the user is actively moving the camera, **When** the camera position changes, **Then** a low-resolution preview reflecting the new viewpoint is displayed within 100ms
2. **Given** the user stops moving the camera, **When** no camera input is detected for 200ms, **Then** progressive high-quality rendering begins automatically

---

### User Story 3 - Graceful Degradation Under Load (Priority: P2)

As a user with a complex model, I want the system to prioritize UI responsiveness over preview quality, so that the application remains usable even with demanding scenes.

**Why this priority**: Complex models may exceed the GPU's ability to render previews fast enough. The system should degrade gracefully rather than become unresponsive.

**Independent Test**: Can be tested by loading a very complex model and verifying UI remains responsive even if preview updates are delayed.

**Acceptance Scenarios**:

1. **Given** a complex model that takes longer than 16ms to render at preview resolution, **When** the user moves the camera, **Then** the UI remains responsive at 55+ FPS while preview frames may be dropped
2. **Given** the preview rendering cannot keep up with camera movement, **When** the user continues to move the camera, **Then** the most recent completed preview frame is displayed (no visual artifacts or corruption)

---

### Edge Cases

- What happens when the user rapidly switches between camera movement and stillness? (System should smoothly transition between preview and HQ modes without visual glitches)
- How does the system handle preview rendering when an SDF precomputation is in progress? (Preview should use the last valid SDF or fall back to full model rendering)
- What happens if the async preview rendering consistently fails to complete? (System should fall back to synchronous rendering after a timeout to ensure the user sees something) **[Out of scope for MVP; covered by FR-010 fallback to last valid frame]**
- How does the system behave when preview buffer allocation fails due to memory constraints? (Should log a warning and continue with reduced functionality rather than crash) **[Covered by T022/T023 error recovery tasks]**

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST execute preview rendering on a background thread, separate from the UI thread
- **FR-002**: System MUST maintain UI frame rate of at least 55 FPS during continuous camera movement
- **FR-003**: System MUST display the most recently completed preview frame while a new frame is being rendered
- **FR-004**: System MUST ensure preview frames are displayed in order (no older frames appearing after newer ones)
- **FR-005**: System MUST cancel in-flight preview render jobs when any camera movement input is detected
- **FR-006**: System MUST reuse the existing async rendering infrastructure (`AsyncRenderController`, coroutine backend) for preview jobs
- **FR-007**: System MUST use the existing `RenderJobType::LowResPreview` job type for async preview work
- **FR-008**: System MUST ensure thread-safe access to shared GPU resources between preview and HQ rendering jobs
- **FR-009**: System MUST display preview frames within 100ms from camera movement to visible update
- **FR-010**: System MUST fall back gracefully if async preview rendering fails (e.g., continue showing the last valid frame)
- **FR-011**: System MUST NOT call blocking GPU synchronization (`glFinish()`, `clFinish()`) on the UI thread during preview rendering
- **FR-012**: System MUST NOT block the UI thread waiting for compute mutex acquisition during camera movement

### Key Entities

- **PreviewFrame**: A low-resolution rendered image of the scene from a specific camera viewpoint, used for real-time feedback during navigation
- **RenderJob**: An asynchronous work item describing a rendering task, including job type (preview vs HQ), camera parameters, and target buffer
- **FrameBuffer**: A GPU image buffer used for triple-buffering to decouple rendering from display
- **Epoch**: A version counter used to invalidate stale render jobs when camera or model parameters change

## Assumptions

- The existing async rendering infrastructure (coroutine backend, triple buffering, epoch tracking) is stable and can handle additional job types
- GPU memory is sufficient to maintain both preview and HQ frame buffers simultaneously
- Preview rendering using precomputed SDF is significantly faster than full model evaluation (under 10ms for typical scenes)
- The `LowResPreview` resolution (typically 1/4 to 1/10 of full resolution) provides sufficient quality for navigation feedback

## Clarifications

### Session 2026-01-02

- Q: What defines a "significant" camera position change that should trigger job cancellation? → A: Any camera movement input cancels in-flight preview jobs (simplest approach, ensures freshest frame is always rendered)
- Q: Should there be a fallback behavior when preview latency exceeds the target threshold? → A: Reduce preview resolution dynamically to meet latency target (already implemented in current codebase)

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: UI frame rate remains at or above 55 FPS during continuous camera movement (measured via status bar FPS display)
- **SC-002**: Preview frame latency (time from camera input to visible frame update) is under 100ms for 95% of frames
- **SC-003**: No visual artifacts, frame corruption, or race conditions observed during camera navigation
- **SC-004**: Memory usage for frame buffers does not exceed 2x the current implementation
- **SC-005**: Zero regressions in existing async HQ rendering functionality (progressive rendering still works correctly)
- **SC-006**: Application remains responsive and does not freeze even when preview rendering takes longer than expected
