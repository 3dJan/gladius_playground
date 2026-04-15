# Feature Specification: Async Graph Editing

**Feature Branch**: `028-async-graph-editing`  
**Created**: 2026-04-15  
**Status**: Draft  
**Input**: User description: "I want to be able add nodes and links to a graph without any UI thread blocking. all computations and update workflows should run in the background."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Non-Blocking Node Addition (Priority: P1)

As a user building a model in the node editor, I want to add new nodes without the UI freezing, so that my creative flow is uninterrupted even on complex models.

Currently, adding a node triggers synchronous `updateInputsAndOutputs()`, `updateParameterRegistration()`, `Model::updateTypes()`, and an Assembly deep copy for undo — all on the UI thread. On complex models these operations cause noticeable frame drops.

**Why this priority**: Adding nodes is the most fundamental graph editing action. If this blocks, users perceive the entire editor as sluggish. Making this non-blocking is the foundation for all other async graph edits.

**Independent Test**: Can be fully tested by adding nodes to a model with 50+ existing nodes and verifying the graph editor stays at interactive frame rates throughout. Delivers immediate visual responsiveness during the most common editing operation.

**Acceptance Scenarios**:

1. **Given** a complex model is open in the node editor, **When** the user adds a new node via the context menu, **Then** the node appears in the graph immediately and the UI does not stall while background tasks (type inference, parameter registration, undo snapshot) proceed.

2. **Given** the user adds multiple nodes in quick succession (e.g., 5 nodes in under 2 seconds), **When** the system processes these additions, **Then** each node appears instantly in the editor and the background work for earlier additions is coalesced or queued without blocking subsequent additions.

3. **Given** a background compilation is already in progress from a previous edit, **When** the user adds another node, **Then** the node appears instantly and the pending compilation is either cancelled and restarted with the new state or queued to run after the current one completes.

---

### User Story 2 - Non-Blocking Link Creation and Deletion (Priority: P1)

As a user wiring nodes together, I want to create and delete links without experiencing frame drops, so that connecting a complex graph feels fluid.

Link creation triggers the same synchronous pipeline as node addition — type updates, I/O re-registration, parameter registration, and undo snapshots — all on the UI thread.

**Why this priority**: Link operations are equally fundamental to node additions and happen in the same editing session. They share the same blocking bottleneck, so making both non-blocking together is the most coherent approach.

**Independent Test**: Can be fully tested by rapidly connecting and disconnecting links between nodes in a 50+ node model. Delivers smooth drag-and-drop link wiring without input lag.

**Acceptance Scenarios**:

1. **Given** the user is dragging a link from one node's output to another node's input, **When** the link is released on a compatible port, **Then** the link is accepted immediately and the visual connection renders in the same frame, with type inference and recompilation proceeding in the background.

2. **Given** the user deletes a link between two nodes, **When** the deletion is confirmed, **Then** the link disappears immediately and subsequent graph updates happen in the background.

3. **Given** the user creates a link that changes the output type of downstream nodes, **When** type propagation completes in the background, **Then** the node ports visually update to reflect their new types without any UI stall.

---

### User Story 3 - Non-Blocking Node Deletion (Priority: P2)

As a user refactoring a model, I want to delete nodes (including multi-selection deletion) without UI blocking, so that large-scale graph edits feel responsive.

**Why this priority**: Node deletion is less frequent than addition or wiring but involves the same blocking operations plus potentially expensive graph rebuild. It is critical for refactoring workflows on complex models.

**Independent Test**: Can be tested by selecting and deleting a group of 10+ nodes at once in a complex model. Delivers immediate visual removal with background cleanup.

**Acceptance Scenarios**:

1. **Given** the user selects multiple nodes and presses delete, **When** the deletion is processed, **Then** all selected nodes and their links disappear from the graph immediately, and the assembly cleanup and recompilation proceed in the background.

