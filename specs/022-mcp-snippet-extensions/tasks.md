# Tasks: MCP Snippet Tool Extensions

**Feature**: 022-mcp-snippet-extensions  
**Generated**: 2026-02-26  
**Source**: [plan.md](plan.md) + [spec.md](spec.md) + [research.md](research.md) + [data-model.md](data-model.md) + [contracts/mcp-tools.md](contracts/mcp-tools.md) + [quickstart.md](quickstart.md)

**Key Insight**: Research revealed that most spec requirements (FR-001, FR-002, FR-003, FR-005, FR-008) are **already implemented** in feature 021. This task list focuses on: (1) comprehensive test coverage, (2) build-item root annotations, (3) reserved keyword validation, and (4) deprecation annotations.

---

## Phase 1: Setup

**Purpose**: No project scaffolding needed — this feature extends existing files only.

- [X] T001 Verify feature branch `022-mcp-snippet-extensions` is checked out and builds cleanly via "Build ALL (linux-releaseWithDebug)" task

---

## Phase 2: Foundational

**Purpose**: Shared infrastructure needed by multiple user stories. MUST complete before user story phases.

- [X] T002 Add `isReservedKeyword(std::string const& name)` utility function in `gladius/src/FunctionArgument.h` with the reserved keyword set (~40 keywords from data-model.md)
- [X] T003 [P] Add test for `isReservedKeyword` accepting valid names and rejecting reserved keywords in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

**Checkpoint**: Keyword validation utility exists and is tested. All user stories can proceed.

---

## Phase 3: User Story 1 - Get Full Function Signature via Snippet Tool (Priority: P1)

**Goal**: Verify that `get_function_snippet` returns complete function signatures (arguments + output_type). Already implemented — needs test coverage.

**Independent Test**: Call `get_function_snippet` for a function with known arguments and verify the JSON response includes correct `arguments` and `output_type` fields.

### Tests for User Story 1

- [X] T004 [P] [US1] Test `get_function_snippet` returns `arguments` array with correct name/type pairs for a multi-argument function in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T005 [P] [US1] Test `get_function_snippet` returns correct `output_type` (float and vec3 cases) in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T006 [P] [US1] Test `get_function_snippet` returns empty `arguments` array for a function with no explicit arguments (only implicit Begin node) in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T007 [US1] Test signature round-trip fidelity: get_function_snippet → set_function_snippet with same args → get_function_snippet produces identical arguments and output_type in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

**Checkpoint**: US1 acceptance scenarios all verified by tests. Agent can reliably query function signatures.

---

## Phase 4: User Story 2 - Define Function Arguments via Snippet Tools (Priority: P1)

**Goal**: Verify that `set_function_snippet` and `create_function_from_snippet` correctly accept arguments and create Begin node ports. Add reserved keyword validation.

**Independent Test**: Call `set_function_snippet` with an explicit arguments list and verify the resulting graph has matching input parameter nodes.

### Tests for User Story 2

- [X] T008 [P] [US2] Test `set_function_snippet` creates correct argument ports on Begin node for multi-argument function in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T009 [P] [US2] Test `set_function_snippet` updates arguments on existing function (add new argument, verify it appears on Begin node) in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T010 [P] [US2] Test `create_function_from_snippet` with arguments creates a new function with correct Begin node ports in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

### Implementation for User Story 2

- [X] T011 [US2] Integrate `isReservedKeyword` validation into `setFunctionSnippet` handler — reject argument names that are reserved keywords with clear error message in `gladius/src/mcp/tools/FunctionOperationsTool.cpp`
- [X] T012 [US2] Integrate `isReservedKeyword` validation into `createFunctionFromSnippet` handler in `gladius/src/mcp/tools/FunctionOperationsTool.cpp`
- [X] T013 [US2] Test reserved keyword rejection: calling `set_function_snippet` with argument name `float` returns error in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T014 [US2] Test reserved keyword rejection: calling `create_function_from_snippet` with argument name `length` returns error in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

