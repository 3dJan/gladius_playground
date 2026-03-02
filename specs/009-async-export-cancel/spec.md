# Feature Specification: Async Export Cancellation

**Feature Branch**: `009-async-export-cancel`  
**Created**: 2025-01-20  
**Status**: Draft  
**Input**: Currently cancelling the export blocks the main thread for a long time. Clicking on cancel should give the user instant feedback that the export is being cancelled, without blocking the main thread. The export should be aborted as soon as possible after a cancel request.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Instant Cancel Feedback (Priority: P1)

As a user who has started a mesh export and changed my mind, I want to click the Cancel button and immediately see that my cancellation request was acknowledged, so I know the system is responding to my input.

**Why this priority**: This is the core user experience issue - currently the UI freezes when Cancel is clicked, making users think the application has crashed. Immediate feedback is essential for user confidence.

**Independent Test**: Can be tested by clicking Cancel during any export and verifying the UI remains responsive and shows "Cancelling..." status within one frame.

**Acceptance Scenarios**:

1. **Given** an export is in progress, **When** the user clicks Cancel, **Then** the Cancel button immediately changes to "Cancelling..." and becomes disabled.
2. **Given** an export is in progress, **When** the user clicks Cancel, **Then** the progress message updates to indicate cancellation is in progress.
3. **Given** the user has clicked Cancel, **When** they observe the UI, **Then** all other UI elements remain responsive (no freeze/hang).

---

### User Story 2 - Fast Export Abort (Priority: P1)

As a user who has cancelled an export, I want the export operation to stop as quickly as possible, so I can start a new export or continue with other work without waiting.

**Why this priority**: The user explicitly decided to stop the operation. Continuing to waste time and resources on an unwanted export frustrates users and wastes compute resources.

**Independent Test**: Can be tested by starting a long-running export (large mesh, high resolution), cancelling it, and measuring time until export fully stops.

**Acceptance Scenarios**:

1. **Given** an export is in progress, **When** the user cancels, **Then** the export stops within 2 seconds regardless of mesh size or export format.
2. **Given** a cancellation is requested, **When** the export worker checks for cancellation, **Then** it stops processing at the next safe checkpoint.
3. **Given** an export has been cancelled, **When** the cancellation completes, **Then** the export dialog closes or returns to idle state.

---

### User Story 3 - Non-Blocking Main Thread (Priority: P1)

As a user, I want the application to remain fully usable while the export is being cancelled, so I can continue previewing my model or adjusting settings.

**Why this priority**: Application freezes are perceived as crashes and severely damage user trust. Maintaining responsiveness during all operations is critical for professional software.

**Independent Test**: Can be tested by cancelling an export and immediately attempting to rotate/zoom the 3D viewport - viewport must respond smoothly.

**Acceptance Scenarios**:

1. **Given** a cancellation has been requested, **When** cleanup is in progress, **Then** the main application window repaints at normal frame rate.
2. **Given** a cancellation has been requested, **When** cleanup is in progress, **Then** the user can interact with other UI elements normally.
3. **Given** a cancellation has been requested, **When** the user attempts viewport interaction, **Then** the viewport responds without delay.

---

### User Story 4 - Clean State After Cancel (Priority: P2)

As a user who cancelled an export, I want the application to clean up properly and be ready for my next action, without leaving behind partial files or corrupted state.

**Why this priority**: Important for data integrity and user confidence, but slightly lower priority than the responsiveness aspects since partial file cleanup can be handled with reasonable defaults.

**Independent Test**: Can be tested by cancelling an export, then immediately starting a new export - both should succeed without errors.

**Acceptance Scenarios**:

1. **Given** an export was cancelled, **When** cancellation completes, **Then** any partially written export files are deleted.
2. **Given** an export was cancelled, **When** the user starts a new export, **Then** the new export works correctly without errors.
3. **Given** an export was cancelled, **When** the export dialog closes, **Then** internal export state is fully reset.

---

### Edge Cases

- What happens when the user clicks Cancel multiple times rapidly?
  - Only the first click is processed; subsequent clicks are ignored.
- What happens if cancellation is requested during file finalization?
  - The system attempts to stop as quickly as possible but may need to complete atomic write operations to avoid file corruption.
- What happens if the export completes between the Cancel click and the cancellation signal being processed?
  - The export is treated as successful since it completed before cancellation took effect.
- What happens if cancellation fails (e.g., file deletion fails)?
  - The user is notified of cleanup issues but the UI returns to normal state.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST acknowledge a cancel request within one UI frame (typically 16ms at 60fps).
- **FR-002**: System MUST update the Cancel button to show "Cancelling..." state immediately upon click.
- **FR-003**: System MUST NOT block the main UI thread while cancellation cleanup is in progress.
- **FR-004**: System MUST abort the running export operation within 2 seconds of a cancel request.
- **FR-005**: System MUST delete any partially written export files after cancellation.
- **FR-006**: System MUST reset internal export state to allow a new export to be started immediately after cancellation.
- **FR-007**: System MUST prevent multiple simultaneous cancellation requests from causing issues.
- **FR-008**: System MUST provide visual feedback indicating when cancellation is complete.

### Non-Functional Requirements

- **NFR-001**: Cancellation feedback MUST appear without any perceptible delay to the user.
- **NFR-002**: The viewport and other UI elements MUST remain responsive during the entire cancellation process.

### Key Entities

- **CancellationToken**: A thread-safe signal that communicates cancellation requests from the UI to the export worker. Can be checked at multiple points during export processing.
- **ExportState**: Extended to include a "Cancelling" state distinct from "Exporting" and "Idle" to track the cancellation lifecycle.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Cancel button state change occurs within 16ms of user click (measured from button click to visual update).
- **SC-002**: Export abort completes within 2 seconds for exports of any size, measured from cancel click to dialog close/idle state.
- **SC-003**: Main UI maintains 30+ fps during the entire cancellation process (no frame drops below 33ms).
- **SC-004**: 100% of cancelled exports leave no orphaned partial files on disk.
- **SC-005**: Users can successfully start a new export immediately after cancelling a previous export, with 0% failure rate.

## Assumptions

- The export worker runs on a separate thread and can check for cancellation signals at regular intervals.
- Export operations have natural checkpoints where cancellation can be checked (e.g., between slice processing, between voxel grid operations).
- The existing ExportState infrastructure can be extended to support a "Cancelling" state.
- File deletion operations are non-blocking or can be performed asynchronously.
