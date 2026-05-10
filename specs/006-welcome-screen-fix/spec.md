# Feature Specification: Welcome Screen Improvements

**Feature Branch**: `006-welcome-screen-fix`  
**Created**: January 3, 2026  
**Status**: Draft  
**Input**: User description: "Improve the welcome screen: 1) Async and progressive thumbnail loading, 2) Fix timing issue when clicking thumbnails that causes default template to load instead of selected file, 3) Fix ImGui docking layout persistence after closing welcome screen"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Responsive Welcome Screen with Progressive Thumbnails (Priority: P1)

As a user, when I launch Gladius and the welcome screen appears, I want to see the UI respond immediately without freezing, even when there are many recent files. Thumbnails should load progressively in the background so I can start navigating immediately.

**Why this priority**: A frozen UI on startup creates a poor first impression and can make users think the application has crashed. This directly impacts perceived application quality and user retention.

**Independent Test**: Launch Gladius with 20+ recent files and verify the welcome screen appears immediately (< 100ms), with thumbnails loading progressively over time without any UI freezing.

**Acceptance Scenarios**:

1. **Given** a recent files list with 20 files, **When** the welcome screen is displayed, **Then** the UI should be responsive immediately and thumbnails should appear one by one without blocking user interaction.
2. **Given** a recent files list with files that have thumbnails, **When** thumbnails are loading, **Then** placeholder icons should be shown for files whose thumbnails are not yet loaded.
3. **Given** a recent files list, **When** thumbnails are loading in the background, **Then** the user should be able to scroll, click, and interact with the welcome screen without delays.

---

### User Story 2 - Reliable File Selection from Thumbnails (Priority: P1)

As a user, when I click on a recent file thumbnail in the welcome screen, I want the selected file to actually open, not the default template.

**Why this priority**: This is a bug that directly prevents core functionality from working. Users clicking on thumbnails expect their selected file to open, not a blank template. This creates confusion and data loss concerns.

**Independent Test**: Click on any recent file thumbnail in the welcome screen and verify the correct file is loaded with its model data visible in the viewport.

**Acceptance Scenarios**:

1. **Given** a recent files list with existing 3MF files, **When** I click on a thumbnail, **Then** the corresponding 3MF file should be loaded and displayed.
2. **Given** a recent files list, **When** I click on a thumbnail while thumbnails are still loading in the background, **Then** the clicked file should still load correctly.
3. **Given** a recent files list with a file that has a valid thumbnail, **When** I click on the thumbnail image itself, **Then** the file should open correctly.
4. **Given** a recent files list with a file that has no thumbnail (placeholder shown), **When** I click on the placeholder, **Then** the file should open correctly.

---

### User Story 3 - Preserved Window Layout After Closing Welcome Screen (Priority: P2)

As a user, when I close the welcome screen (either by clicking outside, pressing Escape, or loading a file), I want my ImGui window docking layout to remain intact from my previous session.

**Why this priority**: Layout persistence is a quality-of-life feature that prevents users from having to repeatedly rearrange their workspace. While annoying, this doesn't block core functionality.

**Independent Test**: Customize the ImGui window layout (move/dock panels), close the application, reopen it, close the welcome screen, and verify the custom layout is restored.

**Acceptance Scenarios**:

1. **Given** a custom ImGui docking layout saved from a previous session, **When** I close the welcome screen by loading a recent file, **Then** my custom layout should be preserved.
2. **Given** a custom ImGui docking layout, **When** I close the welcome screen by clicking "New Project", **Then** my custom layout should be preserved.
3. **Given** a custom ImGui docking layout, **When** I close the welcome screen by clicking the X button or pressing Escape, **Then** my custom layout should be preserved.
4. **Given** no previous layout saved (first-time user), **When** I close the welcome screen, **Then** the default layout should be applied correctly.

---

### Edge Cases

- What happens when a recent file no longer exists on disk when clicked? (Show error message, don't load template)
- What happens when thumbnail extraction fails for a corrupted 3MF file? (Show placeholder, allow file to be clicked)
- What happens when the user rapidly clicks multiple thumbnails? (Only the first click should be processed)
- What happens when the user closes the welcome screen while thumbnails are still loading? (Gracefully stop background loading)
- What happens when the imgui.ini file is corrupted or unreadable? (Fall back to default layout)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST load thumbnails asynchronously in a background thread, not on the main UI thread.
- **FR-002**: System MUST display placeholder icons for files whose thumbnails have not yet been loaded.
- **FR-003**: System MUST progressively update the UI as each thumbnail finishes loading.
- **FR-004**: System MUST ensure the welcome screen remains interactive during thumbnail loading (no UI freezes).
- **FR-005**: System MUST capture the selected file path before initiating the hide operation when a thumbnail is clicked.
- **FR-006**: System MUST ensure the file open operation uses the captured path, not rely on state that may change during the close transition.
- **FR-007**: System MUST preserve the ImGui docking layout when the welcome screen is closed.
- **FR-008**: System MUST NOT reset or modify the docking layout as a side effect of closing the welcome screen.
- **FR-009**: System MUST support cancellation of pending thumbnail loading operations when the welcome screen is closed.
- **FR-010**: System MUST handle the case where a clicked file no longer exists by logging an error via `m_logger->addEvent()` with `Severity::Error` rather than loading the default template.

### Key Entities

- **ThumbnailInfo**: Represents a file's thumbnail state (path, loaded status, texture data, loading state)
- **ThumbnailLoadRequest**: Represents a pending async load operation for a thumbnail
- **WelcomeScreen**: The modal overlay that displays on startup with recent files, examples, and backup options
- **AsyncThumbnailLoader**: Component responsible for background thumbnail loading (new component)

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Welcome screen appears within 100ms of launch, regardless of the number of recent files (up to 100). Measured from application start to first frame with welcome screen visible.
- **SC-002**: No frame during welcome screen display exceeds 16ms (maintains 60fps responsiveness).
- **SC-003**: 100% of thumbnail clicks result in the correct file being loaded (not the default template).
- **SC-004**: ImGui docking layout is preserved in 100% of welcome screen close scenarios.
- **SC-005**: No UI freezes longer than 16ms (one frame at 60fps) during thumbnail loading.
- **SC-006**: Thumbnail loading completes for all visible items within 2 seconds for a list of 20 files.

## Assumptions

- The lib3mf library's thumbnail extraction API is thread-safe for read-only operations on separate file handles.
- OpenGL texture creation must happen on the main thread; only the file I/O and PNG decoding should be offloaded.
- The current imgui.ini mechanism is sufficient for layout persistence; the issue is likely with when/how layout is being reset.
- The timing issue with file selection is likely a race condition between hiding the welcome screen and processing the file open callback.
