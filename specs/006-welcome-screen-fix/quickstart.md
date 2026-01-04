# Quickstart: Welcome Screen Improvements

**Feature**: 006-welcome-screen-fix  
**Phase**: 1 - Design  
**Date**: January 3, 2026

## Overview

This feature fixes three issues in the welcome screen:
1. **Async thumbnail loading**: Prevent UI freezes when loading thumbnails
2. **File selection bug**: Fix timing issue causing default template to load
3. **Layout persistence**: Preserve ImGui docking layout after closing

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        MainWindow                            │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                     WelcomeScreen                        │ │
│  │  ┌───────────────┐  ┌────────────────────────────────┐  │ │
│  │  │ ThumbnailGrid │  │     AsyncThumbnailLoader       │  │ │
│  │  │               │  │  ┌────────────────────────┐    │  │ │
│  │  │ [thumb1]      │◄─┼──│ Background Thread Pool │    │  │ │
│  │  │ [thumb2]      │  │  │ (std::async futures)   │    │  │ │
│  │  │ [thumb3]      │  │  └────────────────────────┘    │  │ │
│  │  │ [loading...]  │  │              │                 │  │ │
│  │  └───────────────┘  │              ▼                 │  │ │
│  │         │           │  ┌────────────────────────┐    │  │ │
│  │         │ click     │  │ Main Thread Texture    │    │  │ │
│  │         ▼           │  │ Creation Queue         │    │  │ │
│  │  ┌─────────────────┐│  └────────────────────────┘    │  │ │
│  │  │m_pendingFileOpen││                                │  │ │
│  │  └────────┬────────┘│                                │  │ │
│  └───────────┼─────────┴────────────────────────────────┘  │
│              │                                              │
│              ▼                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ processWelcomeScreenClose()                             ││
│  │   if (pendingFile) open(pendingFile);                   ││
│  │   // Layout preserved - no reset operations             ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

## Implementation Order

### Step 1: Fix File Selection Bug (P1, Low Risk)

**Files**: `WelcomeScreen.h`, `WelcomeScreen.cpp`, `MainWindow.cpp`

1. Add `m_pendingFileOpen` member to WelcomeScreen
2. Store file path on click instead of calling callback immediately
3. Remove `m_isVisible = false` from click handler
4. Add `processFileOpen()` method to retrieve and clear pending path
5. In MainWindow, check for pending file after welcome screen closes
6. Call `open(path)` only after confirming file exists

**Test**: Click thumbnails rapidly; verify correct file always loads.

### Step 2: Fix Layout Persistence (P2, Low Risk)

**Files**: `MainWindow.cpp`, `GLView.cpp`

1. Identify code that resets docking layout on welcome close
2. Remove or guard any layout-affecting operations during close transition
3. Ensure `startAnimationMode()` doesn't trigger dock rebuilds
4. Verify imgui.ini is not reloaded during transition

**Test**: Customize layout, restart app, close welcome screen; verify layout preserved.

### Step 3: Implement Async Thumbnail Loading (P1, Medium Risk)

**Files**: `AsyncThumbnailLoader.h/cpp` (new), `ThreemfThumbnailExtractor.h/cpp`, `WelcomeScreen.h/cpp`

1. Create `AsyncThumbnailLoader` class
2. Add `ThumbnailLoadState` enum to `ThumbnailInfo`
3. Add `extractThumbnailDataOnly()` method to extractor (returns decoded pixels without GL texture)
4. Modify `WelcomeScreen::updateThumbnailInfos()` to use async loader
5. Add `processPendingTextures()` call in render loop
6. Handle cancellation on welcome screen close

**Test**: Launch with 50 recent files; verify UI responsive, thumbnails appear progressively.

## Key Code Patterns

### Async Load Pattern

```cpp
// In AsyncThumbnailLoader::requestLoad()
auto future = std::async(std::launch::async, [filePath, this]() {
    ThumbnailLoadResult result;
    try {
        // Thread-safe: creates own lib3mf wrapper
        auto wrapper = gladius::io::loadLib3mfScoped();
        auto pngData = extractThumbnailData(wrapper, filePath);
        result.decodedPixels = decodePng(pngData, result.width, result.height);
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }
    return result;
});
m_pendingLoads.push_back({&info, std::move(future)});
```

### Pending File Open Pattern

```cpp
// In WelcomeScreen click handler
void WelcomeScreen::handleThumbnailClick(const ThumbnailInfo& info)
{
    if (!m_pendingFileOpen.has_value()) {
        m_pendingFileOpen = info.filePath;
        m_isVisible = false;  // Single place to hide
    }
}

// In MainWindow render loop
if (!m_welcomeScreen.isVisible() && m_wasWelcomeScreenVisible) {
    if (auto path = m_welcomeScreen.processFileOpen()) {
        if (std::filesystem::exists(*path)) {
            open(*path);
        } else {
            m_logger->addEvent({"File not found: " + path->string(), Severity::Error});
        }
    }
}
```

## Testing Checklist

- [ ] Unit test: AsyncThumbnailLoader loads thumbnails correctly
- [ ] Unit test: AsyncThumbnailLoader cancellation works
- [ ] Unit test: ThumbnailLoadState transitions are valid
- [ ] Integration test: Click thumbnail → correct file opens
- [ ] Integration test: Click while loading → correct file opens
- [ ] Integration test: Click on missing file → error shown
- [ ] Integration test: Layout preserved after welcome close
- [ ] Performance test: 50 recent files, UI responsive

## Dependencies

- No new external dependencies
- Uses existing: std::async, std::future, lodepng, lib3mf, ImGui

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| lib3mf not thread-safe | Create new wrapper per thread |
| Race condition in file open | Use optional<path> with atomic access pattern |
| Layout still resets | Fallback: force reload from imgui.ini after close |
| Texture creation fails | Show placeholder, log error, continue |
