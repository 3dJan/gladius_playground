# Feature Specification: Node Editor UX Improvements

**Feature Branch**: `023-node-editor-ux`  
**Created**: 2026-03-12  
**Status**: Draft  
**Input**: User description: "Improve the node editor UX: better numeric input widgets with drag and keyboard encoder support, visual port compatibility highlighting when linking nodes, compact stylish node rendering without clipping, fluid responsiveness, improved begin/end node usability, and improved function input/output editing UI"

## Clarifications

### Session 2026-03-12

- Q: What color-coding scheme should be used for node categories? → A: Derive category colors automatically by hashing the node's type tag, consistent with existing group tag-based coloring.
- Q: How should orbital dials handle unbounded parameters (no min/max)? → A: Allow infinite rotation — the dial spins freely without an arc endpoint, and the numeric value display is the primary feedback.
- Q: Where should per-parameter widget mode preferences be persisted? → A: The orbital dial is always shown in combination with a numeric drag-float input — they are paired, not alternative modes. The dial provides visual/rotary interaction while the drag-float provides precise numeric readout and text entry. No per-parameter mode persistence needed for this pairing.
- Q: How aggressively should we pursue circular node rendering? → A: Start with heavily rounded rectangles using the same color-ring styling; true circular nodes as an optional second pass if framework customization allows it.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Intuitive Numeric Value Editing (Priority: P1)

As a user tweaking model parameters, I want numeric input fields that let me fine-tune small values precisely and sweep through large value ranges quickly, so that I can iterate on my designs interactively with near-real-time preview feedback.

Currently, numeric inputs use basic drag-float widgets with a fixed step size that makes it awkward to adjust both fine (e.g., 0.001 mm) and coarse (e.g., 50 mm) values. The improved widget should adapt its sensitivity to the current value magnitude and support multiple interaction modes: click-drag for continuous adjustment, scroll wheel or keyboard up/down (including hardware encoders mapped to arrow keys) for stepped increments, and direct text entry for precise values.

Inspired by the reference design, we also want to include an **orbital dial / rotary knob** control rendered **inline within the node itself** alongside the standard numeric drag-float input. The dial and the drag-float are always shown together as a paired widget — the dial provides visual/rotary interaction while the drag-float provides precise numeric readout and text entry. This is not an alternative mode to switch between; every numeric parameter gets both controls. A **slider** mode may additionally be offered as an alternative presentation. Scalar nodes should display their current value prominently (large readable text), and vector nodes should show labeled X/Y/Z rows with individual value fields.

**Why this priority**: Numeric value editing is the single most frequent interaction in the node editor. Every parameter adjustment goes through these widgets. Making them responsive and tactile directly impacts every user session.

**Independent Test**: Can be fully tested by opening any model with numeric parameters and verifying each interaction mode (drag, scroll, keyboard, text entry) produces responsive, proportional value changes with immediate preview updates.

**Acceptance Scenarios**:

1. **Given** a node with a float parameter, **When** the user clicks and drags horizontally on the value widget, **Then** the value changes continuously and proportionally to drag distance, with visual feedback showing the current value updating in real time.
2. **Given** a node with a float parameter, **When** the user holds a modifier key (e.g., Shift) while dragging, **Then** the sensitivity decreases for fine-grained adjustment (smaller increments per pixel).
3. **Given** a node with a float parameter, **When** the user holds Ctrl while dragging, **Then** the sensitivity increases for coarse adjustment (larger increments per pixel).
4. **Given** a node with a float parameter and the widget focused, **When** the user presses Up/Down arrow keys, **Then** the value increments/decrements by a contextual step size, and repeated key presses (or held keys / encoder rotation) produce smooth, continuous value changes.
5. **Given** a node with a float parameter, **When** the user double-clicks the value widget, **Then** a text input field appears allowing direct keyboard entry of a precise value.
6. **Given** any numeric value change via drag, scroll, or keyboard, **When** the value updates, **Then** the 3D preview re-renders without noticeable lag or UI stutter.
7. **Given** a numeric parameter displayed in a node, **When** the node is rendered, **Then** an orbital dial is shown alongside the drag-float input, and both controls manipulate the same value in sync.
8. **Given** a parameter with the orbital dial, **When** the user drags in a circular motion on the dial, **Then** the value changes proportionally to the angular rotation, with a visual arc indicator (bounded) or continuous rotation (unbounded), and the drag-float updates in sync.
9. **Given** a parameter with a "Slider" alternative selected, **When** the user drags the slider handle, **Then** the value changes linearly across the parameter's range, with the current value displayed alongside the slider.

