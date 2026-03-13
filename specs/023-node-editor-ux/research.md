# Research: Node Editor UX Improvements (023)

## R1: ImGui Custom Widget Rendering Within Node Editor

**Decision**: Implement custom ImGui widgets (orbital dial, enhanced drag-float) rendered inline within `ax::NodeEditor` nodes.

**Rationale**: The existing `NodeView.cpp` already renders custom content inside `ed::BeginNode()`/`ed::EndNode()` blocks using standard ImGui draw calls (`ImGui::DragFloat`, `ImGui::InputText`, etc.). Custom widgets like orbital dials are just ImGui draw-list primitives (arcs, circles) combined with input handling — no node-editor API extension needed.

**Alternatives considered**:
- External popup widgets: Rejected — breaks the inline editing UX goal; adds modal friction.
- Separate widget overlay layer: Rejected — complicates hit-testing and z-ordering inside node editor.

## R2: imgui-node-editor Pin Styling & Highlighting API

**Decision**: Use `ax::NodeEditor::PushStyleColor` / `PushStyleVar` for pin highlighting, combined with custom draw-list overlays for glow/pulse effects during link drag.

**Rationale**: The imgui-node-editor library (used via vcpkg as `unofficial::imgui-node-editor`) supports:
- `ed::PushStyleColor(StyleColor_PinRect, ...)` for pin backgrounds
- `ed::GetPinRect()` to get pin bounds for custom overlay drawing
- `ed::BeginCreate()` / `ed::QueryNewLink()` already capture link-drag state in `ModelEditor.cpp:821`
- Pin kind (`Input`/`Output`) and type information is available from the model during render

The link-drag state is already tracked. We need to extend it to compute and cache compatible ports per drag session.

**Alternatives considered**:
- Forking imgui-node-editor for custom pin rendering: Rejected — fragile, hard to maintain across vcpkg updates.
- Post-processing glow shader: Rejected — overkill; ImGui draw-list circles with alpha are sufficient.

## R3: Port Compatibility Resolution Including Dynamic Types

**Decision**: Implement type compatibility checking at the `Model` layer by querying each port's resolved type. Cache the compatibility map when a link drag begins, refresh only if the source port changes.

**Rationale**: The `Model` already has `addLink()` which validates type compatibility. We need to extract the compatibility check into a reusable `isLinkCompatible(PortId source, ParameterId target)` method. For dynamic-typed nodes, the port's current resolved type from connected inputs should be used. If unresolved (no inputs connected), all types are considered compatible.

**Alternatives considered**:
- Static type table only: Rejected — doesn't handle dynamically-typed nodes.
- Re-check on every frame: Rejected — wasteful; cache on drag-start is sufficient.

## R4: Adaptive Drag Sensitivity Strategy

**Decision**: Use a logarithmic sensitivity model: `increment = base_step * pow(10, floor(log10(abs(value) + epsilon)))`, with modifier key multipliers: Shift = ×0.01 (fine), Ctrl = ×100 (coarse).

**Rationale**: This follows DCC tool conventions (Blender, Houdini). The current implementation in `NodeView.cpp:1572` uses a fixed `increment = 0.01f` for all drag operations. The logarithmic model ensures:
- Near zero: tiny steps (precision for 0.001-range values)
- Large values: proportional steps (usable for 100+ range values)
- Modifier keys provide two additional orders of magnitude in each direction

**Alternatives considered**:
- Fixed step size with mode switching: Rejected — adds cognitive load per parameter.
- Velocity-based sensitivity (faster drag = larger steps): Rejected — hard to control precisely; can combine later as enhancement.

## R5: Orbital Dial Widget Design

**Decision**: Implement a custom ImGui widget that renders a circular arc/knob using `ImDrawList` primitives. The dial is always paired with a drag-float input, side by side within the node body.

**Rationale**: ImGui's `ImDrawList` provides `AddCircle`, `AddArc`, `PathArcTo`, and `PathStroke` — sufficient for rendering a dial. Input handling uses `ImGui::InvisibleButton` + `ImGui::IsItemActive` + mouse delta converted to angular delta. For bounded parameters, the arc shows min/max. For unbounded, the dial rotates freely with accumulated angle.

**Alternatives considered**:
- Third-party knob widget library: Rejected — adds dependency; the widget is simple enough to implement in ~150 lines.
- Canvas-level custom rendering outside ImGui: Rejected — breaks ImGui layout flow within nodes.

