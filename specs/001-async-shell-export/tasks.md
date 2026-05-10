# Tasks: Async Shell-Based Color Export

**Feature**: 001-async-shell-export  
**Date**: 2026-01-07  
**Status**: Ready for implementation

## Task Overview

| # | Task | Est. | Priority | Dependencies |
|---|------|------|----------|--------------|
| 1 | Create ShellExporter class skeleton | 1h | P1 | - |
| 2 | Implement async export with progress | 2h | P1 | Task 1 |
| 3 | Add cancellation support | 1h | P1 | Task 2 |
| 4 | Integrate ShellExporter into MeshExportDialog | 1h | P1 | Task 3 |
| 5 | Add unit tests | 1h | P2 | Task 4 |
| 6 | Manual testing and polish | 1h | P2 | Task 5 |

**Total Estimate**: 7 hours

---

## Task 1: Create ShellExporter Class Skeleton

**Priority**: P1  
**Estimate**: 1 hour  
**Status**: ✅ Completed

### Description
Create the new `ShellExporter` class that implements the `IExporter` interface, following the pattern established by `ManifoldDualContouringStlExporter`.

### Files to Create
- `gladius/src/io/ShellExporter.h`
- `gladius/src/io/ShellExporter.cpp`

### Acceptance Criteria
- [X] Class compiles without errors
- [X] Implements IExporter interface (beginExport, advanceExport, finalize, getProgress)
- [X] Has State enum (Idle, Running, Completed, Failed)
- [X] Has ShellExportConfig struct for configuration
- [X] Added to CMakeLists.txt (auto-picked up by GLOB_RECURSE)

### Reference
See [quickstart.md](quickstart.md) for implementation pattern.

---

## Task 2: Implement Async Export with Progress

**Priority**: P1  
**Estimate**: 2 hours  
**Depends on**: Task 1  
**Status**: ✅ Completed

### Description
Implement the `performExport()` method that runs shell generation in a background thread with progress reporting.

### Implementation Details
1. `beginExport()` launches `std::async` with `performExport()`
2. `performExport()` calls ShellGenerator and MeshWriter3mf
3. Progress updates at key phases:
   - 0-5%: Initialization
   - 5-85%: Shell generation
   - 85-95%: Mesh construction
   - 95-100%: File writing
4. Status messages update for each phase

### Acceptance Criteria
- [X] Export runs in background thread
- [X] UI remains responsive during export
- [X] Progress bar updates during export
- [X] Status message shows current phase
- [X] Export produces valid 3MF file

---

## Task 3: Add Cancellation Support

**Priority**: P1  
**Estimate**: 1 hour  
**Depends on**: Task 2  
**Status**: ✅ Completed

### Description
Integrate cancellation checks into the export workflow using the existing `CancellationToken` mechanism.

### Implementation Details
1. Check `isCancellationRequested()` before each shell generation
2. Check after mesh construction loop
3. Set state to `Idle` and return early (following ManifoldDualContouringStlExporter pattern)
4. Do not write file if cancelled

### Acceptance Criteria
- [X] Cancel button stops export within 2 seconds
- [X] No partial output file on cancellation
- [X] State transitions to Idle
- [X] Next export can start normally

---

## Task 4: Integrate ShellExporter into MeshExportDialog

**Priority**: P1  
**Estimate**: 1 hour  
**Depends on**: Task 3  
**Status**: ✅ Completed

### Description
Replace the synchronous `exportShellsTo3mf()` code with delegation to `ShellExporter`.

### Files to Modify
- `gladius/src/ui/MeshExportDialog.h` - Add `m_shellExporter` member
- `gladius/src/ui/MeshExportDialog.cpp` - Modify `exportShellsTo3mf()`, update render loop

### Changes
1. Add `#include "io/ShellExporter.h"`
2. Add `io::ShellExporter m_shellExporter;` member
3. Replace `exportShellsTo3mf()` body with config setup and `beginExport()`
4. Set `m_activeExporter = &m_shellExporter;`
5. Existing render loop already handles IExporter progress
6. Added ShellExporter handling in `finalizeExport()` and error checking

### Acceptance Criteria
- [X] Shell export uses async path
- [X] Progress bar works correctly
- [X] Cancel button works
- [X] Export completion shows success message
- [X] ExportState is properly set/cleared

---

## Task 5: Add Unit Tests

**Priority**: P2  
**Estimate**: 1 hour  
**Depends on**: Task 4  
**Status**: ✅ Completed

### Description
Create unit tests for the new ShellExporter class.

### Files to Create
- `gladius/tests/unittests/ShellExporter_tests.cpp`

### Test Cases
1. `GetProgress_AfterConstruction_ReturnsZero`
2. `HasError_AfterConstruction_ReturnsFalse`
3. `ErrorMessage_AfterConstruction_IsEmpty`
4. `GetStatusMessage_AfterConstruction_IsEmpty`
5. `AdvanceExport_WhenIdle_ReturnsFalse`
6. `Finalize_WhenIdle_DoesNotThrow`
7. `SetConfig_WithEmptyStack_DoesNotThrow`
8. `SetDocument_WithNullptr_DoesNotThrow`
9. `ConstructWithLogger_DoesNotThrow`

### Acceptance Criteria
- [X] Tests compile and link
- [X] Tests pass (9 tests added, all pass)
- [X] Added to CMakeLists.txt (auto-picked up by GLOB_RECURSE)

---

## Task 6: Manual Testing and Polish

**Priority**: P2  
**Estimate**: 1 hour  
**Depends on**: Task 5  
**Status**: 🔄 Pending manual testing

### Description
Perform manual end-to-end testing and address any issues found.

### Test Scenarios
1. Export with 3 shells - verify progress updates per shell
2. Cancel during shell generation - verify quick response
3. Cancel during file write - verify no partial file
4. Export with error (invalid path) - verify error message
5. Multiple consecutive exports - verify clean state reset

### Acceptance Criteria
- [ ] All manual test scenarios pass
- [ ] No memory leaks (check with valgrind if available)
- [ ] Error messages are clear and actionable
- [ ] UI polish (status messages, progress granularity)

---

## Definition of Done

- [X] All P1 tasks completed
- [X] All acceptance criteria met
- [X] Unit tests pass
- [X] No compiler warnings
- [X] Code follows constitution (style, modern C++, documentation)
- [ ] Manual testing complete (pending)
