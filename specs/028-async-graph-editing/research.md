# Research: Async Graph Editing

**Feature**: 028-async-graph-editing  
**Date**: 2026-04-15

## R-001: Can Assembly be snapshotted cheaply for background processing?

**Decision**: Yes — use the existing Assembly copy constructor (same mechanism as undo).

**Rationale**: The undo system already performs deep copies of Assembly via `History::storeState(*m_assembly)`. The copy constructor chain (`Assembly → Model → Node::clone()`) produces a fully independent copy suitable for background reading. The cost (2–30 ms for large models) is acceptable when moved off the UI thread since it becomes the background worker's problem, not the UI's.

**Alternatives considered**:
- **Fine-grained locking on Model/Node**: Rejected — Model and Node have no internal locking, and adding mutexes to every graph operation would be invasive, error-prone, and likely slower than a snapshot for the read-heavy background pipeline.
- **Copy-on-write (COW) Assembly**: Rejected — significant new infrastructure for marginal benefit. The undo system proves full copies work. COW could be revisited later if snapshot cost becomes a bottleneck.

## R-002: What synchronization is needed between UI edits and the background worker?

**Decision**: Snapshot-based isolation. The UI thread creates a snapshot (Assembly deep copy) and hands it to the background worker. No concurrent access to the same Assembly instance.

**Rationale**: The existing `refreshWorker()` already operates on `m_assembly` by reference but relies on implicit serialization (one worker at a time, UI doesn't modify Assembly while worker runs). With async graph editing, the UI *will* modify Assembly while the worker runs, so the worker must operate on an independent snapshot.

**Key insight**: `refreshWorker()` currently does NOT acquire `m_assemblyMutex` — it relies on the `std::future` + compute token for serialization. The new pattern must use explicit snapshot isolation instead.

**Alternatives considered**:
- **Acquire `m_assemblyMutex` in refreshWorker**: Rejected — would block either UI or worker, defeating the purpose.
- **Message-passing queue for all edits**: Rejected — would require rewriting the entire ModelEditor/NodeView interaction model. Too invasive.

## R-003: How should the undo snapshot be captured without blocking the UI?

**Decision**: Capture undo snapshot on the UI thread immediately before the edit, using the existing full-Assembly copy mechanism (`History::storeState`). The background worker snapshot (separate from undo) is a second full Assembly copy created at dispatch time.

**Rationale**: The current system copies the entire Assembly for every undo point via `History::storeState(*m_assembly)`. This mechanism is proven and already functional. A per-Model optimization could reduce UI-thread copy cost from O(Assembly) to O(Model), but introduces significant complexity (History must track partial snapshots, undo must reconstruct full Assembly from partial + base). The full-Assembly approach is simpler, matches the existing code, and the copy cost (2–30 ms) is acceptable for the UI thread given that structural edits are infrequent (not every frame).

**Alternatives considered**:
- **Per-Model undo snapshot**: Deferred — would reduce copy cost but adds complexity. Can be revisited as a follow-up optimization if profiling shows full-Assembly copy is a bottleneck for undo.
- **Background undo snapshot**: Rejected — the spec requires undo to always have a valid pre-edit state. Capturing after the edit would snapshot the wrong state.

## R-004: How should coalescing of rapid structural edits work?

**Decision**: Dirty-flag + debounce pattern. Each structural edit sets a `m_structuralChangePending` flag and increments an atomic `m_structuralEditEpoch`. A debounce timer (similar to the existing parameter throttle) triggers background dispatch after a short delay (e.g., 50 ms). If a new edit arrives during the delay, the timer resets and the epoch increments again. The background worker receives the latest epoch and checks it against the current epoch before committing results.

**Rationale**: This matches the existing `m_parameterThrottle` pattern for parameter changes. It naturally coalesces rapid edits (e.g., 5 node additions in 2 seconds) into fewer background cycles while keeping latency low for isolated edits.

**Alternatives considered**:
- **Immediate dispatch with cancel+restart**: Higher latency overhead from repeated worker startup/cancellation. Acceptable for infrequent edits but wasteful for rapid sequences.
- **Queue-based batching**: More complex, no clear advantage over dirty flag + debounce for the expected edit rates.

## R-005: How should in-flight compilation be cancelled when a new structural edit arrives?

**Decision**: Extend the existing epoch-based cancellation from `AsyncRenderController` to the structural update pipeline. The `refreshWorker()` receives a `CancelCheck` function that compares the worker's epoch against `m_structuralEditEpoch`. If stale, the worker exits early.

**Rationale**: The `AsyncRenderController` already uses `std::atomic<uint64_t>` epochs with CAS-based updates and `CancelCheck` lambdas. Reusing this pattern for structural updates keeps the codebase consistent and avoids inventing new cancellation mechanisms.

**Implementation detail**: `refreshWorker()` currently polls `isCompilationInProgress()` in a sleep loop. The epoch check can be inserted at each polling iteration, plus at key checkpoints (after validation, after flattening, after code generation).

**Alternatives considered**:
- **Cooperative cancellation via `std::stop_token`**: C++20 feature, but libcoro doesn't use it; mixing mechanisms adds complexity.
- **Thread interruption**: Unsafe and non-portable.

## R-006: What happens to type display during the background processing interval?

**Decision**: Node ports display their last known types (stale but valid). Once background type inference completes, the UI thread reads the updated types on the next frame and silently refreshes port annotations.

**Rationale**: The `m_typesRequireUpdate` flag already controls when `updateTypes()` runs. After a structural edit, the flag is set but the update is deferred to the background. The UI simply renders whatever type info is currently cached in the Model. When the background worker completes, it publishes updated type info that the UI picks up.

**Communication mechanism**: The background worker writes its results (updated Assembly with resolved types) to a thread-safe result slot. The UI thread checks this slot each frame (similar to `processAsyncResults()` in RenderWindow) and swaps in the updated Assembly.

## R-007: How does `refreshModelIfNoCompilationIsRunning()` guard interact with the new async pattern?

**Decision**: The guard remains but is augmented. Currently it returns `false` if any OpenCL program is compiling, causing the edit to retry next frame. With the new pattern, the guard is checked by the background worker (not the UI thread). If compilation is in progress, the new worker either waits (on the background thread, not UI) or cancels the in-progress compilation via epoch invalidation.

**Rationale**: The guard protects against concurrent OpenCL state modification, which is still necessary. But the check must not happen on the UI thread since that would re-introduce blocking.

## R-008: How to handle the `updateInputsAndOutputs()` / `updateParameterRegistration()` duplication?

**Decision**: Remove the synchronous calls from `MainWindow::nodeEditor()` (UI thread). Keep only the calls in `refreshWorker()` (background thread). The UI thread only performs the minimal visual edit (add/remove node/link from the Model data structure) and defers all derived state updates to the background.

**Rationale**: These operations already run in `refreshWorker()` — the UI thread calls are redundant but were kept for immediate correctness. With the snapshot approach, the background worker operates on a fresh copy and performs all derived state updates there.

**Risk**: Between the edit and the background completion, the UI Assembly has inconsistent derived state (ports may reference stale types, parameter registry may be incomplete). This is acceptable because: (1) ImGui rendering tolerates stale types (FR-010), (2) parameter values don't change during structural edits, (3) the background worker produces a fully consistent Assembly that replaces the stale one.
