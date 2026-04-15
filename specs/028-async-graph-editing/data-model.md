# Data Model: Async Graph Editing

**Feature**: 028-async-graph-editing  
**Date**: 2026-04-15

## Entities

### StructuralEditEpoch

A monotonically increasing counter that uniquely identifies each structural graph edit.

| Field | Type | Description |
|-------|------|-------------|
| value | `std::atomic<uint64_t>` | Current epoch counter, incremented on each structural edit |

**Relationships**: Referenced by `StructuralUpdateJob` to detect staleness. Compared by background worker's `CancelCheck` lambda.

**State transitions**: Only increments. Never decremented or reset (except on file load which resets all state).

---

### StructuralUpdateJob

A unit of background work triggered by one or more coalesced structural edits.

| Field | Type | Description |
|-------|------|-------------|
| epoch | `uint64_t` | Epoch at time of dispatch (snapshot of `StructuralEditEpoch`) |
| assemblySnapshot | `std::shared_ptr<Assembly>` | Deep copy of Assembly at dispatch time |
| cancelCheck | `std::function<bool()>` | Lambda that returns `true` if epoch is stale |

**Relationships**: Produced by UI thread dispatch logic. Consumed by background worker. Results published back via `StructuralUpdateResult`.

**Lifecycle**:
1. Created when debounce timer fires and `m_structuralChangePending` is true
2. Assembly snapshot taken (deep copy)
3. Dispatched to background worker (via `std::async` or coroutine)
4. Worker performs: validate → updateTypes → updateInputsAndOutputs → updateParameterRegistration → flatten → code gen → compile → SDF → bbox
5. On completion, publishes `StructuralUpdateResult`
6. On cancellation (stale epoch), exits early and discards partial results

---

### StructuralUpdateResult

The output of a completed background structural update, ready for the UI thread to consume.

| Field | Type | Description |
|-------|------|-------------|
| epoch | `uint64_t` | Epoch this result corresponds to |
| updatedAssembly | `std::shared_ptr<Assembly>` | Fully processed Assembly with resolved types, validated state |
| compilationSuccess | `bool` | Whether OpenCL compilation succeeded |
| validationIssues | `IssueList` | Any validation errors/warnings discovered |

**Relationships**: Produced by background worker. Consumed by UI thread in frame update loop.

**Consumption**: UI thread checks for available result each frame. If `result.epoch == currentEpoch`, the result is applied (Assembly swapped, types refreshed, validation issues displayed). If `result.epoch < currentEpoch`, the result is discarded (superseded by a newer edit).

---

### StructuralEditDebouncer

Controls the timing of background dispatch to coalesce rapid edits.

| Field | Type | Description |
|-------|------|-------------|
| pending | `bool` | Whether a structural edit is awaiting dispatch |
| lastEditTime | `std::chrono::steady_clock::time_point` | Time of most recent structural edit |
| debounceDelay | `std::chrono::milliseconds` | Delay before dispatching (e.g., 50 ms) |

**Relationships**: Set by UI thread on structural edit. Checked each frame by `MainWindow::updateModel()` or equivalent.

**Behavior**: When `pending && (now - lastEditTime >= debounceDelay)`, dispatch a `StructuralUpdateJob` and clear `pending`.

---

### Assembly (existing, modified)

Extended with snapshot support.

| New Field | Type | Description |
|-----------|------|-------------|
| (none — uses existing copy constructor) | | Deep copy via `Assembly(const Assembly&)` already exists |

**Key change**: The copy constructor is already functional (used by undo). No new fields needed. The snapshot is a `std::shared_ptr<Assembly>` created by the dispatch logic.

---

### History (existing, modified)

Extended for async-safe operation.

| Existing Field | Change |
|----------------|--------|
| storeState(Assembly const&) | Undo snapshot captured on UI thread immediately before edit (no change to timing) |

**Key change**: The undo snapshot is still captured synchronously on the UI thread. The *background worker snapshot* is a separate copy created at dispatch time. These are two independent copies with different lifetimes:
- Undo snapshot: owned by History, restored on undo
- Worker snapshot: owned by StructuralUpdateJob, discarded after worker completes

---

## Data Flow Diagram

```
UI Thread                                  Background Worker
─────────                                  ─────────────────
                                           
User edit (add node/link/etc.)             
  │                                        
  ├─ Apply visual edit to m_assembly       
  ├─ Capture undo snapshot (History)       
  ├─ Increment m_structuralEditEpoch       
  ├─ Set debouncer.pending = true          
  │                                        
  ... (next frames) ...                    
  │                                        
  ├─ Debounce fires                        
  │   ├─ Deep copy m_assembly → snapshot   
  │   ├─ Create StructuralUpdateJob        
  │   └─ Dispatch to background ─────────► Worker receives job
  │                                          │
  │                                          ├─ validateAssembly(snapshot)
  │                                          ├─ updateInputsAndOutputs(snapshot)
  │                                          ├─ updateParameterRegistration(snapshot)
  │                                          ├─ updateTypes(snapshot)
  │                                          ├─ updateFlatAssembly(snapshot)
  │                                          ├─ refreshProgram(snapshot)
  │                                          ├─ recompileIfRequired()
  │                                          ├─ precomputeSdf()
  │                                          ├─ updateBBox()
  │                                          └─ Publish StructuralUpdateResult
  │                                                │
  ├─ processStructuralUpdateResult() ◄─────────────┘
  │   ├─ Check epoch freshness
  │   ├─ Swap updated types into m_assembly
  │   ├─ Apply validation issues
  │   └─ Trigger render invalidation
  │
  └─ Continue rendering with updated state
```

## Validation Rules

- `StructuralEditEpoch` MUST only be incremented on the UI thread.
- `StructuralUpdateResult` MUST only be consumed on the UI thread.
- Background worker MUST NOT write to `m_assembly` — it operates on its own snapshot.
- Undo snapshot MUST be captured before the edit is applied to `m_assembly`.
- If `result.epoch < m_structuralEditEpoch`, the result MUST be discarded.
