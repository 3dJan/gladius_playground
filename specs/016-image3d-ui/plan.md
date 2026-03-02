# Implementation Plan: Image3D & FunctionFromImage3D UI

**Branch**: `016-image3d-ui` | **Date**: 2026-01-26 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/016-image3d-ui/spec.md`

## Summary

Extend Gladius UI to provide dedicated workflows for ImageStack viewing/editing and FunctionFromImage3D configuration. Create a tabbed interface for function editing (Properties vs Graph tabs), 2D layer viewer with navigation, and integrated preview capabilities—all following existing ImGui patterns.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: ImGui (UI), Lib3MF (3MF I/O), OpenCL (GPU compute)  
**Storage**: In-memory Assembly + 3MF serialization  
**Testing**: GTest/GMock  
**Target Platform**: Linux (primary), Windows  
**Project Type**: Single desktop application  
**Performance Goals**: <200ms layer navigation, <500ms preview update  
**Constraints**: UI thread must remain responsive (Constitution VI)  
**Scale/Scope**: Single user, locally opened 3MF files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ | ✅ PASS | C++20 features, smart pointers, STL algorithms |
| II. Test-First | ✅ PASS | Unit tests for transforms, integration for UI |
| III. Simplicity | ✅ PASS | Extends existing patterns, no over-engineering |
| IV. Code Style | ✅ PASS | Follow existing naming/formatting conventions |
| V. Documentation | ✅ PASS | Doxygen for public APIs |
| VI. UI Responsiveness | ⚠️ REQUIRES ATTENTION | Large ImageStack loading must be async; see design |

**VI. UI Responsiveness Compliance**:
- Layer navigation: Texture is pre-loaded, slider updates are immediate
- Preview rendering: May require async for large stacks; initial impl uses cached texture
- Import: Existing async pattern in `ImageStackCreator`, add progress callback
- Transforms: Process in background for large stacks (>100 layers)

## Project Structure

### Documentation (this feature)

```text
specs/016-image3d-ui/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/src/
├── ui/
│   ├── ImageStackView.h           # NEW: Layer viewer panel
│   ├── ImageStackView.cpp         # NEW
│   ├── FunctionFromImage3DView.h  # NEW: Properties panel for FunctionFromImage3D
│   ├── FunctionFromImage3DView.cpp # NEW
│   ├── ModelEditor.h              # MODIFY: Tab switching, function type detection
│   ├── ModelEditor.cpp            # MODIFY
│   ├── Outline.h                  # MODIFY: Show ImageStacks, context menus
│   ├── Outline.cpp                # MODIFY
│   └── ResourceView.cpp           # MODIFY: Integration with ImageStackView
├── io/3mf/
│   ├── ImageStack.h               # MODIFY: Add transform methods
│   ├── ImageStack.cpp             # NEW: Transform implementations
│   └── ImageStackCreator.cpp      # MODIFY: Dimension padding, progress
├── nodes/
│   └── Builder.cpp                # Reference only (existing FunctionFromImage3D creation)
└── ImageStackResource.h           # Reference only (existing resource wrapper)

gladius/tests/unittests/
├── ImageStackTransform_Test.cpp   # NEW: Unit tests for flip/rotate
└── FunctionFromImage3DView_Test.cpp # NEW: Integration tests for config panel
```

**Structure Decision**: Single project structure. New UI components follow existing `*View` naming pattern. Tests in existing test directory.

## Data Model

### Key Entities

```cpp
// Existing - no changes needed
class ImageStack {
    std::vector<Image> m_stack;
    ResourceId m_resourceId;
public:
    // NEW transform methods
    void flipHorizontal();
    void flipVertical();
    void rotate90CW();
    void rotate90CCW();
};

// Existing - SamplingSettings struct
struct SamplingSettings {
    TextureTileStyle tileStyleU = TTS_WRAP;
    TextureTileStyle tileStyleV = TTS_WRAP;
    TextureTileStyle tileStyleW = TTS_WRAP;
    SamplingFilter filter = SF_LINEAR;
    float offset = 0.0f;
    float scale = 1.0f;
};

// NEW UI state
struct FunctionFromImage3DViewState {
    ResourceId selectedImageStackId;
    int previewSliceIndex = 0;
    float previewSliceRange = 1.5f;  // Allow outside [0,1] for tile demo
    bool showTilePreview = false;
};

struct ImageStackViewState {
    int currentLayerIndex = 0;
    float zoom = 1.0f;
    ImVec2 pan = {0, 0};
    GLuint currentLayerTexture = 0;  // Cached OpenGL texture
};
```

## Complexity Tracking

> No constitution violations requiring justification.

## Phase 0 Complete

Research documented in [research.md](research.md). Key findings:
- Existing `SliceView` provides pattern for 2D viewer with zoom/pan
- `ModelEditor` already supports undo via `History` class
- `ImageSampler` node parameters can be directly modified for config changes
- `ImageStackCreator` handles directory import; needs padding extension

## Phase 1 Complete

Design artifacts generated:
- [data-model.md](data-model.md) - Entity definitions, state structures
- [quickstart.md](quickstart.md) - User workflows and API usage
- [contracts/api.md](contracts/api.md) - C++ interface contracts

### Post-Design Constitution Re-Check

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ | ✅ PASS | Design uses smart pointers, STL, RAII |
| II. Test-First | ✅ PASS | Test files specified in structure |
| III. Simplicity | ✅ PASS | Reuses existing patterns (SliceView, History) |
| IV. Code Style | ✅ PASS | Naming follows *View pattern |
| V. Documentation | ✅ PASS | Doxygen comments in contracts |
| VI. UI Responsiveness | ✅ PASS | Async strategies documented for large data |

**All gates pass. Ready for Phase 2 task generation.**

## Next Steps

Run `/speckit.tasks` to generate implementation tasks from this plan.
