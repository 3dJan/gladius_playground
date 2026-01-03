# Tasks: Test Suite Restructuring

**Input**: Design documents from `/specs/004-test-suite-restructure/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, quickstart.md ✓

**Tests**: No test tasks included - this feature is about reorganizing existing tests, not adding new ones.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Initial project structure for test reorganization

- [ ] T001 Create `gladius/tests/apitests/` directory structure
- [ ] T002 [P] Create `gladius/tests/apitests/CMakeLists.txt` with basic executable setup
- [ ] T003 [P] Create `gladius/tests/apitests/main.cpp` (GTest main entry point)
- [ ] T004 Update `gladius/tests/CMakeLists.txt` to add `apitests` subdirectory

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before user story implementation

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [ ] T005 Add CTest test presets to `gladius/CMakePresets.json`:
  - `UnitTests` preset with label filter for "unit"
  - `IntegrationTests` preset with label filter for "integration" and `GLADIUS_RUN_GPU_TESTS=1`
  - `ApiTests` preset with label filter for "api"
  - `AllTests` preset with all tests enabled
- [ ] T006 [P] Update `gladius/tests/unittests/CMakeLists.txt` to add `PROPERTIES LABELS "unit"` to `gtest_discover_tests()`
- [ ] T007 [P] Update `gladius/tests/integrationtests/CMakeLists.txt` to add `PROPERTIES LABELS "integration"` to `gtest_discover_tests()`
- [ ] T008 [P] Update `gladius/tests/apitests/CMakeLists.txt` to add `PROPERTIES LABELS "api"` to `gtest_discover_tests()`

**Checkpoint**: CTest presets work and tests can be filtered by category label

---

## Phase 3: User Story 1 - Run Fast Unit Tests Quickly (Priority: P1) 🎯 MVP

**Goal**: Unit tests complete in <60 seconds without GPU dependencies

**Independent Test**: Run `ctest --preset UnitTests` and verify all tests complete in <60s with no GPU-related skips

### Implementation for User Story 1

- [ ] T009 [US1] Move `gladius/tests/unittests/ColorExport_Integration_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T010 [P] [US1] Move `gladius/tests/unittests/DualContouringOctree_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T011 [P] [US1] Move `gladius/tests/unittests/DualContouringStlExporter_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T012 [P] [US1] Move `gladius/tests/unittests/GlobalMortonOctree_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T013 [P] [US1] Move `gladius/tests/unittests/HierarchicalDC_CompilationDebug_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T014 [P] [US1] Move `gladius/tests/unittests/HierarchicalDC_ExtractionStep_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T015 [P] [US1] Move `gladius/tests/unittests/HierarchicalDC_STLExport_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T016 [P] [US1] Move `gladius/tests/unittests/HierarchicalDualContouring_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T017 [P] [US1] Move `gladius/tests/unittests/ManifoldDualContouring_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T018 [P] [US1] Move `gladius/tests/unittests/MeshBaseline_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T019 [P] [US1] Move `gladius/tests/unittests/MeshSdfPerformance_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T020 [P] [US1] Move `gladius/tests/unittests/NodeLayoutEngine_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T021 [P] [US1] Move `gladius/tests/unittests/PaletteExtractor_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T022 [P] [US1] Move `gladius/tests/unittests/ShellGenerator_tests.cpp` to `gladius/tests/integrationtests/`
- [ ] T023 [US1] Update `gladius/tests/integrationtests/CMakeLists.txt` to include migrated test files (update file globs or explicit file lists)
- [ ] T024 [US1] Verify unit tests build and run without GPU: `ctest --preset UnitTests`
- [ ] T025 [US1] Measure unit test execution time and confirm <60 seconds

**Checkpoint**: Unit tests run in <60s with no GPU-related skips or failures

---

## Phase 4: User Story 2 - Run Tests by Scope/Component (Priority: P2)

**Goal**: Developers can filter tests by component labels

**Independent Test**: Run `ctest -L mesh-export` and verify only mesh-related tests execute

### Implementation for User Story 2