2. **Given** a node deletion triggers a cascade of type changes in connected nodes, **When** the background type inference completes, **Then** affected node ports update their visual type indicators smoothly.

---

### User Story 4 - Paste and Extract-to-Function Without Blocking (Priority: P3)

As a user organizing a complex model, I want paste operations and extract-to-function to complete without freezing the UI, so that structural refactoring operations feel as fluid as simple edits.

**Why this priority**: These are composite operations that create multiple nodes and links at once, amplifying the blocking effect. They are less frequent than individual node/link edits but cause the longest stalls when they do occur.

**Independent Test**: Can be tested by copying a group of 20+ nodes and pasting them into a function. Delivers immediate visual feedback with background processing.

**Acceptance Scenarios**:

1. **Given** the user pastes a group of copied nodes, **When** the paste is processed, **Then** the new nodes appear in the graph immediately and background processing handles type inference and recompilation.

2. **Given** the user selects a group of nodes and triggers extract-to-function, **When** the extraction begins, **Then** the new function node replaces the selected group immediately in the UI, with code generation and compilation proceeding in the background.

---

### Edge Cases

- What happens when the user adds a node while a previous structural change is still being processed in the background? The new change is coalesced with or queued after the in-flight work; the UI never blocks.
- What happens when a background type inference discovers a type error (e.g., incompatible link)? The error is reported visually on the affected nodes/links once inference completes, without blocking.
- What happens when the user triggers undo while background processing from the original edit is still in flight? The in-flight work is cancelled, the assembly is restored to the undo snapshot, and a new background update is triggered for the restored state.
- What happens when rapid node additions outpace background processing? The system coalesces pending structural updates so that only the latest graph state is compiled, skipping intermediate states.
- What happens during a paste of nodes that reference resources not yet loaded? Resource loading is out of scope for this feature; existing resource loading behavior is unaffected.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST complete all UI-visible effects of node addition (node appears in graph, ports visible) within a single frame, deferring type inference, parameter registration, and compilation to background processing. (Undo snapshot is captured synchronously on the UI thread per FR-009.)
- **FR-002**: System MUST complete all UI-visible effects of link creation and deletion (link drawn/removed) within a single frame, deferring type propagation and recompilation to background processing.
- **FR-003**: System MUST complete all UI-visible effects of node deletion (nodes and their links removed from the visual graph) within a single frame, deferring assembly cleanup and recompilation to background processing.
- **FR-004**: System MUST move `Model::updateTypes()` off the UI thread for structural changes so that type inference does not block rendering.
- **FR-005**: System MUST move `Assembly::updateInputsAndOutputs()` off the UI thread for structural changes.
- **FR-006**: System MUST move `Document::updateParameterRegistration()` off the UI thread for structural changes.
- **FR-007**: System MUST coalesce multiple rapid structural changes into a single background update cycle when changes arrive faster than the background can process them.
- **FR-008**: System MUST cancel in-flight background compilation and restart with the latest graph state when a new structural change arrives, ensuring the most recent graph state is always what gets compiled.
- **FR-009**: System MUST capture the undo snapshot immediately on the UI thread before dispatching background work, so that undo/redo always restores a valid pre-edit state. When undo is triggered while background processing is in flight, the in-flight work MUST be cancelled and a new background update triggered for the restored state.
- **FR-010**: System MUST display stale or default type annotations on node ports immediately after a structural edit, then silently update them to correct types once background type inference completes — without any UI stall or visual "pending" indicator. Existing compilation progress feedback (e.g., status bar) continues to indicate when background compilation is active, satisfying Constitution VI's progress feedback requirement for operations exceeding ~100 ms.
- **FR-011**: System MUST maintain data integrity of the Assembly when it is read by the background worker concurrently with UI thread modifications, either through snapshot copies or appropriate synchronization.
- **FR-012**: System MUST log background processing failures and display error state on affected nodes without crashing or freezing the UI. The graph remains in its current visual state when background processing fails.
- **FR-013**: System MUST respect the existing auto-compile toggle for structural edits. When auto-compile is disabled, structural edits update the graph visually but do not trigger background compilation until the user manually compiles.

