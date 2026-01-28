# Feature Specification: MDC Export Progress Indication

**Feature Branch**: `001-mdc-export-progress`  
**Created**: 2026-01-06  
**Status**: Draft  
**Input**: User description: "When exporting a mesh using the manifold dual contouring algorithm, the export should happen async and the progress bar should show the progress. this might be a bug or regression, we already had async mesh extraction with progress bar."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Async Export with Live Progress (Priority: P1)

As a user exporting a mesh using Manifold Dual Contouring, I want to see the export progress bar updating smoothly in real-time so that I know how much work remains and that the application is not frozen.

**Why this priority**: This is the core functionality requested. Users exporting large/complex models need visual feedback during potentially long-running operations to avoid thinking the application has frozen.

**Independent Test**: Can be fully tested by initiating an MDC export on a complex model and observing that the progress bar updates incrementally throughout the export process (not just jumping from 0% to 25% to 70% to 100%).

**Acceptance Scenarios**:

1. **Given** a user has a model loaded and initiates a Manifold Dual Contouring mesh export, **When** the export is running, **Then** the progress bar should update at least every 2 seconds with incremental progress values.

2. **Given** a user has initiated an MDC export, **When** viewing the export dialog, **Then** the progress bar should reflect the actual progress of the mesh generation and file writing stages.

3. **Given** an MDC export is in progress, **When** the user observes the UI, **Then** the application remains responsive and the progress bar animates smoothly.

---

### User Story 2 - Export Remains Non-Blocking (Priority: P1)

As a user, I want the mesh export to run asynchronously so that I can continue interacting with the application (e.g., view the model, access menus) while the export completes in the background.

**Why this priority**: Equally critical as P1-1; async operation prevents the UI from freezing during long exports.

**Independent Test**: Can be tested by starting an export and verifying the UI remains interactive (e.g., can rotate/zoom view, access menus).

**Acceptance Scenarios**:

1. **Given** an MDC export is in progress, **When** the user interacts with the viewport, **Then** the viewport should respond normally without lag.

2. **Given** an MDC export is in progress, **When** the user clicks "Cancel Export", **Then** the export should stop within 5 seconds and the UI should indicate cancellation.

---

### User Story 3 - Progress Bar Reflects Export Phases (Priority: P2)

As a user, I want the progress bar to reflect the different phases of the export (mesh generation, post-processing, file writing) so that I understand what stage the export is in.

**Why this priority**: Enhances user understanding but is not strictly required for functional async export with progress.

**Independent Test**: Can be tested by observing that progress advances through distinguishable phases rather than stalling at certain percentages.

**Acceptance Scenarios**:

1. **Given** an MDC export is running, **When** the mesh generation phase completes, **Then** the progress bar should be approximately at 60-70% (mesh generation is the longest phase).

2. **Given** an MDC export is running through all phases, **When** file writing begins, **Then** the progress bar should continue advancing from the post-processing level to 100%.

---

### Edge Cases

- What happens when the model is very simple and exports nearly instantly? Progress should still briefly show and complete gracefully without visual glitches.
- What happens when GPU acceleration fails and CPU fallback is used? Progress should continue updating during fallback execution.
- What happens if the export is cancelled mid-progress? Progress bar should stop and dialog should indicate cancellation.
- What happens when disk write fails at the end? Progress bar reaches near 100% but error state is shown with appropriate message.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST execute Manifold Dual Contouring mesh export asynchronously without blocking the main UI thread.
- **FR-002**: System MUST update the progress bar with the current export progress at least once every 2 seconds during the export.
- **FR-003**: System MUST report progress that reflects actual work completed, not just arbitrary checkpoints.
- **FR-004**: System MUST allow the user to cancel an in-progress export via the Cancel button.
- **FR-005**: System MUST display appropriate status messages during export (e.g., "Generating mesh...", "Writing file...").
- **FR-006**: System MUST report progress from all major export phases: mesh generation, optional post-processing (winding repair, simplification), and file writing.
- **FR-007**: System MUST maintain UI responsiveness during export (smooth interaction with no perceptible freeze).

### Assumptions

- The existing async infrastructure in the exporter is functional and should be preserved.
- Progress reporting granularity from mesh generation may be limited to chunk/octree level operations.
- Color sampling and post-processing phases can also provide progress updates.
- The current progress values (0.25, 0.7) observed in code suggest mesh generation progress is not being reported granularly during octree processing.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: During a 30-second or longer MDC export, the progress bar updates at least 15 times with distinct values.
- **SC-002**: The progress bar reaches 100% only when the export file has been successfully written to disk.
- **SC-003**: Users can cancel an export and see the cancellation acknowledged within 5 seconds.
- **SC-004**: The application remains visually responsive (no apparent freeze or stutter) during the entire export process.
- **SC-005**: The progress bar correctly reflects that mesh generation (the longest phase) consumes approximately 60% of the total progress range (5-65%).
