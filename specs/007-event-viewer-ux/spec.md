# Feature Specification: Event Viewer UX Improvements

**Feature Branch**: `007-event-viewer-ux`  
**Created**: 2026-01-04  
**Status**: Draft  
**Input**: User description: "Improve the event viewer: by default only warnings and errors should be shown. It should be possible to easily copy a set of events/warnings/error messages to the clipboard."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Default Severity Filter (Priority: P1)

As a user opening the event viewer, I want to see only warnings and errors by default, so that I can quickly focus on actionable items without being distracted by informational messages.

**Why this priority**: The event viewer's primary purpose is to alert users to problems. Info messages are typically debug/progress information that most users don't need. Showing only warnings and errors by default makes the tool immediately useful and reduces noise.

**Independent Test**: Open the application, trigger some events of different severities (info, warning, error), open the event viewer, and verify that only warnings and errors are displayed by default.

**Acceptance Scenarios**:

1. **Given** a fresh application launch with events logged at Info, Warning, and Error levels, **When** the user opens the Event Viewer, **Then** only Warning, Error, and Fatal Error events are displayed (Info is hidden by default)
2. **Given** the Event Viewer is open with default filters, **When** the user enables the Info checkbox, **Then** informational messages become visible
3. **Given** the user has modified filter settings during a session, **When** the user closes and reopens the Event Viewer (within the same application session), **Then** the filter settings persist as the user configured them

---

### User Story 2 - Copy Individual Event to Clipboard (Priority: P2)

As a user reviewing the event log, I want to copy a single event message to my clipboard, so that I can share it in bug reports, chat messages, or documentation.

**Why this priority**: Copying individual events is the most common use case - users typically need to share a specific error they encountered. This provides immediate value for troubleshooting workflows.

**Independent Test**: Right-click on any event in the expanded view and select "Copy" to copy that single event's details to the clipboard.

**Acceptance Scenarios**:

1. **Given** the Event Viewer is open in expanded view, **When** the user right-clicks on an event, **Then** a context menu appears with a "Copy" option
2. **Given** the context menu is open, **When** the user selects "Copy", **Then** the event's timestamp, severity, and message are copied to the system clipboard as formatted text
3. **Given** the Event Viewer is open in expanded view, **When** the user selects an event and presses Ctrl+C (Cmd+C on macOS), **Then** the event is copied to clipboard

---

### User Story 3 - Copy All Visible Events to Clipboard (Priority: P2)

As a user troubleshooting an issue, I want to copy all currently visible events to my clipboard at once, so that I can quickly share a complete log with support or include it in a bug report.

**Why this priority**: When reporting bugs or seeking help, users often need to share multiple related events. A one-click solution to copy all filtered/visible events saves significant time.

**Independent Test**: Click a "Copy All" button in the expanded view toolbar, and verify all currently visible events are copied to clipboard.

**Acceptance Scenarios**:

1. **Given** the Event Viewer is open with events displayed (filtered or unfiltered), **When** the user clicks the "Copy All" button, **Then** all visible events are copied to the system clipboard
2. **Given** a text filter is active showing a subset of events, **When** the user clicks "Copy All", **Then** only the filtered (visible) events are copied
3. **Given** severity filters hide some events, **When** the user clicks "Copy All", **Then** only events matching the current severity filter are copied
4. **Given** events are copied to clipboard, **Then** the format includes timestamp, severity, and message for each event, with clear separation between events

---

### User Story 4 - Copy Events by Severity from Collapsed View (Priority: P3)

As a user viewing the collapsed event summary, I want to copy all events of a specific severity (e.g., all errors) with one click, so that I can quickly gather related issues for a bug report.

**Why this priority**: The collapsed view already groups events by severity. Allowing copy from the collapsed view tooltips provides a convenient shortcut for users who know they only need errors or only warnings.

**Independent Test**: In collapsed view, hover over the "Errors" count to show the tooltip, then click a "Copy" button within the tooltip to copy all error messages.

**Acceptance Scenarios**:

1. **Given** the Event Viewer is in collapsed mode showing error count, **When** the user hovers over the error count, **Then** a tooltip shows all error messages with a "Copy All Errors" button
2. **Given** the tooltip with error messages is visible, **When** the user clicks "Copy All Errors", **Then** all error messages are copied to clipboard
3. **Given** the same pattern applies for warnings and fatal errors, **When** hovering and clicking the respective copy buttons, **Then** the corresponding events are copied

---

### Edge Cases

- What happens when the user tries to copy with no events visible? → Show a brief notification that there are no events to copy
- What happens when clipboard access fails (permissions/system issue)? → Show a user-friendly error message indicating copy failed
- How are multi-line event messages handled in the copied output? → Preserve line breaks within messages, use clear separators between events
- What timestamp format is used in copied text? → Use ISO 8601 format (YYYY-MM-DD HH:MM:SS) for unambiguous parsing

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST default to showing only Warning, Error, and Fatal Error events when the Event Viewer is first opened in a session
- **FR-002**: System MUST allow users to enable Info-level messages via checkbox, which updates the display immediately
- **FR-003**: System MUST preserve user's severity filter preferences within a single application session
- **FR-004**: System MUST provide a right-click context menu on event rows in expanded view with a "Copy" option
- **FR-005**: System MUST support keyboard shortcut (Ctrl+C / Cmd+C) to copy selected event
- **FR-006**: System MUST provide a "Copy All" button in the expanded view toolbar
- **FR-007**: The "Copy All" function MUST respect current text filter and severity filters
- **FR-008**: Copied event text MUST include timestamp (ISO 8601), severity level, and full message
- **FR-009**: System MUST provide visual feedback (tooltip showing "Copied!") when copy operation succeeds
- **FR-010**: System MUST provide copy buttons within severity tooltips in collapsed view

### Key Entities

- **Event**: Represents a logged event with timestamp, severity level (Info, Warning, Error, FatalError), and message content
- **Clipboard Format**: Plain text with each event on separate lines, format: `[YYYY-MM-DD HH:MM:SS] [SEVERITY] Message`

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can copy any visible event to clipboard within 2 seconds (one right-click + one click, or one keyboard shortcut)
- **SC-002**: Users can copy all visible events to clipboard with a single click
- **SC-003**: Event Viewer displays only actionable events (warnings, errors) by default, reducing visible noise for typical users
- **SC-004**: 100% of copy operations produce correctly formatted output that can be pasted into any text editor

## Assumptions

- The application uses ImGui for UI rendering, which has clipboard support via `ImGui::SetClipboardText()`
- Users on all supported platforms (Windows, Linux, macOS) have access to system clipboard functionality
- The current filter behavior (text filter) should remain unchanged; this feature adds severity defaults and clipboard functionality
- Filter preference persistence is session-only (not saved to disk between application restarts)
