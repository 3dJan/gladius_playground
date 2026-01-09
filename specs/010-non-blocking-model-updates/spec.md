# Feature Specification: Non-Blocking Model Updates

**Feature Branch**: `010-non-blocking-model-updates`  
**Created**: 2026-01-09  
**Status**: Draft  
**Input**: User description: "When modifying a model in the ModelEditor (modification of the graph that will trigger code generation and recompilation, or parameter changes that will just trigger a preview and bounding box update) should never block the main thread, so that the user always gets a fluid user experience."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Fluid Parameter Editing (Priority: P1)

As a user editing model parameters (e.g., dragging a slider to adjust a sphere radius), I want the UI to remain responsive at all times, so that I can see immediate visual feedback without lag or freezing.

**Why this priority**: Parameter editing is the most frequent user interaction. Users expect immediate visual feedback when adjusting values. Any lag here directly impacts perceived application quality and user productivity.

**Independent Test**: Can be fully tested by dragging any parameter slider rapidly while a model is displayed. Delivers immediate visual feedback without input lag.

**Acceptance Scenarios**:

1. **Given** a model with editable parameters is open in the ModelEditor, **When** the user drags a parameter slider, **Then** the slider follows the mouse without delay and the preview updates progressively.

2. **Given** the GPU is busy computing a previous parameter change, **When** the user makes another parameter adjustment, **Then** the UI remains responsive and the latest parameter value is used for the next render.

3. **Given** background compilation is in progress, **When** the user modifies a parameter value, **Then** the input controls remain responsive and the parameter change is queued for when compilation completes.

---

### User Story 2 - Preview Busy Indicator During Compute (Priority: P1)

As a user viewing the 3D preview, I want to see a clear busy indicator overlay when the system is computing (kernel compilation, bounding box, SDF precomputation), so that I understand the system is working and know when results are ready.

**Why this priority**: Users need visual feedback that the system is processing their changes. Without an indicator, a frozen preview appears broken. This is tied with P1 as it directly affects perceived responsiveness.

**Independent Test**: Can be fully tested by making a graph change that triggers recompilation. Delivers clear visual feedback that computation is in progress.

**Acceptance Scenarios**:

1. **Given** the user makes a change that triggers kernel recompilation, **When** compilation begins, **Then** the preview shows a busy indicator overlay on top of the last valid frame.

2. **Given** bounding box computation or SDF precomputation is running (distinct from kernel compilation), **When** the preview displays, **Then** the busy indicator remains visible until all pending compute operations complete.

3. **Given** computation completes successfully, **When** new results are ready, **Then** the busy indicator disappears and the updated preview is shown.

**Note**: A busy indicator exists in `RenderWindow.cpp` (lines 580-608) that shows during compilation when `!isRendererReady() || isAnyCompilationInProgress()`. Investigation needed to determine if it's not triggering in certain scenarios (potential regression) or if additional compute states need to trigger it (e.g., bounding box computation, SDF precomputation).

---

### User Story 3 - Responsive Graph Editing (Priority: P2)

As a user modifying the node graph (adding nodes, connecting links, deleting elements), I want the graph editor to remain fluid during the subsequent code generation and recompilation, so that I can continue editing without waiting.

**Why this priority**: Graph modifications trigger expensive OpenCL recompilation. Users should not be blocked from continuing their editing workflow while compilation happens in the background.

**Independent Test**: Can be fully tested by adding/removing nodes while compilation is active. Delivers continuous graph editing capability even during recompilation.

**Acceptance Scenarios**:

1. **Given** the user modifies the node graph (adds a node, creates a link), **When** code generation and recompilation begins, **Then** the graph editor remains fully interactive for further edits.

2. **Given** recompilation is in progress from a previous edit, **When** the user makes additional graph changes, **Then** the new changes are queued and compiled after current compilation completes (or current compilation is cancelled and restarted with latest changes).

3. **Given** a complex model is being recompiled, **When** the user navigates between different functions in the model, **Then** navigation is instant without waiting for compilation.

---

### User Story 4 - Responsive Bounding Box Updates (Priority: P3)

As a user modifying parameters that affect model geometry, I want bounding box calculations to happen asynchronously, so that I don't experience UI freezes when the system recalculates model bounds.

