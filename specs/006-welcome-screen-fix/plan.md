# Implementation Plan: Welcome Screen Improvements

**Branch**: `006-welcome-screen-fix` | **Date**: January 3, 2026 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/006-welcome-screen-fix/spec.md`

## Summary

Improve the welcome screen with three fixes: (1) async thumbnail loading using `std::async` to prevent UI freezes, (2) fix race condition in file selection callback timing, (3) preserve ImGui docking layout when welcome screen closes by avoiding layout-resetting operations during the close transition.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (docking branch), lib3mf, OpenGL, lodepng  
**Storage**: imgui.ini for layout persistence, file system for 3MF files  
**Testing**: GTest/GMock, VS Code task "Run Gladius Tests (linux-releaseWithDebug)"  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single desktop application  
**Performance Goals**: 60 fps UI, <100ms welcome screen appearance, <16ms per frame  
**Constraints**: OpenGL texture creation must happen on main thread; file I/O can be offloaded  
**Scale/Scope**: Up to 100 recent files, ~20 visible thumbnails at once

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | Will use `std::async`, `std::future`, smart pointers |
| II. Test-First Development | ✅ PASS | Unit tests for async loader, callback timing |
| III. Simplicity First (KISS, DRY, YAGNI) | ✅ PASS | Minimal changes to existing classes, no over-engineering |
| IV. Consistent Code Style | ✅ PASS | Will follow existing patterns in WelcomeScreen.cpp |
| V. Documentation and Comments | ✅ PASS | Doxygen comments for new public APIs |

**Gate Status**: ✅ PASS - All principles satisfied, proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/006-welcome-screen-fix/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
gladius/src/ui/
├── WelcomeScreen.h           # Modified: add async loading state
├── WelcomeScreen.cpp         # Modified: async thumbnail loading, callback fix
├── ThreemfThumbnailExtractor.h   # Modified: add thread-safe extraction method
├── ThreemfThumbnailExtractor.cpp # Modified: separate I/O from texture creation
├── AsyncThumbnailLoader.h    # New: background thumbnail loading component
├── AsyncThumbnailLoader.cpp  # New: std::async-based loader
├── MainWindow.cpp            # Modified: fix callback timing, layout preservation
└── GLView.cpp                # Review: ensure layout not reset on welcome close

gladius/tests/unittests/ui/
├── AsyncThumbnailLoader_tests.cpp  # New: unit tests for async loader
└── WelcomeScreen_tests.cpp         # New/Modified: tests for callback timing
```

**Structure Decision**: Modifications to existing UI components in `gladius/src/ui/`, with a new `AsyncThumbnailLoader` class to encapsulate background loading. Tests in `gladius/tests/unittests/ui/`.

## Complexity Tracking

> No violations - design follows constitution principles.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| N/A | N/A | N/A |