## R6: Node Auto-Sizing and Anti-Clipping

**Decision**: Replace fixed-width `ImGui::PushItemWidth(150 * m_uiScale)` calls with content-measured sizing. Use `ImGui::CalcTextSize()` to determine minimum node width, then add padding for embedded widgets.

**Rationale**: Current `NodeView::header()` at line 403 pushes fixed 150px item width and `NodeView::visit(Begin&)` uses fixed 400px table width. These cause clipping on long names and waste space on short ones. ImGui's layout system can auto-size if we remove the fixed constraints and instead use `ImGui::SetNextItemWidth(-FLT_MIN)` or measure content.

**Alternatives considered**:
- Fixed size increase: Rejected — just moves the clipping threshold.
- Scrollable node content: Rejected — bad UX for node graphs; content should always be visible.

## R7: Category Color Coding via Type Tag Hashing

**Decision**: Use the existing `Style.h` `NodeColors` map for known categories and extend with a hash-based fallback for new/unknown type tags using a deterministic HSV hash: `hue = hash(typeTag) % 360`, `saturation = 0.6`, `value = 0.5`.

**Rationale**: `Style.h` already defines per-category colors (`NodeColors` map) and `NodeView::header()` already uses `m_nodeTypeToColor` for border/background coloring. The spec asks for category-based ring/border coloring derived by hashing the type tag. We extend the existing system rather than replace it — known categories keep their established colors, new ones get auto-assigned via hash.

**Alternatives considered**:
- Manual color assignment for all types: Rejected — doesn't scale to new node types.
- Random colors per session: Rejected — inconsistent across sessions.

## R8: Begin/End Node Reordering via Drag-and-Drop

**Decision**: Implement drag-and-drop reordering of argument rows using ImGui's `BeginDragDropSource` / `BeginDragDropTarget` within the existing table layout.

**Rationale**: The current begin/end node rendering in `NodeView::visit(Begin&)` (line 93) uses an `ImGui::BeginTable` with rows for each argument. ImGui supports drag-and-drop within tables. The model's argument list is an ordered container that supports reordering. Connected links reference port IDs (not indices), so reordering the display order requires updating the port order in the model.

**Alternatives considered**:
- Move-up/move-down buttons: Acceptable fallback if drag-and-drop proves unreliable within node-editor context; can be the initial implementation.
- External dialog for reordering: Rejected — breaks inline editing flow.

## R9: Parameter Change Throttling Architecture

**Decision**: Leverage the existing async compute pipeline. The current `MainWindow.cpp` already handles `m_parameterDirty` flags and `compileRequested` separation. Add a timestamp-based throttle: accumulate parameter changes in the UI thread immediately (widget always shows current value), but defer recompilation requests using a configurable debounce interval (default ~100ms). Use an existing pattern from `RenderWindow.h:300` (`kBboxDebounceDelay`).

**Rationale**: The existing architecture at `MainWindow.cpp:1079-1130` already separates parameter-only changes from structural changes. Parameter-only changes use a fast `updateParameter()` path. Adding debounce on the recompilation trigger ensures the UI stays fluid during rapid dragging while the GPU only recomputes on the latest value.

**Alternatives considered**:
- Background compilation thread: Already exists in the compute pipeline — no new threading needed.
- Frame-skipping on GPU: Rejected — would cause visible render gaps instead of smooth updates.

## R10: Rounded Rectangle Node Shape with Border Ring

**Decision**: Override the node-editor's default node background rendering using `ed::PushStyleVar(ed::StyleVar_NodeRounding, ...)` for heavy rounding, combined with a thicker border via `ed::StyleVar_NodeBorderWidth`. The category color is applied to the border/ring using `ed::PushStyleColor(ed::StyleColor_NodeBorder, categoryColor)`.

**Rationale**: The imgui-node-editor already supports `NodeRounding` style variable. Current code in `NodeView::header()` uses `ed::PushStyleColor(ed::StyleColor_NodeBorder, color)` — we increase rounding and border width. True circular nodes require significant custom rendering and are deferred to a future pass per spec guidance.

**Alternatives considered**:
- Custom node rendering bypassing the editor: Rejected — would break selection, dragging, and other built-in features.
- True circular shapes in v1: Rejected per spec — "start with heavily rounded rectangles."
