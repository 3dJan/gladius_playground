# Data Model: Welcome Screen Click Handling State Machine

**Feature**: 012-welcome-file-load  
**Date**: January 24, 2026

## Overview

This document describes the state machine for handling thumbnail clicks in the welcome screen. The design ensures that a clicked file path is reliably stored and processed.

## State Diagram

```
                    ┌─────────────┐
                    │   Visible   │
                    │ (Idle)      │
                    └──────┬──────┘
                           │
                    User clicks thumbnail
                           │
                           ▼
                    ┌─────────────┐
           ┌───────│  Validate   │───────┐
           │       │   Click     │       │
           │       └─────────────┘       │
           │                             │
     Click rejected              Click accepted
     (already processing         (path stored)
      or path exists)                    │
           │                             │
           ▼                             ▼
    ┌─────────────┐              ┌─────────────┐
    │   Visible   │              │   Hidden    │
    │ (Log warn)  │              │ (Pending)   │
    └─────────────┘              └──────┬──────┘
                                        │
                                 Next frame
                                        │
                                        ▼
                                 ┌─────────────┐
                                 │ MainWindow  │
                                 │ processes   │
                                 └──────┬──────┘
                                        │
                              processFileOpen() called
                                        │
                                        ▼
                                 ┌─────────────┐
                                 │ File loads  │
                                 │ async       │
                                 └─────────────┘
```

## Entities

### PendingFileRequest (Conceptual)

Represents the user's intent to open a specific file from the welcome screen.

| Field | Type | Description |
|-------|------|-------------|
| path | std::filesystem::path | Absolute path to the file to open |
| timestamp | (implicit) | When the click occurred (frame number) |

**Implemented as**: `std::optional<std::filesystem::path> m_pendingFileOpen`

**Invariants**:
- If `m_pendingFileOpen.has_value()` is true, the welcome screen MUST be hidden (`m_isVisible == false`)
- Once set, `m_pendingFileOpen` is only cleared by `processFileOpen()` or explicit reset
- Path MUST be validated to exist before storing

### ClickProcessingState (Conceptual)

Tracks whether a click has been processed in the current frame to prevent duplicate handling.

| Field | Type | Description |
|-------|------|-------------|
| processed | bool | True if a click was handled this frame |

**Implemented as**: `bool m_clickProcessed`

**Invariants**:
- Reset to `false` at start of each `render()` call
- Set to `true` when any click is processed (success or failure)

## State Transitions

### Transition: Idle → Pending

**Trigger**: User clicks thumbnail and `trySetPendingFileOpen()` succeeds

**Guard conditions** (ALL must be true):
- `m_isVisible == true` (screen is showing)
- `m_clickProcessed == false` (no click this frame yet)
- `m_pendingFileOpen.has_value() == false` (no pending file)
- `std::filesystem::exists(path) == true` (file exists)

**Actions**:
1. Store path: `m_pendingFileOpen = path`
2. Mark processed: `m_clickProcessed = true`
3. Hide screen: `m_isVisible = false`
4. Log: "File selected for opening: {path}"

### Transition: Idle → Idle (Rejected)

**Trigger**: User clicks thumbnail but `trySetPendingFileOpen()` fails

**Guard conditions** (ANY can trigger):
- `m_clickProcessed == true` (already processed a click)
- `m_pendingFileOpen.has_value() == true` (already have pending file)
- `std::filesystem::exists(path) == false` (file doesn't exist)

**Actions**:
1. Mark processed: `m_clickProcessed = true`
2. Log warning with rejection reason
3. **Do NOT hide screen** (allow user to try again)
4. If file doesn't exist: Show error message to user

### Transition: Pending → FileLoading

**Trigger**: `processFileOpen()` called in MainWindow after screen closed

**Guard conditions**:
- `m_pendingFileOpen.has_value() == true`

**Actions**:
1. Extract path: `auto path = m_pendingFileOpen.value()`
2. Clear pending: `m_pendingFileOpen.reset()`
3. Return path to caller
4. Caller initiates async file load

## Validation Rules

### Path Validation

Before storing in `m_pendingFileOpen`:
1. Path must not be empty
2. Path must exist on filesystem
3. Path must be a regular file (not directory)

### Click Validation

Before processing any click:
1. Must be in visible state
2. Must not have already processed a click this frame
3. Must not already have a pending file

## Error Handling

| Scenario | Response |
|----------|----------|
| File doesn't exist | Show error message, keep screen visible |
| Click already processed | Log warning, keep screen visible |
| Pending file already set | Log warning, keep screen visible |
| processFileOpen returns empty after close | Log warning in MainWindow |
