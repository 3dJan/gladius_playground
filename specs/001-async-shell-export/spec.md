# Feature Specification: Async Shell-Based Color Export

**Feature Branch**: `001-async-shell-export`  
**Created**: 2026-01-07  
**Status**: Draft  
**Input**: User description: "when exporting with manifold contouring, the meshes are watertight, the export is async, and the progress bar shows up. we want the same also for the shell based color export."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Non-Blocking Export with Progress Feedback (Priority: P1)

As a user exporting a model with shell-based color export (HueForge-style), I want the export to run in the background with a progress bar, so that the application remains responsive and I can see the export progress.

**Why this priority**: This is the core request - the export currently blocks the UI completely, making the application unresponsive during long exports. Users cannot see progress or cancel the operation.

**Independent Test**: Export a model with shell-based color export enabled and verify:
1. The UI remains responsive during export
2. Progress bar updates during export
3. Export completes successfully with valid output file

**Acceptance Scenarios**:

1. **Given** a model with volumetric color and shell export enabled, **When** user clicks Export, **Then** the progress bar appears immediately and updates as shells are generated
2. **Given** shell export is in progress, **When** user interacts with the UI (moving windows, scrolling), **Then** the application responds smoothly without freezing
3. **Given** shell export completes, **When** checking the output file, **Then** it contains the expected shell meshes with correct material colors

---

### User Story 2 - Export Cancellation (Priority: P2)

As a user who started a shell export by mistake or with wrong settings, I want to cancel the export in progress, so that I don't have to wait for a potentially long operation to complete.

**Why this priority**: Cancellation is essential for long operations but depends on the async infrastructure from P1.

**Independent Test**: Start a shell export and click Cancel, verify the export stops promptly and the UI returns to the pre-export state.

**Acceptance Scenarios**:

1. **Given** shell export is in progress, **When** user clicks Cancel, **Then** the export stops within 2 seconds
2. **Given** export was cancelled, **When** checking the file system, **Then** no partial/corrupted output file exists
3. **Given** export was cancelled, **When** user starts a new export, **Then** the new export proceeds normally

---

### User Story 3 - UI Lock During Export (Priority: P3)

As a user, I want destructive operations (opening new files, modifying the model) to be blocked during shell export, so that I don't accidentally corrupt the export or lose work.

**Why this priority**: This prevents data corruption but is less critical than basic async functionality.

**Independent Test**: Verify that model editing, node changes, and file operations are disabled during shell export.

**Acceptance Scenarios**:

1. **Given** shell export is in progress, **When** user attempts to open a new file, **Then** the Open menu/button is disabled
2. **Given** shell export is in progress, **When** user attempts to modify the node graph, **Then** modifications are blocked
3. **Given** shell export completes, **When** user attempts to modify the model, **Then** editing is re-enabled

---

### Edge Cases

- What happens if the export fails mid-way (e.g., disk full)? → Progress should stop, error message displayed, UI unlocked
- What happens if user closes the application during export? → Export should be cancelled gracefully, no partial files left
- What happens if there are many shells (>10 layers)? → Progress should update for each shell, not just at start/end
- What happens if shell generation produces an empty mesh for one layer? → Export should skip empty shells with a warning, continue with remaining shells

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Shell export MUST run asynchronously without blocking the main UI thread
- **FR-002**: System MUST display a progress bar that updates during shell generation
- **FR-003**: Progress reporting MUST indicate per-shell progress (e.g., "Shell 2/5 generating...")
- **FR-004**: System MUST support cancellation of shell export via the existing Cancel button
- **FR-005**: Cancellation MUST clean up partial output files to prevent corrupted exports
- **FR-006**: System MUST block model modifications via ExportState during shell export
- **FR-007**: System MUST report errors clearly if shell generation or file writing fails
- **FR-008**: Each shell mesh MUST be watertight (reusing the watertight Manifold Dual Contouring algorithm)
- **FR-009**: Shell export MUST use the existing CancellationToken mechanism for cooperative cancellation
- **FR-010**: Export completion MUST notify ExportState to re-enable UI editing

### Key Entities

- **ShellExporter**: Encapsulates the async shell export operation, implements IExporter interface, tracks progress across multiple shell generations
- **ExportState**: Existing entity that tracks export-in-progress status for UI blocking (already implemented)
- **CancellationToken**: Existing cooperative cancellation mechanism (already implemented)
- **ShellMesh**: Result of shell generation containing vertices, indices, layer index, and filament name

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: UI remains responsive during shell export (no frame drops below 30 FPS)
- **SC-002**: Progress bar updates at least once per second during export
- **SC-003**: Cancel takes effect within 2 seconds of user clicking Cancel
- **SC-004**: Export produces identical output files whether run sync or async (output determinism)
- **SC-005**: No memory leaks or resource leaks after export completion or cancellation
- **SC-006**: Error messages are shown within 1 second of failure detection

## Assumptions

- The existing ManifoldDualContouringStlExporter provides a proven pattern for async export with IExporter interface
- ShellGenerator already produces watertight meshes via Manifold Dual Contouring
- ExportState and CancellationToken infrastructure is already in place and working
- Progress granularity at per-shell level is sufficient (no need for sub-shell progress)