---

### User Story 2 - Visual Port Compatibility Highlighting During Linking (Priority: P1)

As a user connecting nodes, I want to immediately see which ports are compatible when I start dragging a connection from a port, so that I can quickly and confidently build correct node graphs without trial-and-error.

Currently, port colors indicate type, but when the user starts a link drag, there is no dynamic feedback highlighting which target ports accept the connection. The improved experience should dim or de-emphasize incompatible ports and visually highlight compatible ones, accounting for dynamic type resolution on nodes whose port types change based on their inputs.

**Why this priority**: Node linking is the second most frequent interaction. Unclear compatibility leads to frustration and wasted time, especially for new users and with dynamically-typed nodes.

**Independent Test**: Can be tested by dragging from any output port and verifying that compatible input ports become visually highlighted while incompatible ones are visually suppressed.

**Acceptance Scenarios**:

1. **Given** a node graph with multiple nodes, **When** the user starts dragging a link from an output port, **Then** all compatible input ports across the graph are visually highlighted (e.g., glowing border, increased brightness, pulsing animation).
2. **Given** the user is dragging a link from an output port, **When** viewing incompatible input ports, **Then** those ports are visually de-emphasized (e.g., dimmed, reduced opacity) to clearly distinguish them from compatible ones.
3. **Given** a node with dynamically-typed ports (e.g., a math operation node whose output type depends on its inputs), **When** the user drags a link toward it, **Then** the compatibility check reflects the node's current resolved types, not a static type definition.
4. **Given** the user is dragging a link from an input port (reverse linking), **When** viewing output ports, **Then** compatible output ports are highlighted in the same manner.
5. **Given** the user releases the drag over an incompatible port, **When** the drop occurs, **Then** the connection is rejected and no invalid link is created.
6. **Given** the user is dragging a link, **When** hovering over a compatible port, **Then** a tooltip or visual cue shows the port name and type for confirmation.

---

### User Story 3 - Compact, Stylish Node Rendering Without Clipping (Priority: P2)

As a user working with complex graphs containing many nodes, I want nodes to be compact and visually polished while ensuring all labels, pins, and embedded widgets are fully visible without clipping, so that I can read and interact with every element even in dense layouts.

Currently, node sizing is sometimes inadequate — labels or widgets can clip at node boundaries, and nodes take more space than necessary. Nodes should auto-size to fit their content with appropriate padding, use a consistent visual style, and remain readable at various zoom levels.

The reference design demonstrates a compelling **circular node shape** with color-coded rings that indicate node category (e.g., green for primitives, blue for boolean operations, red for subtractive operations, orange for value/constant nodes). Nodes display their name prominently in the center, with ports arranged around the perimeter. This approach yields a distinctive, modern look and makes it easy to identify node function at a glance through silhouette and color. Where circular shapes are impractical (nodes with many ports or large embedded widgets), a rounded-rectangle fallback with the same color-coding language should be used. An icon or glyph in the node center can further reinforce the node's purpose.

**Why this priority**: Visual clarity directly affects usability in complex graphs. Clipped content forces the user to guess or hover, slowing workflow.

**Independent Test**: Can be tested by creating nodes of various types (including those with long names, many pins, and embedded widgets) and verifying that all content is fully visible at default zoom and at reduced zoom levels.

**Acceptance Scenarios**:

1. **Given** a node with parameters, input pins, and output pins, **When** the node is rendered, **Then** all labels, pin names, and input widgets are fully visible without any clipping or overlap.
2. **Given** a node with a long display name, **When** the node is rendered, **Then** the node header expands to accommodate the full name, or the name is truncated with an ellipsis and the full name shown on hover.
3. **Given** a node with embedded parameter widgets (sliders, text fields), **When** the user interacts with them, **Then** the widgets are large enough to be usable and do not overlap with pin labels.
4. **Given** the user zooms out on a large graph, **When** nodes become smaller, **Then** the most important information (node name, connection state) remains legible, and less critical details (parameter values) gracefully fade or collapse.
5. **Given** multiple node types of varying complexity, **When** rendered side by side, **Then** they share a consistent visual language (spacing, colors, typography, corner rounding) that looks cohesive and modern.
6. **Given** a simple node with few ports (e.g., Sphere with Radius and Center), **When** rendered, **Then** it appears as a heavily rounded rectangle with its name centered, ports on left/right, and a category-specific ring color. True circular rendering may be explored in a future pass.
7. **Given** a complex node with many ports or embedded widgets (e.g., a matrix node or a node with 6+ ports), **When** rendered, **Then** it uses the same rounded-rectangle layout and color-coding language.
8. **Given** any node, **When** the user looks at the graph overview, **Then** the node's category is identifiable by its ring/border color without reading the label.