**Why this priority**: Bounding box computation involves GPU synchronization which can cause noticeable stalls. Less frequent than parameter editing but still impacts fluid feel.

**Independent Test**: Can be tested by modifying parameters that change model size significantly. Delivers smooth UI while bounds are recalculated.

**Acceptance Scenarios**:

1. **Given** a parameter change affects model geometry, **When** the system updates the bounding box, **Then** the UI remains responsive during the calculation.

2. **Given** bounding box computation is in progress, **When** the user makes another parameter change, **Then** the pending bounding box calculation is superseded by a new calculation with the latest parameters.

---

### Edge Cases

- What happens when multiple rapid parameter changes occur faster than the GPU can render? System uses last-write-wins coalescing via `tryToupdateParameter()` - if mutex is busy, the update is skipped and retried next frame with the latest value. This naturally coalesces rapid changes to the most recent state.
- How does the system handle a parameter change during active mesh export? Export uses the parameter values at export start; UI edits do not block or affect ongoing export.
- What happens when recompilation fails due to invalid graph state? UI remains responsive and shows clear error feedback without blocking.
- How does the system behave when GPU memory is exhausted? Graceful degradation with error message, not UI freeze.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST allow parameter value modifications without blocking the main/UI thread.
- **FR-002**: System MUST display a busy indicator overlay on the preview while kernel compilation, bounding box computation, or SDF precomputation is in progress.
- **FR-003**: System MUST perform OpenCL code generation on a background thread, not the UI thread.
- **FR-004**: System MUST perform OpenCL compilation on a background thread, not the UI thread.
- **FR-005**: System MUST show the last valid preview frame with busy overlay while new computation proceeds.
- **FR-006**: System MUST perform bounding box computation without blocking UI interactions.
- **FR-007**: System MUST coalesce rapid parameter changes (>60 changes/second) using last-write-wins semantics to avoid queueing excessive GPU buffer updates. The existing `tryToupdateParameter()` pattern already implements this via non-blocking mutex acquisition.
- **FR-008**: System MUST maintain graph editor responsiveness during all background compute operations.
- **FR-009**: System MUST remove the busy indicator and display updated preview when computation completes.
- **FR-010**: System MUST preserve parameter change order when multiple changes occur during a single frame.

### Key Entities

- **ComputeToken**: Mutex-based synchronization primitive controlling access to compute resources. Must support both blocking and non-blocking acquisition.
- **ParameterBuffer**: GPU buffer holding runtime parameter values. Must be updatable without full recompilation.
- **BusyIndicatorState**: Tracks which compute operations are in progress (compilation, bounding box, SDF) to display appropriate overlay.
- **CompilationState**: Tracks background compilation progress for UI feedback.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Parameter slider drag maintains 60fps UI responsiveness (ImGui frame time stays under 16ms during slider interaction, measured via `ImGui::GetIO().DeltaTime`).
- **SC-002**: Busy indicator appears within 100ms of triggering a compute operation.
- **SC-003**: Graph editor node drag/drop operations complete within 16ms regardless of compute state.
- **SC-004**: Preview shows busy overlay with last valid frame during computation (no blank/black screen).
- **SC-005**: Busy indicator disappears within 100ms of computation completing.
- **SC-006**: Time from parameter change to visible preview update is under 100ms for fast path (no recompilation needed).
- **SC-007**: User can continue editing graph while compilation runs without perceptible lag.

## Assumptions

- The existing async patterns (`refreshModelAsync()`, `precomputeSdfAsync()`) provide a proven foundation for this work.
- OpenCL command queues support proper event-based synchronization for async operations.
- The ImGui-based UI runs on a single thread that should never block on GPU operations.
- Triple-buffering or similar techniques can be used to decouple render production from display.
- The existing `requestComputeToken()` non-blocking API is the correct pattern to extend.

## Dependencies

- Existing async infrastructure in `Document.cpp` and `ComputeCore.cpp`.
- OpenCL event system for GPU operation tracking.
- ImGui rendering loop in `MainWindow.cpp`.

## Out of Scope

- GPU performance optimization of the actual ray marching/rendering algorithms.
- Multi-GPU support or compute distribution.
- Changes to the OpenCL kernel code generation logic itself.
- File I/O operations (loading/saving models) - these have their own blocking considerations.