### Key Entities

- **Graph Edit**: A discrete structural modification to the node graph (node add/delete, link create/delete, paste, extract-to-function). The unit of work that triggers background processing.
- **Structural Update Pipeline**: The sequence of operations triggered by a graph edit: type inference, I/O update, parameter registration, flat assembly generation, code generation, compilation, SDF precomputation, bounding box update.
- **Edit Epoch**: A monotonically increasing counter that tags each structural edit. Background work compares its epoch against the latest to detect staleness and self-cancel.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Adding a node to a model with 100+ nodes completes the UI-visible portion (node appears) in under 16 ms (one frame at 60 fps).
- **SC-002**: Creating a link between two nodes in a 100+ node model completes the UI-visible portion in under 16 ms.
- **SC-003**: Deleting 10 selected nodes simultaneously completes the UI-visible portion in under 16 ms.
- **SC-004**: The graph editor maintains at least 30 fps during rapid structural edits (5+ changes per second) on a model with 100+ nodes.
- **SC-005**: Background compilation triggered by a structural edit starts within 100 ms of the edit (excluding time spent waiting for a previous compilation to cancel).
- **SC-006**: Undo of a structural change restores the graph to its prior visual state within one frame, regardless of background processing state.

## Assumptions

- The existing `refreshModelAsync()` / `refreshWorker()` infrastructure provides a proven pattern for offloading compilation work to a background thread. This feature extends that pattern to cover the pre-compilation steps (`updateTypes`, `updateInputsAndOutputs`, `updateParameterRegistration`).
- The Assembly data structure can either be snapshotted cheaply for background processing or protected with fine-grained synchronization. The current undo system already performs deep copies, suggesting snapshots are feasible.
- The epoch-based cancellation pattern used by `AsyncRenderController` can be extended to structural update pipeline work.
- ImGui node rendering can display nodes with stale type information while background type inference is running. Types update silently once inference completes.
- Undo snapshots are captured on the UI thread immediately before dispatching background work, ensuring a consistent pre-edit state is always available for undo.
- The existing auto-compile toggle governs whether structural edits trigger background compilation. Manual-compile mode defers compilation until user request.

## Dependencies

- Spec `010-non-blocking-model-updates` — this feature builds on the async parameter update and streaming preview infrastructure established there.
- Existing `Document::refreshWorker()` background compilation pipeline.
- `AsyncRenderController` coroutine and epoch-based cancellation system.
- ImGui node editor rendering in `NodeView` and `ModelEditor`.

## Out of Scope

- Parameter-only changes (already handled by spec 010 with streaming preview).
- GPU rendering performance optimizations (ray marching, SDF evaluation speed).
- File I/O operations (load/save).
- Multi-GPU or distributed compute support.
- Changes to the OpenCL code generation algorithms themselves.
- Resource loading state display for pasted nodes referencing unloaded resources (existing behavior preserved).

## Clarifications

### Session 2026-04-15

- Q: What should node ports display during background type inference after a structural edit? → A: Show stale/default types; silently update once inference completes (no pending indicator).
- Q: When a new structural edit arrives during an active compilation, should the system cancel+restart or queue? → A: Cancel the current compilation and restart with the latest graph state.
- Q: When is the undo snapshot captured relative to background processing? → A: Immediately on the UI thread before dispatching background work, ensuring undo always has a valid pre-edit state.
- Q: How should the system handle background processing failures (e.g., invalid assembly, OOM)? → A: Log the error, show error state on affected nodes, keep graph in its current visual state without crashing.
- Q: Should the existing auto-compile toggle apply to structural edits? → A: Yes, respect the toggle. When disabled, structural edits update visually but don't trigger background compilation until manual compile.