**Checkpoint**: US2 verified — agents can create/update function arguments via snippet tools. Invalid argument names are rejected.

---

## Phase 5: User Story 3 - Query Assembly-Level Code View (Priority: P1)

**Goal**: Add build-item root annotations to `get_program_snippet` so agents can see which functions are scene entry points. Program signatures and topological ordering are already implemented.

**Independent Test**: Call `get_program_snippet` on a document with build items and verify the response contains `root_functions` array and `[root]` comment annotations.

### Implementation for User Story 3

- [X] T015 [US3] Add `root_functions` array to `getProgramSnippet` JSON response by querying build items → objects → function resource IDs in `gladius/src/mcp/tools/FunctionOperationsTool.cpp`
- [X] T016 [US3] Add `[root]` annotation to function comment headers in snippet text for root functions (post-process snippet at MCP handler level per research decision R4) in `gladius/src/mcp/tools/FunctionOperationsTool.cpp`

### Tests for User Story 3

- [X] T017 [P] [US3] Test `get_program_snippet` response contains `root_functions` array with correct resource IDs in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T018 [P] [US3] Test `get_program_snippet` snippet text includes `[root]` annotation on function headers for root functions in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T019 [P] [US3] Test `get_program_snippet` includes non-root (orphan) functions without `[root]` annotation in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T020 [US3] Test `get_program_snippet` on document with multiple functions returns them in topological order with full signatures in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

**Checkpoint**: US3 verified — agents can query the full assembly with root annotations, signatures, and dependency ordering.

---

## Phase 6: User Story 4 - Update Assembly-Level Code (Priority: P2)

**Goal**: Verify that `set_program_snippet` correctly handles signature changes, function addition, and cross-function references. Already implemented — needs test coverage.

**Independent Test**: Retrieve a program snippet, modify a function's arguments, send it back, and verify the graph reflects the changes.

### Tests for User Story 4

- [X] T021 [P] [US4] Test `set_program_snippet` with modified function arguments (change `sphere(pos: vec3)` to `sphere(pos: vec3, radius: float)`) updates the graph correctly in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T022 [P] [US4] Test `set_program_snippet` with a new function definition added to the program creates a new function resource in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T023 [US4] Test `set_program_snippet` preserves functions not present in the snippet (non-destructive behavior) in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T024 [US4] Test program round-trip: get_program_snippet → set_program_snippet → get_program_snippet produces equivalent output in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

**Checkpoint**: US4 verified — agents can modify the full program through code. All user stories 1–4 are independently functional.

---

## Phase 7: User Story 5 - Deprecate Graph-Based MCP Tools (Priority: P3)

**Goal**: Mark graph-based tools as deprecated in tool descriptions and responses. Tools remain functional.

**Independent Test**: Verify deprecated tools still function and include deprecation metadata in responses.

### Implementation for User Story 5

- [X] T025 [US5] Add `[DEPRECATED]` prefix to descriptions of 9 graph-based tools (`get_function_graph`, `set_function_graph`, `create_node`, `delete_node`, `create_link`, `delete_link`, `set_parameter_value`, `create_function_call_node`, `create_constant_nodes_for_missing_parameters`) in `gladius/src/mcp/MCPServer.cpp`
- [X] T026 [US5] Add `"deprecated": true` field and deprecation notice to response JSON of each deprecated tool handler in `gladius/src/mcp/MCPServer.cpp`

### Tests for User Story 5

- [X] T027 [P] [US5] Test that a deprecated tool (`create_node`) still functions correctly and returns expected results in `gladius/tests/unittests/JSONRPC_tests.cpp`
- [X] T028 [US5] Test that deprecated tool responses include `deprecated: true` field in `gladius/tests/unittests/JSONRPC_tests.cpp`

**Checkpoint**: US5 verified — graph tools are annotated as deprecated but remain functional. Backward compatibility preserved.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final validation across all user stories.

