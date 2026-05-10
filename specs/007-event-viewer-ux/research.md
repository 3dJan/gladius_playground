# Research: Event Viewer UX Improvements

**Feature**: 007-event-viewer-ux  
**Date**: 2026-01-04

## Executive Summary

This document consolidates research findings for implementing clipboard copy functionality and default severity filtering in the Gladius Event Viewer (LogView component).

## Research Topics

### 1. ImGui Clipboard API

**Decision**: Use `ImGui::SetClipboardText()` for clipboard operations

**Rationale**:
- ImGui provides cross-platform clipboard support out of the box
- `ImGui::SetClipboardText(const char* text)` sets clipboard content
- `ImGui::GetClipboardText()` retrieves clipboard (not needed for this feature)
- No additional dependencies required
- Works on all target platforms (Windows, Linux, macOS)

**Alternatives Considered**:
- Native clipboard APIs (platform-specific) - Rejected: unnecessary complexity, ImGui already abstracts this
- External libraries (e.g., clip) - Rejected: adds dependency, ImGui sufficient

### 2. ImGui Context Menu Pattern

**Decision**: Use `ImGui::BeginPopupContextItem()` for right-click context menus

**Rationale**:
- Standard ImGui pattern for context menus
- Automatically positioned near the click location
- Works with the existing list clipper pattern used in `renderExpandedView()`

**Implementation Pattern**:
```cpp
// In the event rendering loop
ImGui::PushID(index);  // Unique ID for each event
if (ImGui::Selectable("##event", false, ImGuiSelectableFlags_SpanAllColumns))
{
    // Optional: track selection for keyboard shortcut
}
if (ImGui::BeginPopupContextItem("event_context"))
{
    if (ImGui::MenuItem("Copy"))
    {
        // Copy this event to clipboard
    }
    ImGui::EndPopup();
}
ImGui::PopID();
```

**Alternatives Considered**:
- `ImGui::OpenPopupOnItemClick()` - Works but `BeginPopupContextItem` is simpler
- Custom popup positioning - Rejected: ImGui handles positioning automatically

### 3. Keyboard Shortcut Handling

**Decision**: Use `ImGui::IsKeyPressed()` with modifier check

**Rationale**:
- ImGui provides `ImGui::IsKeyPressed(ImGuiKey key)` for key detection
- Use `ImGui::GetIO().KeyCtrl` for Ctrl modifier (maps to Cmd on macOS)
- Check when window is focused: `ImGui::IsWindowFocused()`

**Implementation Pattern**:
```cpp
if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
{
    copySelectedEventToClipboard();
}
```

### 4. Event Formatting for Clipboard

**Decision**: Use fmt library with ISO 8601 timestamp format

**Rationale**:
- fmt is already used in LogView.cpp for timestamp formatting
- ISO 8601 format (`YYYY-MM-DD HH:MM:SS`) is unambiguous and parseable
- Consistent with existing code patterns

**Format Specification**:
```
[2026-01-04 14:30:45] [WARNING] Message text here
[2026-01-04 14:30:46] [ERROR] Another message
```

**Implementation**:
```cpp
std::string formatEventForClipboard(events::Event const& event)
{
    auto inTime = std::chrono::system_clock::to_time_t(event.getTimeStamp());
    std::tm tm{};
    localtime_r(&inTime, &tm);  // Use localtime_s on Windows
    
    char const* severityStr = "INFO";
    switch (event.getSeverity())
    {
        case events::Severity::Warning: severityStr = "WARNING"; break;
        case events::Severity::Error: severityStr = "ERROR"; break;
        case events::Severity::FatalError: severityStr = "FATAL"; break;
        default: break;
    }
    
    return fmt::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}\n", tm, severityStr, event.getMessage());
}
```

### 5. Visual Feedback for Copy Operations

**Decision**: Use ImGui notification/tooltip pattern

**Rationale**:
- Brief, non-intrusive feedback
- Consistent with existing UI patterns

**Options**:
1. **Tooltip on button hover after copy** - Simple, shows "Copied!" briefly
2. **Temporary overlay notification** - More visible but requires timer management
3. **Button text change** - "Copy" → "Copied!" for 1-2 seconds

**Recommendation**: Option 1 (tooltip) for simplicity; can upgrade to Option 3 if needed.

### 6. Collapsed View Copy Buttons

**Decision**: Add "Copy" button inside tooltip popups

**Rationale**:
- Tooltips in ImGui are actually windows, can contain interactive elements
- Current implementation uses `ImGui::BeginTooltip()` which supports buttons
- Need to switch to `ImGui::BeginPopup()` for persistent interaction

**Challenge**: Standard tooltips disappear when mouse moves; need persistent popup

**Solution**: Convert severity count areas to button-like elements that open a persistent popup:
```cpp
if (ImGui::SmallButton(fmt::format(ICON_FA_EXCLAMATION_TRIANGLE " Errors: {}", errorCount).c_str()))
{
    ImGui::OpenPopup("errors_popup");
}
if (ImGui::BeginPopup("errors_popup"))
{
    // List errors with copy button
    if (ImGui::Button("Copy All Errors"))
    {
        copyEventsBySeverity(events::Severity::Error);
    }
    ImGui::Separator();
    // ... list error messages ...
    ImGui::EndPopup();
}
```

## Design Decisions Summary

| Topic | Decision | Key Reason |
|-------|----------|------------|
| Clipboard API | `ImGui::SetClipboardText()` | Cross-platform, no dependencies |
| Context Menu | `BeginPopupContextItem()` | Standard ImGui pattern |
| Keyboard | `IsKeyPressed()` + `KeyCtrl` | Native ImGui support |
| Format | ISO 8601 + severity + message | Parseable, readable |
| Feedback | Tooltip "Copied!" | Simple, non-intrusive |
| Collapsed Copy | Button → Popup pattern | Interactive, persistent |

## Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Large clipboard content | Low | Medium | Limit copy to visible/filtered events; ImGui handles internally |
| Platform clipboard issues | Very Low | High | ImGui abstracts; test on all platforms |
| Selection tracking complexity | Low | Low | Start without selection, add if needed |

## Open Questions (Resolved)

1. ~~Should copied events include the full timestamp with milliseconds?~~
   → **Resolved**: Use seconds precision (matches display format)

2. ~~Should there be a "Select All" function?~~
   → **Resolved**: Not needed; "Copy All" copies all visible events without selection

3. ~~Should the collapsed view tooltips become popups?~~
   → **Resolved**: Yes, convert to popups for interactive copy buttons
