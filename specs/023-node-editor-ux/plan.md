# Implementation Plan: Node Editor UX Improvements

**Branch**: `023-node-editor-ux` | **Date**: 2026-03-12 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/023-node-editor-ux/spec.md`

## Summary

Overhaul the Gladius node editor UX with: enhanced numeric input widgets (orbital dial + adaptive drag-float), visual port compatibility highlighting during link creation, compact stylish node rendering with auto-sizing and category color-coded rounded borders, improved begin/end node argument management (reorder, rename, remove with confirmation), parameter change throttling for fluid responsiveness, and improved function call node discoverability.

## Technical Context

**Language/Version**: C++20
**Primary Dependencies**: ImGui, imgui-node-editor (vcpkg: `unofficial::imgui-node-editor`), lib3mf, OpenCL 1.2+
**Storage**: 3MF files (widget layout mode persistence in parameter metadata)
**Testing**: GTest/GMock
**Target Platform**: Linux (primary), Windows (secondary)
**Project Type**: Single desktop application
**Performance Goals**: ≥30 fps in node editor during continuous parameter dragging; parameter widget response <16ms
**Constraints**: Node editor rendering must not block the OpenCL compute pipeline; all new widgets must work within `ax::NodeEditor` node boundaries
**Scale/Scope**: ~50 node types, typically 5-50 nodes per function graph, up to 20 functions per document

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | All new code uses C++20, smart pointers, constexpr, east-side const |
| II. Test-First Development | ✅ PASS | Unit tests for NumericWidget logic, LinkDragState compatibility, ParameterThrottle timing; integration tests for begin/end reorder |
| III. Simplicity First | ✅ PASS | Widgets extracted to focused ~200-line files; no over-abstraction; no speculative features |
| IV. Consistent Code Style | ✅ PASS | camelCase functions, PascalCase types, Allman braces, 4-space indent, <400 line files |
| V. Documentation | ✅ PASS | Public widget APIs documented with Doxygen; no redundant comments |
| VI. UI Responsiveness | ✅ PASS | Parameter throttle decouples UI from compute; debounce prevents recompile storms; widgets never block main thread |

**Post-Phase 1 Re-check**: All principles satisfied. Key design decisions:
- New files (`NumericWidgets.cpp`, `LinkDragState.cpp`, `ParameterThrottle.cpp`) keep code modular and under 400 lines
- Throttle uses existing async compute pipeline patterns (no new threading model)
- Widget rendering uses ImGui draw-list primitives (no external dependencies)

## Project Structure

### Documentation (this feature)

```text
specs/023-node-editor-ux/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0: technology research
├── data-model.md        # Phase 1: entity/state model
├── quickstart.md        # Phase 1: dev quickstart guide
├── contracts/
│   └── widget-api.md    # Phase 1: C++ API contracts
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code (repository root)

```text
gladius/src/ui/
├── NumericWidgets.h          # NEW: orbital dial, adaptive drag-float, slider
├── NumericWidgets.cpp         # NEW: widget implementations
├── LinkDragState.h            # NEW: port compatibility during link drag
├── LinkDragState.cpp          # NEW: compatibility computation
├── ParameterThrottle.h        # NEW: debounce for parameter→recompile
├── ParameterThrottle.cpp      # NEW: throttle implementation
├── NodeView.h                 # MODIFIED: integrate new widgets, port highlighting
├── NodeView.cpp               # MODIFIED: refactored node rendering
├── ModelEditor.h              # MODIFIED: LinkDragState integration
├── ModelEditor.cpp            # MODIFIED: link creation with compatibility
├── Style.h                    # MODIFIED: extended NodeStyle with rounding/border
├── Style.cpp                  # MODIFIED: hash-based color fallback
├── Widgets.h                  # MODIFIED: minor integration changes
├── MainWindow.cpp             # MODIFIED: throttle integration
└── LinkColors.h               # MODIFIED: highlighted/dimmed variants

gladius/tests/unittests/
├── NumericWidget_tests.cpp    # NEW: sensitivity, bounds, modifier keys
├── LinkDragState_tests.cpp    # NEW: compatibility computation
└── ParameterThrottle_tests.cpp # NEW: timing/debounce tests
```

**Structure Decision**: All new code is added within the existing `gladius/src/ui/` directory as focused, small files. No new directories or project restructuring needed. This follows the existing pattern of one-class-per-file in the ui namespace.

## Complexity Tracking

No constitution violations to justify — all design choices align with established patterns.

## Implementation Phases

### Phase 1: Numeric Widgets (FR-001 through FR-006d)

**Deliverables**:
- `NumericWidgets.h/.cpp` with `orbitalDial()`, `adaptiveDragFloat()`, `numericWidget()`
- Adaptive sensitivity (log-scale + Shift/Ctrl modifiers)
- Keyboard Up/Down support with held-key repeat
- Double-click text entry
- Dial+drag-float paired layout (default) and slider alternative
- Widget layout mode persistence in parameter metadata
- Replace all `ImGui::DragFloat` calls in `NodeView.cpp` with `numericWidget()`
- Unit tests for sensitivity computation and bounds handling

### Phase 2: Node Rendering (FR-012 through FR-014d)

**Deliverables**:
- Extended `NodeStyle` with `borderWidth` and `rounding`
- Heavily rounded rectangle rendering via `ed::StyleVar_NodeRounding`
- Category color-coded border ring
- Hash-based color fallback for unknown type tags
- Content-based auto-sizing (replace fixed widths)
- Truncation with ellipsis for long names
- Unit tests for auto-size computation and color hash

### Phase 3: Port Compatibility Highlighting (FR-007 through FR-011)

**Deliverables**:
- `LinkDragState.h/.cpp` with compatibility computation
- `isLinkCompatible()` extracted from `Model::addLink()`
- Pin rendering with highlight (glow/brightness) for compatible ports
- Pin rendering with dim (reduced opacity) for incompatible ports
- Dynamic type resolution for unresolved ports
- Tooltip showing port name/type on hover during drag
- Unit tests for compatibility computation with static and dynamic types

### Phase 4: Begin/End Node UX (FR-015 through FR-018)

**Deliverables**:
- Visual distinction for begin/end nodes (different header accent)
- Inline rename for arguments/outputs
- Drag-and-drop reorder (with move-up/down button fallback)
- Remove with confirmation when links exist
- Integration test for reorder + link preservation

### Phase 5: Parameter Throttle (FR-019, FR-020)

**Deliverables**:
- `ParameterThrottle.h/.cpp` with debounce logic
- Integration into `MainWindow.cpp` parameter dirty flow
- Widget always updates immediately; recompile debounced at ~100ms
- Unit tests for throttle timing

### Phase 6: Function Call Node Improvements (FR-021 through FR-023)

**Deliverables**:
- Prominent function name in node header
- Navigation action to open referenced function graph
- Searchable function selection list
- Leverages existing `FunctionNavigationHistory` infrastructure