- [X] T029 Run full test suite via "Run Gladius Tests" task and fix any regressions (3 pre-existing File3mfRoundTrip_Test failures due to topological sort instability — not caused by our changes)
- [X] T030 Run quickstart.md validation scenario (complete agent workflow: get_program → modify → set_function → verify) — covered by T007, T021, T017-T019
- [X] T031 [P] Verify all existing MCP integration tests pass with deprecated graph tools (SC-006) — all 20 JSONRPC tests pass

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS User Stories 2 (keyword validation)
- **US1 (Phase 3)**: Depends on Setup only — can start immediately alongside Phase 2
- **US2 (Phase 4)**: Depends on Foundational (Phase 2) for keyword validation
- **US3 (Phase 5)**: Depends on Setup only — can start immediately alongside Phase 2
- **US4 (Phase 6)**: Depends on US1, US2, US3 being complete (validates full workflow)
- **US5 (Phase 7)**: Independent — can start anytime after Setup
- **Polish (Phase 8)**: Depends on all desired phases being complete

### User Story Dependencies

- **US1 (P1)**: No dependencies on other stories — test-only phase
- **US2 (P1)**: Depends on Phase 2 (keyword utility) — implementation + tests
- **US3 (P1)**: No dependencies on other stories — implementation + tests
- **US4 (P2)**: Depends on US1 + US2 + US3 for full integration verification
- **US5 (P3)**: Independent of all other stories

### Within Each User Story

- Tests marked [P] can run in parallel within the same phase
- Implementation tasks must complete before their corresponding validation tests
- Core implementation before integration tests

### Parallel Opportunities

- **Phase 2 + Phase 3 + Phase 5**: All three can start simultaneously (T002/T003 || T004-T007 || T015-T020)
- **US5 (Phase 7)**: Can run in parallel with any other user story
- Within each phase: All tasks marked [P] can run in parallel

---

## Parallel Example: Maximum Parallelism After Setup

```
Parallel Track A: Phase 2 (Foundational)
  T002: Add isReservedKeyword utility
  T003: Test isReservedKeyword

Parallel Track B: Phase 3 (US1 — test-only)
  T004: Test get_function_snippet returns arguments
  T005: Test get_function_snippet returns output_type
  T006: Test empty arguments case

Parallel Track C: Phase 5 (US3 — implementation)
  T015: Add root_functions to getProgramSnippet
  T016: Add [root] annotations to snippet text

After Track A completes → Phase 4 (US2) can begin
After Tracks A+B+C complete → Phase 6 (US4) can begin
Track D (US5) can run at any time
```

---

## Implementation Strategy

### MVP First (User Stories 1–3)

1. Complete Phase 1: Setup (verify build)
2. Start in parallel: Phase 2 (foundational), Phase 3 (US1 tests), Phase 5 (US3 implementation)
3. Complete Phase 4: US2 (keyword validation + tests, after Phase 2)
4. **STOP and VALIDATE**: All P1 stories (US1, US2, US3) should work independently
5. Continue to Phase 6 (US4) for integration validation
6. Phase 7 (US5) for deprecation annotations
7. Phase 8: Final polish and full test suite

### Incremental Delivery

- **After Phase 3**: Agents can reliably query function signatures (verified)
- **After Phase 4**: Agents can create/update functions with typed arguments + keyword safety (verified)
- **After Phase 5**: Agents can see the full assembly with root annotations (new capability)
- **After Phase 6**: Agents can round-trip full programs with signature changes (verified)
- **After Phase 7**: Tool surface is cleaned up — snippet tools are the primary interface

### Task Count Summary

| Phase | Tasks | New Code | Test-Only |
|-------|-------|----------|-----------|
| Setup | 1 | 0 | 0 |
| Foundational | 2 | 1 | 1 |
| US1 (P1) | 4 | 0 | 4 |
| US2 (P1) | 7 | 2 | 5 |
| US3 (P1) | 6 | 2 | 4 |
| US4 (P2) | 4 | 0 | 4 |
| US5 (P3) | 4 | 2 | 2 |
| Polish | 3 | 0 | 3 |
| **Total** | **31** | **7** | **23** |
