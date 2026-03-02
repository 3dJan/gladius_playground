# Quickstart: Testing Welcome Screen File Loading Fix

**Feature**: 012-welcome-file-load  
**Date**: January 24, 2026

## Overview

This guide explains how to test the welcome screen thumbnail click fix.

## Prerequisites

- Build the project using "Build ALL (linux-releaseWithDebug)" task
- Have some .3mf files in your recent files or examples directory

## Manual Testing

### Test 1: Basic Thumbnail Click

1. Launch Gladius
2. Welcome screen appears
3. Click any thumbnail in Recent Files or Examples tab
4. **Expected**: That specific file loads (not the default template)
5. **Verify**: Check the title bar shows the correct filename

### Test 2: Rapid Double-Click

1. Launch Gladius
2. Welcome screen appears
3. Rapidly double-click a thumbnail
4. **Expected**: File loads exactly once, no errors
5. **Verify**: No duplicate load messages in Event Log

### Test 3: Click Different Thumbnails Rapidly

1. Launch Gladius
2. Welcome screen appears
3. Click thumbnail A, then immediately click thumbnail B
4. **Expected**: File A loads (first click wins)
5. **Verify**: Check title bar shows A's filename

### Test 4: Deleted File Error

1. Add a file to recent files
2. Delete that file from filesystem
3. Launch Gladius
4. Click the deleted file's thumbnail
5. **Expected**: Error message appears, screen stays visible
6. **NOT expected**: Default template loads silently

## Unit Tests

Run the welcome screen unit tests:

```bash
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=WelcomeScreen*
```

### Expected Test Cases

| Test | Description |
|------|-------------|
| `WelcomeScreen_TrySetPendingFileOpen_WithValidPath_StoresPath` | Basic click stores path |
| `WelcomeScreen_TrySetPendingFileOpen_WhenAlreadyProcessed_RejectsAndStaysVisible` | Double-click handling |
| `WelcomeScreen_TrySetPendingFileOpen_WhenPendingExists_RejectsAndStaysVisible` | Rapid different clicks |
| `WelcomeScreen_ProcessFileOpen_AfterClick_ReturnsStoredPath` | Path retrieval works |
| `WelcomeScreen_ProcessFileOpen_WithoutClick_ReturnsEmpty` | No-click case handled |

## Debugging

If the bug still occurs, enable logging and check for:

```
[WARN] Welcome screen click rejected: <reason>
[WARN] Welcome screen closed but no pending file found
```

These messages indicate the guard conditions are triggering.

## Success Criteria

- [ ] 100% of single thumbnail clicks load the correct file
- [ ] Double-clicks result in exactly one file load
- [ ] Deleted files show error instead of loading template
- [ ] All unit tests pass
