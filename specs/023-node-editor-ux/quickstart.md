# Quickstart: Node Editor UX Improvements (023)

## Branch

```bash
git checkout 023-node-editor-ux
```

## Build

Use the VS Code task **"Build ALL (linux-releaseWithDebug)"** or:
```bash
cd gladius && cmake --build out/build/linux-releaseWithDebug --parallel 8
```

## Test

Use the VS Code task **"Run Gladius Tests (linux-releaseWithDebug)"** or:
```bash
cd gladius/out/build/linux-releaseWithDebug/tests/unittests && ./gladius_test
```

Filter to feature-related tests:
```bash
./gladius_test --gtest_filter='*NumericWidget*:*OrbitalDial*:*LinkDrag*:*PortCompat*:*ArgumentReorder*:*ParameterThrottle*'
```

## Key Files to Modify

### New Files
| File | Purpose |
|------|---------|
| `src/ui/NumericWidgets.h/.cpp` | Orbital dial, adaptive drag-float, slider widgets |
| `src/ui/LinkDragState.h/.cpp` | Port compatibility tracking during link creation |
| `src/ui/ParameterThrottle.h/.cpp` | Debounce controller for parameter → recompile |
| `tests/unittests/NumericWidget_tests.cpp` | Widget logic tests (sensitivity, bounds) |
| `tests/unittests/LinkDragState_tests.cpp` | Port compatibility computation tests |
| `tests/unittests/ParameterThrottle_tests.cpp` | Throttle/debounce timing tests |

### Modified Files
| File | Changes |
|------|---------|
| `src/ui/NodeView.h/.cpp` | Replace DragFloat calls with numericWidget; enhanced begin/end rendering; port highlighting; auto-sizing; rounded style |
| `src/ui/ModelEditor.h/.cpp` | Integrate LinkDragState into link creation flow; pass drag state to NodeView |
| `src/ui/Style.h/.cpp` | Add borderWidth/rounding to NodeStyle; extend color hash fallback |
| `src/ui/Widgets.h/.cpp` | Move/integrate floatEdit, angleEdit into new NumericWidgets |
| `src/ui/MainWindow.cpp` | Integrate ParameterThrottle into parameter dirty → recompile flow |
| `src/ui/LinkColors.h` | Add dimmed/highlighted color variants |

## Implementation Order

1. **NumericWidgets** — Orbital dial + adaptive drag-float (FR-001 through FR-006d)
2. **Node Rendering** — Rounded style, auto-sizing, category colors (FR-012 through FR-014d)
3. **Port Compatibility** — LinkDragState + highlighting (FR-007 through FR-011)
4. **Begin/End Nodes** — Enhanced argument management UI (FR-015 through FR-018)
5. **Parameter Throttle** — Debounce integration (FR-019, FR-020)
6. **Function Call Nodes** — Display name, navigation, searchable selection (FR-021 through FR-023)

## Development Notes

- `NodeView.cpp` is 3027 lines — when adding new widget code, prefer extracting to `NumericWidgets.cpp` to keep files under 400 lines
- The `ax::NodeEditor` API used is from vcpkg package `unofficial::imgui-node-editor`
- ImGui draw list API (`ImDrawList*`) for custom widget rendering: `GetWindowDrawList()`, `AddCircle`, `PathArcTo`, `PathStroke`
- Existing debounce pattern in `RenderWindow.h:300` (`kBboxDebounceDelay`) can serve as reference
- Widget layout mode persistence (FR-006b) should use the existing parameter metadata in the 3MF document
