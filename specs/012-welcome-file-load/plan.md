# Implementation Plan: Reliable Async File Loading from Welcome Screen

**Branch**: `012-welcome-file-load` | **Date**: January 24, 2026 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/012-welcome-file-load/spec.md`

**Note**: This is a bug fix. Most infrastructure exists; changes are surgical.

## Summary

Fix race condition where clicking a welcome screen thumbnail sometimes opens the default template instead of the selected file. The root cause is a fragile state machine in `WelcomeScreen::trySetPendingFileOpen()` that can silently reject clicks under certain timing conditions. The fix involves making file path storage atomic and guaranteed, adding explicit error handling, and preventing silent fallback to templates.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (UI), std::filesystem, std::optional  
**Storage**: N/A (in-memory state only)  
**Testing**: GTest/GMock  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Desktop application (single project)  
**Performance Goals**: >30 FPS during file loading, <100ms visual feedback  
**Constraints**: Must not block UI thread during file operations  
**Scale/Scope**: Bug fix - changes to 2-3 source files, ~50-100 lines modified

## Constitution Check

*GATE: All items pass ✅*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ Pass | Using std::optional, std::filesystem, C++20 features |
| II. Test-First Development | ✅ Pass | Will add unit tests for click handling logic |
| III. Simplicity First (KISS) | ✅ Pass | Minimal changes to existing working code |
| IV. Consistent Code Style | ✅ Pass | Following existing WelcomeScreen patterns |
| V. Documentation | ✅ Pass | Will document new state machine behavior |

## Project Structure

### Documentation (this feature)

```text
specs/012-welcome-file-load/
├── spec.md              # Feature specification (done)
├── plan.md              # This file
├── research.md          # Root cause analysis (Phase 0)
├── data-model.md        # State machine documentation (Phase 1)
├── quickstart.md        # Testing guide (Phase 1)
└── tasks.md             # Implementation tasks (Phase 2)
```

### Source Code (files to modify)

```text
gladius/src/ui/
├── WelcomeScreen.h       # Add state enum, modify trySetPendingFileOpen signature
├── WelcomeScreen.cpp     # Fix race condition in click handling
└── MainWindow.cpp        # Add error handling when processFileOpen returns empty

gladius/tests/unittests/
└── WelcomeScreen_tests.cpp  # New: Unit tests for click handling state machine
```

**Structure Decision**: This is a bug fix in existing code. No new directories or major restructuring needed. Changes are localized to WelcomeScreen click handling and MainWindow file open processing.

## Root Cause Analysis

### Current Behavior (Buggy)

```cpp
bool WelcomeScreen::trySetPendingFileOpen(std::filesystem::path const & path)
{
    if (m_clickProcessed || m_pendingFileOpen.has_value())
    {
        return false;  // Silent rejection!
    }
    m_pendingFileOpen = path;
    m_clickProcessed = true;
    m_isVisible = false;
    return true;
}
```

**Problem**: When `trySetPendingFileOpen()` returns `false`:
1. No path is stored
2. Welcome screen still hides (via other code paths)
3. `processFileOpen()` returns empty
4. No file loads → user sees template from startup

### Identified Race Conditions

1. **Double-click in same frame**: `m_clickProcessed` already true
2. **Multiple thumbnails clicked rapidly**: `m_pendingFileOpen` already has value
3. **Click during loading fade-out**: Click may be ignored

### Fix Strategy

1. **Single source of truth**: Store path atomically with visibility change
2. **No silent failures**: Log when click is rejected, explain why
3. **Explicit error path**: If processFileOpen() returns empty after close, show error (not template)
4. **First-click-wins guarantee**: Clear rejection doesn't hide screen

## Implementation Approach

### Phase 1: Fix Core Race Condition

**Change 1**: Modify `trySetPendingFileOpen` to be more robust:
- Add logging when rejecting a click
- Don't hide screen if click is rejected
- Ensure path is stored before any visibility change

**Change 2**: Modify `MainWindow::render()` welcome screen handling:
- Add explicit check: if screen closed but no pending file → log warning
- Don't silently fall through to template

### Phase 2: Add Defensive Measures

**Change 3**: Add file existence validation in `trySetPendingFileOpen`:
- Validate path exists before storing
- Show error immediately if file doesn't exist

**Change 4**: Add unit tests for click handling state machine

## Test Plan

| Test Case | Description | Expected Result |
|-----------|-------------|-----------------|
| SingleClick_StoresPath | Click thumbnail once | Path stored, screen hides |
| DoubleClick_FirstWins | Click same thumbnail twice in one frame | First path stored, second rejected |
| DifferentThumbnails_FirstWins | Click A then B rapidly | A's path stored, B rejected |
| RejectedClick_ScreenStaysVisible | Click rejected | Screen remains visible |
| NonExistentFile_ShowsError | Click thumbnail for deleted file | Error shown, template not loaded |
| ProcessFileOpen_AfterClose_ReturnsPath | Screen closes after click | processFileOpen returns correct path |

## Complexity Tracking

No constitution violations. This is a minimal bug fix.
