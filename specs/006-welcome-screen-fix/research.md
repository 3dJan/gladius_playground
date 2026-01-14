# Research: Welcome Screen Improvements

**Feature**: 006-welcome-screen-fix  
**Phase**: 0 - Research  
**Date**: January 3, 2026

## Research Tasks

### 1. Async Thumbnail Loading Pattern

**Question**: What is the best pattern for async thumbnail loading in a C++ ImGui application?

**Research Findings**:

The codebase already uses `std::async` for background work (see `ManifoldDualContouringStlExporter.cpp:352`, `Document::refreshModelAsync`). The pattern is:

1. Launch work with `std::async(std::launch::async, [](){ ... })`
2. Store the `std::future<T>` as a member variable
3. Poll the future status each frame using `future.wait_for(0ms)`
4. When ready, retrieve result and update UI state on main thread

**Decision**: Use `std::async` with `std::future` to offload file I/O and PNG decoding to background threads. Texture creation remains on main thread (OpenGL requirement).

**Rationale**: Consistent with existing codebase patterns, simple, and efficient for the use case.

**Alternatives Considered**:
- Thread pool: Overkill for thumbnail loading; adds complexity without benefit
- Single background thread with queue: More complex, not needed for ~20 thumbnails

---

### 2. lib3mf Thread Safety

**Question**: Is the lib3mf library thread-safe for concurrent thumbnail extraction?

**Research Findings**:

Examining the codebase usage patterns:
- Each extraction creates a new model instance via `m_wrapper->CreateModel()`
- No shared state between extraction operations
- Each `Lib3MF::PReader` operates on a separate file handle

The lib3mf wrapper (`Lib3MF::PWrapper`) is used to create independent model instances. Each thumbnail extraction creates its own model and reader, so concurrent extractions on different files should be safe.

**Decision**: Create a new `Lib3MF::PWrapper` instance per background thread or use a mutex-protected pool.

**Rationale**: Avoids potential thread-safety issues with the wrapper singleton.

**Alternatives Considered**:
- Single wrapper with mutex: Would serialize all extractions, defeating async purpose
- Assume thread-safe: Risky without explicit documentation

---

### 3. File Selection Race Condition

**Question**: What causes the timing issue where clicking a thumbnail loads the default template instead of the selected file?

**Research Findings**:

In `WelcomeScreen.cpp`, the click handler pattern is:
```cpp
if (ImGui::Button("##thumbnail", ImVec2(cellWidth, cellHeight)))
{
    if (m_openFileCallback)
    {
        m_openFileCallback(info.filePath);
        m_isVisible = false;
    }
}
```

In `MainWindow.cpp`, the callback is:
```cpp
m_welcomeScreen.setOpenFileCallback(
  [this](const std::filesystem::path & path)
  {
      if (path.empty()) { open(); }
      else { open(path); }
      m_welcomeScreen.hide();
  });
```

The issue: `hide()` is called twice - once in the callback and once in `WelcomeScreen::renderThumbnailItem()`. The `MainWindow::newModel()` is called during initialization which may interfere.

Looking at `MainWindow.cpp:133-134`:
```cpp
nodeEditor();
newModel();  // <-- This loads the default template!
```

The race condition: If `newModel()` completes after the `open(path)` call starts but before it finishes loading, the default template overwrites the selected file.

**Decision**: Remove duplicate `hide()` calls and ensure `open(path)` completes before any other file operations. Use a flag to prevent `newModel()` from running if a file load is pending.

**Rationale**: Fix the root cause rather than adding workarounds.

**Alternatives Considered**:
- Defer file opening: Would add latency and complexity
- Queue-based approach: Overkill for this simple fix

---

### 4. ImGui Docking Layout Persistence

**Question**: Why is the ImGui docking layout lost when the welcome screen closes?

**Research Findings**:

In `MainWindow.cpp:419-423`:
```cpp
bool welcomeScreenHasbeenClosed = !welcomeScreenVisible && m_wasWelcomeScreenVisible;
if (welcomeScreenHasbeenClosed)
{
    m_overlayFadeoutActive = true;
    m_mainView.startAnimationMode();
}
```

The `startAnimationMode()` or subsequent operations may be triggering a layout reset. 

In `GLView.cpp:298`, layout is loaded from disk:
```cpp
ImGui::LoadIniSettingsFromDisk(m_iniFileNameStorage.c_str());
```

This happens during initialization. The issue may be that when the welcome screen renders with `ImGuiWindowFlags_NoSavedSettings`, and subsequent dock operations during the close transition cause ImGui to rebuild the dock layout from scratch.

The welcome screen uses:
```cpp
ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
```

The `ImGuiWindowFlags_NoSavedSettings` prevents the welcome screen itself from affecting saved layout, but doesn't address the dock space interaction.

**Decision**: Ensure the dock space is rendered consistently throughout the welcome screen lifecycle. Avoid any operations that would trigger dock layout rebuilding during the close transition.

**Rationale**: Maintain dock space state continuity.

**Alternatives Considered**:
- Force reload layout after close: Would cause a visible flicker
- Disable docking during welcome screen: Would prevent docking features entirely

---

### 5. OpenGL Texture Creation Thread Safety

**Question**: How to handle OpenGL texture creation from background-loaded data?

**Research Findings**:

OpenGL context is thread-local. Texture creation (`glGenTextures`, `glTexImage2D`) must happen on the main thread where the GL context is current.

Current `ThreemfThumbnailExtractor::loadThumbnail()` does:
1. Extract PNG from 3MF (can be async)
2. Decode PNG with lodepng (can be async)
3. Create OpenGL texture (MUST be main thread)

**Decision**: Split thumbnail loading into two phases:
1. **Background phase**: Extract and decode PNG → produces decoded RGBA pixels
2. **Main thread phase**: Create OpenGL texture from decoded pixels

Store decoded pixels in `ThumbnailInfo` and check for pending texture creation each frame.

**Rationale**: Clean separation of thread-safe and thread-unsafe operations.

**Alternatives Considered**:
- Shared GL context: Complex setup, platform-specific issues
- PBO async upload: More complex, marginal benefit for small thumbnails

---

## Summary

| Topic | Decision | Key Insight |
|-------|----------|-------------|
| Async pattern | `std::async` + `std::future` | Consistent with codebase |
| lib3mf thread safety | New wrapper per thread | Avoid shared state |
| Race condition | Fix callback timing, remove duplicate hide() | Root cause is double hide + newModel() |
| Layout persistence | Maintain dock space continuity | Avoid rebuild triggers |
| GL texture creation | Two-phase load (async decode, sync texture) | GL context is thread-local |

All research tasks complete. Ready for Phase 1: Design & Contracts.
