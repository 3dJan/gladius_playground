# Tasks: Deduplicate Functionally Identical Functions

**Input**: Design documents from `/specs/014-dedupe-functions/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Unit tests are REQUIRED by the specification (FR-013).

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story (US1, US2, US3) - only for story-specific tasks

---

## Phase 1: Setup

**Purpose**: Create new source files and test infrastructure

- [X] T001 [P] Create `FunctionalEquality.h` header in `gladius/src/nodes/FunctionalEquality.h`
- [X] T002 [P] Create `FunctionalEquality.cpp` implementation stub in `gladius/src/nodes/FunctionalEquality.cpp`
- [X] T003 [P] Create `FunctionDeduplicator.h` header in `gladius/src/nodes/FunctionDeduplicator.h`
- [X] T004 [P] Create `FunctionDeduplicator.cpp` implementation stub in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T005 [P] Create `FunctionalEquality_tests.cpp` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T006 [P] Create `FunctionDeduplicator_tests.cpp` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T007 Update `gladius/src/nodes/CMakeLists.txt` to include new source files (GLOB_RECURSE auto-discovers)
- [X] T008 Update `gladius/tests/unittests/CMakeLists.txt` to include new test files (GLOB_RECURSE auto-discovers)

**Checkpoint**: ✅ Project builds successfully with stub files, 607 tests pass

---

## Phase 2: Foundational (Core Comparison Algorithm)

**Purpose**: Implement `FunctionalEquality` - MUST complete before any user story

**⚠️ CRITICAL**: Deduplication cannot work without functional equality comparison

### Unit Tests (Write First, Ensure They Fail)

- [X] T009 [P] Add test `FunctionalEquality_ComputeHash_IdenticalModels_ReturnsSameHash` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T010 [P] Add test `FunctionalEquality_ComputeHash_DifferentModels_ReturnsDifferentHash` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T011 [P] Add test `FunctionalEquality_AreEqual_IdenticalStructure_ReturnsTrue` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T012 [P] Add test `FunctionalEquality_AreEqual_DifferentConstants_ReturnsFalse` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T013 [P] Add test `FunctionalEquality_AreEqual_DifferentNodeTypes_ReturnsFalse` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T014 [P] Add test `FunctionalEquality_AreEqual_DifferentNodeNames_ReturnsTrue` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T015 [P] Add test `FunctionalEquality_AreEqual_EmptyModels_ReturnsTrue` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [X] T016 [P] Add test `FunctionalEquality_AreEqual_ConstantsWithinEpsilon_ReturnsTrue` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`

### Implementation

- [X] T017 Implement `FunctionalEquality::computeHash()` in `gladius/src/nodes/FunctionalEquality.cpp`
- [X] T018 Implement helper `compareNodeTypes()` in `gladius/src/nodes/FunctionalEquality.cpp`
- [X] T019 Implement helper `compareConstants()` with epsilon tolerance in `gladius/src/nodes/FunctionalEquality.cpp`
- [X] T020 Implement helper `compareTopology()` for graph connectivity in `gladius/src/nodes/FunctionalEquality.cpp`
- [X] T021 Implement `FunctionalEquality::areEqual()` using helpers in `gladius/src/nodes/FunctionalEquality.cpp`

**Checkpoint**: ✅ All `FunctionalEquality` tests pass (10 tests)

---

## Phase 3: User Story 1 - Clean Up Duplicate Functions (Priority: P1) 🎯 MVP

**Goal**: User can click a button to remove duplicate functions; all references updated

**Independent Test**: Load 3MF with duplicates, click deduplicate, verify only unique functions remain

### Unit Tests (Write First)

- [X] T022 [P] [US1] Add test `FunctionDeduplicator_FindDuplicateGroups_NoDuplicates_ReturnsEmpty` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T023 [P] [US1] Add test `FunctionDeduplicator_FindDuplicateGroups_OnePair_ReturnsOneGroup` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T024 [P] [US1] Add test `FunctionDeduplicator_FindDuplicateGroups_MultiplePairs_ReturnsMultipleGroups` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T025 [P] [US1] Add test `FunctionDeduplicator_FindDuplicateGroups_AssemblyModelExcluded_NeverInGroup` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T026 [P] [US1] Add test `FunctionDeduplicator_Deduplicate_RemovesDuplicates_ReturnsCorrectCount` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T027 [P] [US1] Add test `FunctionDeduplicator_Deduplicate_UpdatesFunctionCallReferences` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T028 [P] [US1] Add test `FunctionDeduplicator_Deduplicate_UpdatesFunctionGradientReferences` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`

### Implementation

- [X] T029 [US1] Implement `FunctionDeduplicator::findDuplicateGroups()` in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T030 [US1] Implement `FunctionDeduplicator::countReferences()` helper in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T031 [US1] Implement `FunctionDeduplicator::updateInternalReferences()` in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T032 [US1] Implement `FunctionDeduplicator::deduplicate()` (no history overload) in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T033 [US1] Add "Remove duplicate functions" menu item in `gladius/src/ui/ModelEditor.cpp` outline() method
- [X] T034 [US1] Add user feedback (event log message) for deduplication result in `gladius/src/ui/ModelEditor.cpp`

**Checkpoint**: User can click button, duplicates removed, references updated, tests pass ✓

---

## Phase 4: User Story 2 - Preserve Most-Referenced Function (Priority: P2)

**Goal**: When duplicates exist, keep the function with highest reference count

**Independent Test**: Create duplicates with different reference counts, verify correct one retained

### Unit Tests (Write First)

- [N/A] T035 [P] [US2] Add test `FunctionDeduplicator_SelectCanonical_ExternalRef_SelectsExternallyReferenced` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp` (Skipped: MVP only handles internal refs)
- [X] T036 [P] [US2] Add test `FunctionDeduplicator_SelectCanonical_NoExternalRefs_HigherInternalCount_SelectsHigher` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`
- [X] T037 [P] [US2] Add test `FunctionDeduplicator_SelectCanonical_EqualRefCounts_SelectsLowerResourceId` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`

