# Feature Specification: Export UI Lock

**Feature Branch**: `008-export-ui-lock`  
**Created**: 2025-01-06  
**Status**: Draft  
**Input**: User description: "During export the UI should be locked with a blocking overlay to prevent model alterations while export is in progress"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Model Protection During Export (Priority: P1)

As a user exporting a mesh, I want the application to prevent any model modifications during export so that the exported mesh accurately represents the model state when I initiated the export.

**Why this priority**: This is the core requirement - without protection, users could inadvertently corrupt exports or cause application crashes by modifying the model while GPU operations are in progress.

**Independent Test**: Start an MDC export on a complex model, attempt to modify node parameters via the node editor - modifications should be blocked until export completes.

**Acceptance Scenarios**:

1. **Given** an export is in progress, **When** user attempts to drag a parameter slider in the node editor, **Then** the input is ignored and the parameter value does not change
2. **Given** an export is in progress, **When** user attempts to create or delete a node, **Then** the action is blocked
3. **Given** an export is in progress, **When** user attempts to create or delete a link between nodes, **Then** the action is blocked
4. **Given** no export is in progress, **When** user modifies any node parameter, **Then** the modification succeeds normally

---

### User Story 2 - Visual Feedback During Export Lock (Priority: P1)

As a user, I want a clear visual indication that the model editor is locked during export so that I understand why my inputs are not being accepted.

**Why this priority**: Without visual feedback, users would think the application is broken when their inputs are ignored.

**Independent Test**: Start an export and observe that a semi-transparent overlay appears over the model editor area with a message indicating export is in progress.

**Acceptance Scenarios**:

1. **Given** an export is in progress, **When** user views the model editor, **Then** a semi-transparent dark overlay is displayed over the editor area
2. **Given** an export is in progress, **When** user views the overlay, **Then** a message indicating "Export in progress..." is displayed
3. **Given** an export completes (success or failure), **When** user views the model editor, **Then** the overlay is removed and normal interaction resumes

---

### User Story 3 - Block File Operations During Export (Priority: P1)

As a user, I want the application to prevent loading a new file or creating a new model during export so that I don't lose my export progress or corrupt the export operation.

**Why this priority**: Loading a new file during export would invalidate the ComputeCore being used by the export thread, causing undefined behavior or crashes.

**Independent Test**: Start an export, attempt to use File > Open or File > New - these actions should be blocked.

**Acceptance Scenarios**:

1. **Given** an export is in progress, **When** user attempts to open a new file via menu, **Then** the action is blocked (menu item disabled or shows warning)
2. **Given** an export is in progress, **When** user attempts to create a new model via menu, **Then** the action is blocked
3. **Given** an export is in progress, **When** user attempts to import functions, **Then** the action is blocked
4. **Given** an export is in progress, **When** user attempts to close the application, **Then** a confirmation dialog warns about the in-progress export

---

### User Story 4 - Format-Agnostic Protection (Priority: P2)

As a user exporting meshes in any format (STL, 3MF) using any extraction method (MDC, HDC, DC, LMC), I want the same UI protection to apply regardless of export configuration.

**Why this priority**: The protection mechanism should be universal, not tied to a specific exporter implementation.

**Independent Test**: Start exports using different combinations of file format and extraction method, verify overlay appears in all cases.

**Acceptance Scenarios**:

1. **Given** user initiates STL export with MDC, **When** export begins, **Then** UI lock is applied
2. **Given** user initiates 3MF export with HDC, **When** export begins, **Then** UI lock is applied
3. **Given** user initiates export via MCP API, **When** export begins, **Then** UI lock is applied

---

### Edge Cases

- What happens when multiple exports are queued (if supported)? Lock should remain until all complete.
- What happens if export fails mid-operation? Lock should be released on failure.
- What happens if user force-closes the application during export? Graceful handling, no file corruption.
- What happens to keyboard shortcuts during export? Model-modifying shortcuts (Ctrl+Z, Delete) should be blocked.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST block all model-modifying interactions in the node editor during export (parameter changes, node creation/deletion, link creation/deletion)
- **FR-002**: System MUST display a semi-transparent dark overlay over the model editor area during export
- **FR-003**: System MUST display a status message on the overlay indicating export is in progress
- **FR-004**: System MUST disable file operations (New, Open, Import) during export
- **FR-005**: System MUST release the UI lock immediately when export completes (success or failure)
- **FR-006**: System MUST apply UI lock for all export types (STL, 3MF) and all mesh extraction methods (MDC, HDC, DC, LMC)
- **FR-007**: System MUST NOT block the main thread - UI must remain responsive (progress bar updates, cancel button works)
- **FR-008**: System MUST continue to allow viewport interaction (rotation, zoom, pan) during export

### Non-Functional Requirements

- **NFR-001**: Overlay rendering must not impact export performance (< 0.1ms per frame)
- **NFR-002**: Lock state must be thread-safe (export runs on background thread)

### Key Entities

- **ExportState**: Existing class that tracks export progress; will be extended to provide overlay rendering trigger
- **ModelEditor**: Primary UI component that needs overlay integration
- **NodeView**: Component handling parameter editing; must respect export lock
- **MainWindow**: Owns ExportState; coordinates lock across all views

## Assumptions

- The existing `ExportState` class already provides `isExportInProgress()` which is checked by some components
- The `ModelEditor` already has access to `ExportState` via `setExportState()`
- Menu items in `MainWindow` already have partial blocking via `ImGui::BeginDisabled()` when export is in progress
- Current gap: `NodeView` and parameter editing do not check export state

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of model-modifying UI interactions are blocked during export (measured via manual testing of all interaction types)
- **SC-002**: Overlay appears within 1 frame of export beginning (< 16ms latency)
- **SC-003**: Overlay is removed within 1 frame of export completing
- **SC-004**: Export performance is not measurably impacted by overlay rendering (< 1% overhead)
- **SC-005**: Zero crashes or undefined behavior when user attempts blocked operations during export