- [ ] T026 [US2] Create CMake function `add_component_labels()` in `gladius/tests/CMakeLists.txt` for applying labels based on file patterns
- [ ] T027 [US2] Apply `mesh-export` label to test files matching `*Writer3mf*`, `*Exporter*`, `CliWriter*` in `gladius/tests/unittests/CMakeLists.txt`
- [ ] T028 [P] [US2] Apply `dual-contouring` label to test files matching `DualContouring*`, `HierarchicalDC*`, `ManifoldDualContouring*` in `gladius/tests/integrationtests/CMakeLists.txt`
- [ ] T029 [P] [US2] Apply `mcp` label to test files matching `MCP_*`, `ApplicationMCPAdapter*`, `JSONRPC*` 
- [ ] T030 [P] [US2] Apply `io` label to test files matching `CliReader*`, `*Writer*`, `ImageExtractor*`
- [ ] T031 [P] [US2] Apply `graph` label to test files matching `GraphFlattener*`, `ExpressionToGraph*`, `NodeView*`, `Node*`
- [ ] T032 [P] [US2] Apply `mesh` label to test files matching `MeshBVH*`, `MeshSimplification*`, `MeshVoxelGrid*`
- [ ] T033 [P] [US2] Apply `sdf` label to test files matching `MeshSDF*`, `SignDetermination*`, `Spatial*`
- [ ] T034 [P] [US2] Apply `beamlattice` label to test files matching `BeamLattice*`
- [ ] T035 [P] [US2] Apply `parser` label to test files matching `ExpressionParser*`, `Function*`
- [ ] T036 [P] [US2] Apply `ui` label to test files matching `*Dialog*`, `MainWindow*`
- [ ] T037 [US2] Verify component filtering works: `ctest -L mesh-export`, `ctest -L mcp`

**Checkpoint**: Developers can run component-specific tests with `-L <component>` filter

---

## Phase 5: User Story 3 - Run GPU/Integration Tests Separately (Priority: P2)

**Goal**: GPU/OpenCL tests run only when explicitly requested via IntegrationTests preset

**Independent Test**: Run `ctest --preset IntegrationTests` with GPU and verify GPU tests execute

### Implementation for User Story 3

- [ ] T038 [US3] Add `opencl` label to all integration tests requiring OpenCL context in `gladius/tests/integrationtests/CMakeLists.txt`
- [ ] T039 [US3] Verify integration tests include GPU-dependent tests: `ctest --preset IntegrationTests`
- [ ] T040 [US3] Verify negative filtering works: `ctest --preset AllTests -LE opencl` excludes GPU tests
- [ ] T041 [US3] Update integration test skip messages to be consistent: "OpenCL context not available" or "GPU tests disabled; set GLADIUS_RUN_GPU_TESTS=1"

**Checkpoint**: GPU tests are isolated in IntegrationTests preset and skip gracefully without GPU

---

## Phase 6: User Story 4 - Run API Tests for External Interface Validation (Priority: P3)

**Goal**: API tests validate GladiusLib and MCP external interfaces

**Independent Test**: Run `ctest --preset ApiTests` and verify only API boundary tests execute

### Implementation for User Story 4

- [ ] T042 [US4] Move `gladius/tests/unittests/MCP_tests.cpp` to `gladius/tests/apitests/`
- [ ] T043 [P] [US4] Move `gladius/tests/unittests/ApplicationMCPAdapter_tests.cpp` to `gladius/tests/apitests/`
- [ ] T044 [P] [US4] Move `gladius/tests/unittests/ApplicationMCPAdapter_Rollback_tests.cpp` to `gladius/tests/apitests/`
- [ ] T045 [P] [US4] Move `gladius/tests/integrationtests/GladiusLib_tests.cpp` to `gladius/tests/apitests/`
- [ ] T046 [P] [US4] Move `gladius/tests/integrationtests/MCP_tests.cpp` to `gladius/tests/apitests/`
- [ ] T047 [US4] Update `gladius/tests/apitests/CMakeLists.txt` to include all API test files and link required dependencies
- [ ] T048 [US4] Verify API tests build and run: `ctest --preset ApiTests`

**Checkpoint**: API tests validate external interfaces independently

---

## Phase 7: User Story 5 - CI/CD Runs Complete Test Suite Efficiently (Priority: P3)

**Goal**: CI can run test categories in optimal order with fail-fast behavior

**Independent Test**: Run full test suite and measure time vs. sequential execution

### Implementation for User Story 5

