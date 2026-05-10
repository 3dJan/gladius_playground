# Quickstart: Event Viewer UX Improvements

**Feature**: 007-event-viewer-ux  
**Date**: 2026-01-04

## Overview

This feature enhances the Gladius Event Viewer (LogView) with:
1. Smarter default filtering (hide Info, show Warnings/Errors/Fatal)
2. Clipboard copy functionality for events

## Files to Modify

| File | Changes |
|------|---------|
| `gladius/src/ui/LogView.h` | Change default for `m_showInfo`, add format/copy method declarations |
| `gladius/src/ui/LogView.cpp` | Implement copy functionality, context menus, keyboard shortcuts |

## Files to Create

| File | Purpose |
|------|---------|
| `gladius/tests/unittests/ui/LogView_tests.cpp` | Unit tests for event formatting |

## Implementation Steps

### Step 1: Change Default Filter (5 min)

In `LogView.h`, change the default value:

```cpp
// Before
bool m_showInfo = true;

// After  
bool m_showInfo = false;
```

### Step 2: Add Format Helper Function (15 min)

In `LogView.cpp`, add a helper to format events for clipboard:

```cpp
namespace
{
    std::string formatEventForClipboard(events::Event const& event)
    {
        auto inTime = std::chrono::system_clock::to_time_t(event.getTimeStamp());
        std::tm tm{};
#ifdef _MSVC_LANG
        localtime_s(&tm, &inTime);
#else
        localtime_r(&inTime, &tm);
#endif
        
        char const* severityStr = "INFO";
        switch (event.getSeverity())
        {
            case events::Severity::Warning: severityStr = "WARNING"; break;
            case events::Severity::Error: severityStr = "ERROR"; break;
            case events::Severity::FatalError: severityStr = "FATAL"; break;
            default: break;
        }
        
        return fmt::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}", 
                          tm, severityStr, event.getMessage());
    }
}
```

### Step 3: Add "Copy All" Button (15 min)

In `renderExpandedView()`, add a button in the toolbar area:

```cpp
ImGui::SameLine();
if (ImGui::Button("Copy All"))
{
    std::string clipboardText;
    for (auto iter = eventsBegin; iter != eventsEnd; ++iter)
    {
        clipboardText += formatEventForClipboard(*iter) + "\n";
    }
    if (!clipboardText.empty())
    {
        ImGui::SetClipboardText(clipboardText.c_str());
    }
}
```

### Step 4: Add Right-Click Context Menu (30 min)

Modify the event rendering loop to support right-click copy:

```cpp
ImGui::PushID(static_cast<int>(std::distance(eventsBegin, iter)));
// ... existing event rendering ...

if (ImGui::BeginPopupContextItem("event_context"))
{
    if (ImGui::MenuItem("Copy"))
    {
        ImGui::SetClipboardText(formatEventForClipboard(*iter).c_str());
    }
    ImGui::EndPopup();
}
ImGui::PopID();
```

### Step 5: Add Collapsed View Copy Buttons (30 min)

Convert the existing tooltips to interactive popups with copy buttons.

### Step 6: Write Unit Tests (30 min)

Create `gladius/tests/unittests/ui/LogView_tests.cpp`:

```cpp
#include <gtest/gtest.h>
#include "EventLogger.h"

namespace gladius::ui::tests
{
    // Test formatting function (extract to testable helper)
}
```

## Build & Test

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run tests
# Use VS Code task: "Run Unit Tests (Fast)"
```

## Key ImGui APIs Used

| API | Purpose |
|-----|---------|
| `ImGui::SetClipboardText(text)` | Copy text to system clipboard |
| `ImGui::BeginPopupContextItem(id)` | Right-click context menu |
| `ImGui::MenuItem(label)` | Menu item in popup |
| `ImGui::IsKeyPressed(key)` | Detect keyboard input |
| `ImGui::GetIO().KeyCtrl` | Check Ctrl modifier |

## Testing Checklist

- [ ] Open Event Viewer → Info messages hidden by default
- [ ] Enable Info checkbox → Info messages appear
- [ ] Right-click event → Context menu with "Copy"
- [ ] Select "Copy" → Event copied to clipboard
- [ ] Click "Copy All" → All visible events copied
- [ ] Apply filter → "Copy All" respects filter
- [ ] Collapsed view → Click severity count opens popup with copy button
