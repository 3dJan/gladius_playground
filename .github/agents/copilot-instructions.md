# gladius Development Guidelines

Auto-generated from all feature plans. Last updated: 2025-12-29

## Active Technologies
- C++20 (as per constitution) + GTest/GMock, CMake 3.21+ (for test presets), CTest (004-test-suite-restructure)
- C++20, OpenCL 1.2 + OpenCL (GPU compute), OpenGL (rendering), ImGui (UI/debug overlay) (005-ray-march-perf)
- N/A (in-memory GPU buffers) (005-ray-march-perf)
- C++20 + ImGui (UI rendering), fmt (string formatting) (007-event-viewer-ux)
- N/A (in-memory event log, session-only filter preferences) (007-event-viewer-ux)

- C++20 + OpenCL 1.2+, existing gladius compute infrastructure (001-spatial-sdf)

## Project Structure

```text
src/
tests/
```

## Commands

# Add commands for C++20

## Code Style

C++20: Follow standard conventions

## Recent Changes
- 007-event-viewer-ux: Added C++20 + ImGui (UI rendering), fmt (string formatting)
- 005-ray-march-perf: Added C++20, OpenCL 1.2 + OpenCL (GPU compute), OpenGL (rendering), ImGui (UI/debug overlay)
- 004-test-suite-restructure: Added C++20 (as per constitution) + GTest/GMock, CMake 3.21+ (for test presets), CTest


<!-- MANUAL ADDITIONS START -->
<!-- MANUAL ADDITIONS END -->