- [ ] T049 [US5] Update `.vscode/tasks.json` to add "Run Unit Tests" task using `ctest --preset UnitTests`
- [ ] T050 [P] [US5] Update `.vscode/tasks.json` to add "Run Integration Tests" task using `ctest --preset IntegrationTests`
- [ ] T051 [P] [US5] Update `.vscode/tasks.json` to add "Run API Tests" task using `ctest --preset ApiTests`
- [ ] T052 [P] [US5] Update `.vscode/tasks.json` to add "Run All Tests" task using `ctest --preset AllTests`
- [ ] T053 [US5] Document CI workflow in `docs/developer_onboarding.md` with test stage ordering

**Checkpoint**: VS Code tasks and CI documentation reflect new test organization

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and cleanup across all stories

- [ ] T054 [P] Update `docs/developer_onboarding.md` with new test organization section
- [ ] T055 [P] Copy `specs/004-test-suite-restructure/quickstart.md` content to `docs/testing.md` (new file)
- [ ] T056 Build all test targets and verify no compilation errors: `cmake --build . --target gladius_test gladius_integrationtest gladius_apitest`
- [ ] T057 Run full validation: `ctest --preset AllTests` and verify all tests pass or skip gracefully
- [ ] T058 Verify unit test timing: `time ctest --preset UnitTests` confirms <60 seconds
- [ ] T059 Update `.github/copilot-instructions.md` test-related guidance if needed
- [ ] T060 Remove any obsolete test-related VS Code tasks from `.vscode/tasks.json`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-7)**: All depend on Foundational phase completion
  - User Story 1 (P1) should complete first (MVP)
  - User Stories 2 & 3 (both P2) can run in parallel after US1
  - User Stories 4 & 5 (both P3) can run in parallel after US2/US3
- **Polish (Phase 8)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Foundation only - No dependencies on other stories
- **User Story 2 (P2)**: Foundation only - Can run in parallel with US3
- **User Story 3 (P2)**: Foundation only - Can run in parallel with US2
- **User Story 4 (P3)**: Foundation only - Independent but lower priority
- **User Story 5 (P3)**: Depends on US1-US4 presets being functional

### Within Each User Story

- File moves can run in parallel (marked [P])
- CMakeLists.txt updates must follow file moves
- Verification tasks must run after implementation

### Parallel Opportunities

**Phase 1**: T002 and T003 can run in parallel

**Phase 2**: T006, T007, T008 can run in parallel

**User Story 1**: All file move tasks (T009-T022) can run in parallel, followed by CMake updates

**User Story 2**: All label application tasks (T027-T036) can run in parallel after T026

**User Story 4**: All file move tasks (T042-T046) can run in parallel

**User Story 5**: All VS Code task updates (T049-T052) can run in parallel

---

## Parallel Example: User Story 1 File Migrations

```bash
# All 14 file moves can execute simultaneously:
git mv gladius/tests/unittests/ColorExport_Integration_tests.cpp gladius/tests/integrationtests/
git mv gladius/tests/unittests/DualContouringOctree_tests.cpp gladius/tests/integrationtests/
git mv gladius/tests/unittests/DualContouringStlExporter_tests.cpp gladius/tests/integrationtests/
# ... (remaining moves)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (4 tasks)
2. Complete Phase 2: Foundational (4 tasks)
3. Complete Phase 3: User Story 1 (17 tasks)
4. **STOP and VALIDATE**: Run `ctest --preset UnitTests` - must complete in <60s
5. Deploy/demo if ready - developers can now run fast unit tests

### Incremental Delivery

1. **Foundation Ready** → Setup + Foundational complete
2. **MVP (US1)** → Fast unit tests work → Immediate developer productivity gain
3. **Add US2** → Component filtering works → Focused testing enabled
4. **Add US3** → GPU isolation verified → Clean separation of concerns
5. **Add US4** → API tests isolated → External interface validation
6. **Add US5** → CI/VS Code integration → Full workflow support
7. **Polish** → Documentation complete → Maintainability ensured

---

## Notes

- All file moves should use `git mv` to preserve history
- After moving files, run `cmake --build . --target <target>` to verify compilation
- CTest label syntax: `PROPERTIES LABELS "label1;label2"` for multiple labels
- Test presets in CMakePresets.json require CMake 3.21+
- Verify no tests are orphaned (not included in any test executable)