---

### User Story 4 - Improved Begin/End Node Usability (Priority: P2)

As a user defining function interfaces, I want the begin (inputs) and end (outputs) nodes to provide a clear, intuitive interface for adding, removing, reordering, and renaming function arguments and return values, so that I can quickly set up and modify function signatures.

Currently, the begin/end nodes have a basic table layout with an "Add Argument" / "Add Output" button and type dropdown. The improved experience should make argument management more discoverable, support reordering, and visually differentiate these special nodes from regular computational nodes.

**Why this priority**: Begin/end nodes define the function contract. A confusing interface here leads to structural mistakes in the graph that are hard to debug.

**Independent Test**: Can be tested by creating a new function and using only the begin/end nodes to add, rename, reorder, and remove arguments and outputs, verifying each operation works smoothly.

**Acceptance Scenarios**:

1. **Given** a function's begin node, **When** the user clicks "Add Argument", **Then** a new argument row appears with an editable name field, a type selector, and the new pin is immediately available for linking.
2. **Given** a begin node with multiple arguments, **When** the user drags an argument row up or down, **Then** the argument order changes and connected links follow the reordered pins.
3. **Given** a begin node with an argument, **When** the user clicks a remove control on that argument, **Then** the argument is removed (with a confirmation if links exist) and connected links are disconnected.
4. **Given** a begin or end node, **When** rendered in the graph, **Then** it is visually distinct from regular computation nodes (e.g., different header color, shape accent, or icon) to clearly communicate its special role.
5. **Given** an end node with a single output "shape", **When** the user adds a second output, **Then** the node expands cleanly and both outputs are fully visible with their pins.

---

### User Story 5 - Fluid Responsiveness During Parameter Editing (Priority: P2)

As a user adjusting parameters with real-time preview, I want the UI to remain responsive at all times — parameter changes should never cause the interface to freeze or stutter, even when triggering expensive recompilations or re-renders.

Currently, some parameter changes can cause momentary UI freezes while the compute pipeline reprocesses. The editing experience should decouple UI responsiveness from compute workload, ensuring smooth interaction even during heavy operations.

**Why this priority**: Perceived responsiveness is critical for the "fun" factor of direct manipulation. Any stutter breaks the creative flow.

**Independent Test**: Can be tested by rapidly dragging a parameter value back and forth on a complex model and verifying the UI (node editor panning, widget response) stays fluid throughout.

**Acceptance Scenarios**:

1. **Given** the user is dragging a numeric parameter on a complex model, **When** the compute pipeline is recompiling, **Then** the widget continues to respond to input without freezing or dropping frames.
2. **Given** rapid parameter changes (e.g., fast dragging or repeated key presses), **When** the preview cannot keep up, **Then** intermediate values are coalesced/throttled so only the latest value triggers a full recompute, while the widget always reflects the current user input immediately.
3. **Given** a long-running recompute is in progress, **When** the user interacts with other parts of the UI (panning the canvas, selecting nodes, scrolling), **Then** those interactions remain smooth and unblocked.

---

### User Story 6 - Improved Function Input/Output Editing (Priority: P3)

As a user working with function call nodes, I want a streamlined way to view and edit which function a node calls, see its current parameter bindings at a glance, and navigate to the referenced function, so that I can work efficiently with reusable function blocks.

Currently, function call nodes show a yellow button that opens a selection popup. The improved UX should make function selection more discoverable, show parameter binding status inline, and provide quick navigation to the referenced function.

**Why this priority**: Function call nodes are powerful but currently somewhat opaque. Better discoverability supports the reuse patterns that make the graph system powerful.

**Independent Test**: Can be tested by placing function call nodes, changing their referenced functions, and navigating to those functions, verifying all operations are discoverable without prior knowledge.

**Acceptance Scenarios**:

