# Data Model: Event Viewer UX Improvements

**Feature**: 007-event-viewer-ux  
**Date**: 2026-01-04

## Existing Entities (No Changes Required)

### Event (`gladius::events::Event`)

The Event class already contains all necessary data. No modifications needed.

| Field | Type | Description |
|-------|------|-------------|
| `m_timestamp` | `std::chrono::system_clock::time_point` | When the event occurred |
| `m_msg` | `std::string` | Event message content |
| `m_severity` | `Severity` | Info, Warning, Error, or FatalError |

### Severity Enum (`gladius::events::Severity`)

```cpp
enum class Severity
{
    Info,
    Warning,
    Error,
    FatalError
};
```

## New/Modified Types

### LogView State (Modified)

The `LogView` class in `gladius::ui` needs these state changes:

| Field | Current Default | New Default | Rationale |
|-------|-----------------|-------------|-----------|
| `m_showInfo` | `true` | `false` | FR-001: Hide info by default |
| `m_showWarnings` | `true` | `true` (unchanged) | Show warnings by default |
| `m_showErrors` | `true` | `true` (unchanged) | Show errors by default |
| `m_showFatal` | `true` | `true` (unchanged) | Show fatal by default |

### New Private Members (LogView)

```cpp
// Optional: Track selected event for keyboard copy (can be -1 for none)
int m_selectedEventIndex = -1;

// Copy feedback state
bool m_showCopyFeedback = false;
std::chrono::steady_clock::time_point m_copyFeedbackTime;
```

## Clipboard Text Format

### Single Event Format

```
[YYYY-MM-DD HH:MM:SS] [SEVERITY] Message text
```

**Examples**:
```
[2026-01-04 14:30:45] [WARNING] OpenCL kernel compilation failed for device X
[2026-01-04 14:30:46] [ERROR] Failed to load resource: texture.png
[2026-01-04 14:30:47] [FATAL] Out of GPU memory
```

### Multiple Events Format

Events are separated by newlines. No header or footer.

```
[2026-01-04 14:30:45] [INFO] Loading model...
[2026-01-04 14:30:46] [WARNING] Model has non-manifold edges
[2026-01-04 14:30:47] [ERROR] Export failed: insufficient permissions
```

### Severity Labels

| Enum Value | Clipboard Label |
|------------|-----------------|
| `Severity::Info` | `INFO` |
| `Severity::Warning` | `WARNING` |
| `Severity::Error` | `ERROR` |
| `Severity::FatalError` | `FATAL` |

## Data Flow

```
User Action                    Data Transformation              Output
───────────────────────────────────────────────────────────────────────
Right-click event    →   formatEventForClipboard(event)   →   Clipboard
Ctrl+C on selected   →   formatEventForClipboard(event)   →   Clipboard
"Copy All" button    →   formatEventsForClipboard(range)  →   Clipboard
Tooltip copy button  →   formatEventsBySeverity(sev)      →   Clipboard
```

## Filter State Persistence

| Scope | Persistence | Storage |
|-------|-------------|---------|
| Within session | ✅ Persisted | `LogView` member variables |
| Across sessions | ❌ Not persisted | N/A (always reset to defaults on app launch) |

This is intentional per FR-003: Filter preferences persist within a session but reset on restart.