### Implementation

- [N/A] T038 [US2] Implement `FunctionDeduplicator::hasExternalReferences()` helper in `gladius/src/nodes/FunctionDeduplicator.cpp` (Skipped: MVP only handles internal refs)
- [X] T039 [US2] Implement `FunctionDeduplicator::selectCanonical()` with internal ref priority in `gladius/src/nodes/FunctionDeduplicator.cpp`
- [X] T040 [US2] Integrate `selectCanonical()` into `deduplicate()` flow in `gladius/src/nodes/FunctionDeduplicator.cpp`

**Checkpoint**: Deduplication always keeps function with most references (or lowest ID on tie)

---

## Phase 5: User Story 3 - Undo Deduplication (Priority: P3)

**Goal**: Deduplication can be undone via Ctrl+Z

**Independent Test**: Perform deduplication, press undo, verify all functions restored

### Unit Tests (Write First)

- [X] T041 [P] [US3] Add test `FunctionDeduplicator_Deduplicate_WithHistory_CanUndo` in `gladius/tests/unittests/FunctionDeduplicator_tests.cpp`

### Implementation

- [X] T042 [US3] N/A - Used existing pattern (storeState before deduplicate)
- [X] T043 [US3] Integrate History::storeState() before deduplication in `gladius/src/ui/ModelEditor.cpp`

**Checkpoint**: Undo fully restores assembly state after deduplication ✅

---

## Phase 6: Polish & Edge Cases

**Purpose**: Handle edge cases and finalize implementation

- [ ] T044 [P] Add test `FunctionalEquality_AreEqual_SelfReferencingFunction_HandledCorrectly` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [ ] T045 [P] Add test `FunctionalEquality_AreEqual_CircularReferences_HandledCorrectly` in `gladius/tests/unittests/FunctionalEquality_tests.cpp`
- [ ] T046 [P] Add Doxygen documentation to `FunctionalEquality.h`
- [ ] T047 [P] Add Doxygen documentation to `FunctionDeduplicator.h`
- [ ] T048 Run all tests via "Run Gladius Tests" task and verify pass
- [ ] T049 Manual test: Load real 3MF with duplicates, verify UI workflow

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup) ─────────────────────────────────────────┐
                                                          │
Phase 2 (Foundational: FunctionalEquality) ◄─────────────┘
     │
     │ ⚠️ BLOCKS all user stories
     ▼
┌────────────────┬────────────────┬────────────────┐
│ Phase 3 (US1)  │ Phase 4 (US2)  │ Phase 5 (US3)  │
│ Core Dedup     │ Smart Select   │ Undo Support   │
│ 🎯 MVP         │                │                │
└───────┬────────┴───────┬────────┴───────┬────────┘
        │                │                │
        └────────────────┴────────────────┘
                         │
                         ▼
                  Phase 6 (Polish)
```

### Within Each Phase

- Tests MUST be written and FAIL before implementation
- Header before implementation
- Helpers before main functions
- Core logic before UI integration

### Parallel Opportunities

**Phase 1** (all parallel):
```
T001, T002, T003, T004, T005, T006 → then T007, T008
```

**Phase 2 Tests** (all parallel):
```
T009, T010, T011, T012, T013, T014, T015, T016
```

**Phase 3 Tests** (all parallel):
```
T022, T023, T024, T025, T026, T027, T028
```

**Across Phases** (after Phase 2):
```
US1 (T022-T034), US2 (T035-T040), US3 (T041-T043) can run in parallel
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T008)
2. Complete Phase 2: FunctionalEquality (T009-T021)
3. Complete Phase 3: User Story 1 (T022-T034)
4. **STOP and VALIDATE**: Click button, verify duplicates removed
5. Demo/merge if acceptable

### Full Feature

1. MVP + Phase 4 (US2: Smart selection)
2. + Phase 5 (US3: Undo support)
3. + Phase 6 (Polish, edge cases)

---

## Notes

- All new files go in `gladius/src/nodes/` (core logic) and `gladius/tests/unittests/` (tests)
- Follow existing code style: Allman braces, 4-space indent, `m_` prefix for members
- Use `[[nodiscard]]` on all query methods
- Use existing `History` class for undo (no new infrastructure needed)
- Commit after each task or logical group of [P] tasks