1. **Given** a function call node, **When** displayed in the graph, **Then** the referenced function name is prominently shown in the node header, and unbound parameters are visually flagged.
2. **Given** a function call node, **When** the user clicks the function name or a dedicated navigation control, **Then** the editor navigates to (or opens) the referenced function's graph.
3. **Given** a function call node, **When** the user wants to change the referenced function, **Then** a searchable selection list appears with function names and previews.

---

### Edge Cases

- What happens when a node has zero input pins and zero output pins? It should still render correctly as a valid (if unusual) node.
- What happens when the user zooms out extremely far? Node content should gracefully simplify (level-of-detail reduction) rather than becoming an illegible mess.
- What happens when two ports have the same type but are semantically incompatible (e.g., a position vector vs. a color vector)? The system should highlight both as compatible since the type system doesn't currently distinguish semantics — compatibility is based on data type only.
- What happens when a dynamic-typed node has no inputs connected yet (unresolved type)? All ports of that node should be shown as potentially compatible, and the tooltip should indicate the type is unresolved.
- What happens when the user starts a link drag and then presses Escape? The drag should cancel cleanly without creating any link or leaving visual artifacts.
- What happens when a begin/end node argument is renamed while links exist? The existing link should be preserved and the connected pin label should update.

## Requirements *(mandatory)*

### Functional Requirements

**Numeric Input Widgets**

- **FR-001**: Numeric input widgets MUST support click-and-drag for continuous value adjustment, with horizontal drag distance proportional to value change.
- **FR-002**: Numeric input widgets MUST support modifier keys to change drag sensitivity — one modifier for fine adjustment (smaller increments) and one for coarse adjustment (larger increments).
- **FR-003**: Numeric input widgets MUST respond to keyboard Up/Down arrow keys for stepped value changes when focused, with step size appropriate to the value's magnitude and content type (e.g., smaller steps for angles, larger for lengths).
- **FR-004**: Numeric input widgets MUST support held arrow keys and rapid repeated presses (e.g., from hardware encoders) to produce smooth, continuous value changes without input lag.
- **FR-005**: Numeric input widgets MUST support double-click to enter direct text input mode for typing precise values.
- **FR-006**: Numeric input widgets MUST visually indicate the current value at all times during drag and keyboard interaction.
- **FR-006a**: Every numeric parameter MUST display an orbital dial (rotary knob) paired with a drag-float input. Both controls manipulate the same value and stay in sync. A slider MAY additionally be offered as an alternative layout.
- **FR-006b**: Users MUST be able to select between the default dial+drag-float layout and an optional slider layout per parameter, and the selection MUST persist with the document.
- **FR-006c**: Orbital dial widgets MUST map circular/angular drag motion to value change. For bounded parameters (with min/max), the dial displays a visual arc indicator showing the current position within the range. For unbounded parameters, the dial spins freely with infinite rotation and the numeric drag-float serves as the primary value feedback.
- **FR-006d**: The orbital dial and its paired drag-float MUST be rendered inline within the node body, and the node MUST auto-resize to fit them.

**Port Compatibility Highlighting**

- **FR-007**: When the user initiates a link drag from any port, the system MUST visually highlight all compatible target ports across the entire visible graph.
- **FR-008**: When the user is dragging a link, the system MUST visually de-emphasize (dim) all incompatible ports.
- **FR-009**: Port compatibility MUST be determined based on the port's current resolved data type, including dynamically-typed ports whose type depends on connected inputs.
- **FR-010**: When hovering over a compatible port during link drag, the system MUST show the port name and type as a tooltip or inline label.
- **FR-011**: The system MUST prevent creation of links between incompatible port types.

**Node Rendering**

- **FR-012**: Nodes MUST auto-size to fully contain all labels, pin names, and embedded widgets without clipping or overlap.
- **FR-013**: Nodes MUST have consistent visual styling including spacing, colors, corner rounding, and typography across all node types.
- **FR-014**: Node display names that exceed available width MUST be truncated with an ellipsis, with the full name shown on hover.
- **FR-014a**: Nodes MUST use category-based color coding (ring/border color) derived automatically by hashing the node's type tag, consistent with the existing group tag-based coloring system. This ensures new node types automatically receive distinct colors without manual assignment.
- **FR-014b**: All nodes MUST be rendered as heavily rounded rectangles with a color-coded ring/border. True circular shapes for simple nodes (few ports) MAY be explored as an optional second pass if framework customization allows, but are not required for the initial implementation.
- **FR-014c**: Complex nodes that cannot fit a circular layout MUST fall back to a rounded-rectangle shape that shares the same color-coding language.
- **FR-014d**: Nodes MAY display a category icon or glyph in the center to further reinforce their purpose.

