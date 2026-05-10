# Research: Welcome Screen File Loading Race Condition

**Feature**: 012-welcome-file-load  
**Date**: January 24, 2026

## Research Summary

This document consolidates the root cause analysis for the welcome screen timing bug where clicking a thumbnail sometimes loads the default template instead of the clicked file.

## Decision: Root Cause Identification

**Decision**: The bug is caused by `trySetPendingFileOpen()` silently rejecting clicks when `m_clickProcessed` is already true or `m_pendingFileOpen` already has a value, while the welcome screen visibility is controlled independently.

**Rationale**: 
- Code analysis shows `m_clickProcessed` is reset per-frame in `render()`, but multiple click handlers can fire within the same frame
- The `m_pendingFileOpen` guard prevents overwriting, but doesn't prevent the screen from hiding via other paths
- When rejection occurs, no path is stored but the screen may still close, leading to `processFileOpen()` returning empty

**Alternatives considered**:
- Thread synchronization issue: Ruled out - all click handling is on main UI thread
- ImGui double-click detection: Ruled out - ImGui handles this correctly
- Async loading race: Ruled out - async loading starts after path is processed

## Decision: Click Rejection Should Not Hide Screen

**Decision**: When `trySetPendingFileOpen()` rejects a click, the welcome screen visibility state should NOT change.

**Rationale**: 
- Current code has click handlers that call `trySetPendingFileOpen()` and separately call `m_isVisible = false`
- The fix consolidates visibility change into `trySetPendingFileOpen()` only
- If path cannot be stored, screen stays visible, giving user another chance

**Alternatives considered**:
- Queue rejected clicks: Too complex, first-click-wins is simpler and expected behavior
- Show modal error for rejected clicks: Unnecessary UI complexity for rare edge case

## Decision: Add Logging for Rejected Clicks

**Decision**: Log a warning when a click is rejected to aid debugging.

**Rationale**:
- Silent failures are hard to diagnose
- Logging helps identify if the bug recurs in production
- No user-visible impact, just development/support aid

**Alternatives considered**:
- Throw exception: Too disruptive for a non-critical guard condition
- No logging: Makes debugging difficult

## Decision: MainWindow Should Not Fall Back Silently

**Decision**: When welcome screen closes but `processFileOpen()` returns empty, MainWindow should log a warning rather than silently continuing with whatever model happens to be loaded.

**Rationale**:
- Current code does nothing when `processFileOpen()` returns empty after screen close
- This makes the bug appear as "wrong file loaded" when actually "no file loaded"
- Explicit logging clarifies the actual failure mode

**Alternatives considered**:
- Show error dialog: Could be confusing if it happens during legitimate "New Model" click
- Prevent screen close: Not feasible, multiple paths can close the screen

## Technical Findings

### Code Flow Analysis

```
User clicks thumbnail
    → ImGui::Button returns true (click detected)
    → trySetPendingFileOpen(path) called
        → IF m_clickProcessed OR m_pendingFileOpen.has_value():
            → return false (silent rejection)
            → Screen may still hide via other paths
        → ELSE:
            → m_pendingFileOpen = path
            → m_clickProcessed = true
            → m_isVisible = false
            → return true

Next frame (MainWindow::render):
    → welcomeScreenHasbeenClosed detected (was visible, now hidden)
    → processFileOpen() called
        → Returns m_pendingFileOpen (may be empty if rejected)
        → Clears m_pendingFileOpen
    → IF path returned:
        → open(path)
    → ELSE:
        → Nothing happens (user sees whatever was loaded before)
```

### Failure Scenarios

1. **Same-frame double click**: Two ImGui button clicks register in same frame
2. **Rapid different thumbnail clicks**: User clicks A, then B before frame completes
3. **Click during animation**: Screen is fading out, user clicks, state is inconsistent

## Implementation Recommendations

1. **Atomic operation**: Make `trySetPendingFileOpen()` the single authority for path storage AND visibility change
2. **Guard on entry**: Check `m_isVisible` at start - if already hiding, reject immediately
3. **Log rejections**: Add debug logging when click is rejected
4. **MainWindow awareness**: Log when screen closed but no pending file found
