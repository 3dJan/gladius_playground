# Research: Node Editor UX Improvements (023)

## Decision 1: Use a shared inline pin interaction model for all node types

**Decision**: Compact nodes and regular nodes will share the same pin interaction model: inline layout, consistent hit target sizing, identical drag-start semantics, and identical hover/tooltip behavior.

**Rationale**: The observed clipping, anchor misalignment, and unreliable drag behavior all stem from diverging interaction geometry between node styles. A unified pin primitive removes those inconsistencies and lets polish come from style rather than from special-case geometry.

**Alternatives considered**:
- Perimeter-mounted compact pins with custom hit regions — rejected because interaction quality degrades and the design becomes fragile.
- Separate pin widgets for compact vs. regular nodes — rejected because it guarantees behavioral drift.

## Decision 2: Compact nodes should be rounded/capsule first, not truly circular

**Decision**: The first polished implementation will use heavily rounded or capsule-like compact nodes with aligned left/right pin rails. True circular nodes are deferred.

**Rationale**: Rounded/capsule nodes preserve a strong compact silhouette while staying compatible with the framework’s natural layout and link-anchor behavior. This aligns with the updated spec and avoids spending effort on geometry experiments before the baseline UX is reliable.

**Alternatives considered**:
- True circular nodes with perimeter pins in the initial pass — rejected due to repeated clipping, drag, and anchor regressions.
- Plain rectangular nodes only — rejected because the spec still calls for a more polished and distinctive visual language.

## Decision 3: Numeric editing belongs in extracted, reusable UI helpers

**Decision**: Numeric parameter interaction will be handled by dedicated UI helper modules for dial + drag-float + slider presentation and not embedded ad hoc inside each node renderer.

**Rationale**: Numeric editing is a cross-cutting behavior used by multiple node types. Extracting it reduces duplication, keeps `NodeView` from growing further, and makes unit-testing sensitivity, bounds, and keyboard behavior practical.

**Alternatives considered**:
- Keep all numeric behavior inline inside `NodeView.cpp` — rejected because the file is already large and difficult to reason about.
- Build a separate modal/property-editor-only experience — rejected because the spec requires inline node editing.

## Decision 4: Port compatibility should be derived from current model state and reused per drag session

**Decision**: Compatibility highlighting will use model-derived type/direction rules captured in a drag-session state object and reused while the same drag source remains active.

**Rationale**: This keeps the compatibility signal accurate for dynamic types while avoiding scattered, inconsistent compatibility checks across rendering code. It also supports one-frame feedback without frame-by-frame recomputation everywhere.

**Alternatives considered**:
- Purely visual/static compatibility tables — rejected because they do not reflect dynamic typing.
- Recompute compatibility independently inside each rendering branch every frame — rejected as harder to maintain and reason about.

## Decision 5: Responsiveness requires explicit coalescing between UI edits and expensive recompute work

**Decision**: Parameter widgets update their displayed values immediately, while recompilation/re-render triggers are throttled/coalesced through a dedicated responsiveness boundary.

**Rationale**: The spec’s responsiveness requirements cannot be met if expensive work is tied directly to every drag sample. Separating immediate UI state from deferred recompute requests preserves tactile editing and uses the existing async pipeline more effectively.

**Alternatives considered**:
- Recompute on every parameter change event — rejected because it risks visible stutter.
- Add a new heavyweight async framework — rejected because the existing architecture already supports async work; this feature only needs a disciplined trigger boundary.

## Decision 6: Begin/end node editing should remain inline, with discoverable controls and reliable reorder semantics

**Decision**: Begin/end node signature editing will remain inline within the node graph and use explicit row-level controls for add/rename/remove/reorder behaviors.

**Rationale**: Signature editing is part of graph authoring, so pushing it into a separate dialog would reduce discoverability and break flow. The design should remain table-oriented but become clearer and more robust.

**Alternatives considered**:
- Separate modal editor for function signatures — rejected because it disconnects the signature from the graph context.
- Pure drag-and-drop without visible affordances — rejected because it is less discoverable for infrequent users.