**Begin/End Nodes**

- **FR-015**: Begin and end nodes MUST be visually distinct from regular computation nodes through differentiated styling (color, header accent, or icon).
- **FR-016**: Users MUST be able to add, rename, and remove arguments/outputs on begin/end nodes through clearly discoverable controls.
- **FR-017**: Users MUST be able to reorder arguments/outputs on begin/end nodes via drag-and-drop, with connected links following the reordered pins.
- **FR-018**: Removing an argument/output that has connected links MUST prompt for confirmation before disconnecting.

**Responsiveness**

- **FR-019**: UI interactions (widget manipulation, canvas panning, node selection) MUST remain responsive during compute pipeline reprocessing, with no perceptible frame drops in the editor.
- **FR-020**: Rapid parameter changes MUST be coalesced so that only the latest value triggers a full recompute, while the widget always reflects the user's current input immediately.

**Function Call Node Improvements**

- **FR-021**: Function call nodes MUST prominently display the referenced function name in the node header.
- **FR-022**: Function call nodes MUST provide a navigation action to open the referenced function's graph.
- **FR-023**: Function selection MUST provide a searchable list of available functions.

### Key Entities

- **Numeric Input Widget**: A UI component for editing scalar, vector, integer, angle, and matrix values. Has properties: value, content type (length, angle, color), step size, sensitivity mode, and display format.
- **Port**: A connection point on a node, either input or output. Has properties: name, data type (which can be static or dynamically resolved), visual state (normal, highlighted, dimmed), and connection status.
- **Node**: A visual block in the graph representing a computation. Has properties: display name, type, pins (input/output ports), embedded parameter widgets, and visual style.
- **Begin/End Node**: A special node representing a function's input arguments or output values. Has properties: ordered list of arguments/outputs, each with a name and type.
- **Link**: A directed connection between an output port and an input port. Has properties: source port, target port, type compatibility status, and visual style (color based on data type).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can adjust a numeric parameter from a coarse value to a fine-tuned value (e.g., from 10.0 down to 0.005) in under 5 seconds using drag interaction alone, without needing text entry.
- **SC-002**: When initiating a link drag, all compatible ports are visually distinguished from incompatible ones within 1 frame of the drag starting — the user never has to guess.
- **SC-003**: No node type in the system exhibits clipped or overlapping content at default zoom level.
- **SC-004**: The node editor maintains at least 30 fps during continuous parameter dragging, even on models that take over 1 second to recompile.
- **SC-005**: Users can add, rename, reorder, and remove a function argument on a begin node in under 10 seconds each, with each operation achievable in 2 clicks or fewer (plus typing for rename).
- **SC-006**: 90% of first-time users can successfully create a correct node link on first attempt when port highlighting is active, compared to the current experience.
- **SC-007**: The visual styling of nodes is perceived as modern and cohesive — no visual inconsistencies between node types (verified by visual review).
- **SC-008**: Node categories are identifiable by color alone (without reading labels) at normal and zoomed-out views, matching the color-coding scheme defined for each category.
- **SC-009**: Every numeric parameter displays an orbital dial paired with a drag-float input by default, with an optional slider alternative available.

## Assumptions

- The existing imgui-node-editor library supports custom styling of pins and nodes sufficient to implement highlighting and dimming effects. If it does not, the node editor wrapper layer may need to be extended.
- Orbital dial, slider, and other alternative widget modes are rendered **inline within the node body**. The orbital dial is always paired with a drag-float input (not an alternative mode). Nodes auto-resize to accommodate the widget layout. This keeps all parameter editing self-contained within the graph.
- Hardware keyboard encoders that map to Up/Down arrow keys will generate standard key repeat events that the input system can process like normal keyboard input — no special driver integration is required.
- The existing asynchronous compute pipeline architecture can be leveraged for parameter change throttling — this feature does not require a new async framework, only integration with the existing one.
- The initial implementation uses heavily rounded rectangles with color-coded ring styling for all nodes. True circular node shapes may be explored as a second pass if the imgui-node-editor framework can be customized for perimeter port placement and circular hit-testing without destabilizing the editor.
- The modifier keys for drag sensitivity (Shift for fine, Ctrl for coarse) follow common conventions in DCC tools (Blender, Houdini, etc.) and do not conflict with existing editor shortcuts.
