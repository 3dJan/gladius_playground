# Tasks: Graph ↔ Code View

**Input**: Design documents from `/specs/021-graph-code-view/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story. Tests are included per the constitution's Test-First principle (Principle II).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `gladius/src/` for production code
- **Tests**: `gladius/tests/unittests/` for GTest unit tests
- **Headers**: `gladius/src/*.h` for declarations
- **Spec docs**: `specs/021-graph-code-view/` for feature documentation

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Scaffolding and utilities shared by all user stories

- [X] T001 Add `generateUniqueFunctionName(displayName, resourceId)` utility in `gladius/src/ExpressionToGraphConverter.h` and `gladius/src/ExpressionToGraphConverter.cpp` — sanitize display name (replace non-alnum with `_`, collapse consecutive, prepend `f_` if starts with digit), append `_resourceId`
- [X] T002 [P] Add unit tests for `generateUniqueFunctionName` in `gladius/tests/unittests/CodeView_tests.cpp` — cover: basic name, spaces, special chars, leading digit, identical display names with different IDs, empty name
- [X] T003 [P] Create `gladius/src/ui/CodeView.h` with `CodeView` class declaration: `CodeBuffer` struct, `setFunction()`, `render()`, `hasUnsavedChanges()`, `discardChanges()`, per-function buffer map (`std::unordered_map<ResourceId, CodeBuffer>`)
- [X] T004 [P] Add `TabMode::Code = 2` to the `TabMode` enum in `gladius/src/ui/ModelEditor.h`, add `#include "CodeView.h"` and `CodeView m_codeView` member

**Checkpoint**: Setup complete — shared utilities and scaffolding in place

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Extend the converter engine to support all node types. This MUST be complete before any user story can deliver full functionality.

**⚠️ CRITICAL**: All user stories depend on the converter supporting their required node types.

### Graph → Snippet (extend `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`)

- [X] T005 Add ComposeVector → `vec3(x, y, z)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp` — recurse into x/y/z inputs, emit `vec3(exprX, exprY, exprZ)`
- [X] T006 [P] Add ConstantVector → `vec3(x, y, z)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp` — read x/y/z parameter values, emit literal `vec3(x, y, z)`
- [X] T007 [P] Add VectorFromScalar → `vec3(scalar)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T008 [P] Add DotProduct → `dot(a, b)` and CrossProduct → `cross(a, b)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T009 [P] Add ConstantMatrix → `mat4(m00..m33)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp` — read 16 parameter values row-major
- [X] T010 [P] Add Transpose → `transpose(m)` and Inverse → `inverse(m)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T011 [P] Add MatrixVectorMultiplication → `matmul(m, v)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T012 Add FunctionCall → `functionName_id(args...)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp` — resolve FunctionId via Resource node, look up display name, generate unique name, recurse into arguments
- [X] T013 [P] Add FunctionGradient → `gradient_functionName_id(pos)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T014 [P] Add resource-backed SDF nodes conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp` — SignedDistanceToMesh → `sdfMesh_id(pos)`, UnsignedDistanceToMesh → `udfMesh_id(pos)`, SignedDistanceToBeamLattice → `sdfBeamLattice_id(pos)`
- [X] T015 [P] Add ImageSampler → `sampleImage3D_id(pos)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T016 [P] Add Transformation → `transform(pos, matrix)` and NormalizeDistanceField → `normalizeSDF(expr)` conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T017 [P] Add ComposeMatrix, ComposeMatrixFromColumns, ComposeMatrixFromRows, and DecomposeMatrix conversion in `nodeToExpression` in `gladius/src/ExpressionToGraphConverter.cpp`

### Snippet → Graph (extend parser in `gladius/src/ExpressionToGraphConverter.cpp`)

- [X] T018 Add `vec3(x, y, z)` / `vec3(s)` constructor parsing in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp` — 3 literal args → ConstantVector, 3 expression args → ComposeVector, 1 arg → VectorFromScalar
- [X] T019 [P] Add `dot()`, `cross()`, `matmul()`, `transpose()`, `inverse()` built-in function parsing in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T020 [P] Add `mat4(...)` constructor parsing in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp` — 16 args → ConstantMatrix
- [X] T021 Add function call parsing with ResourceId extraction in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp` — parse `name_42(args)`, extract trailing integer as ResourceId, create Resource + FunctionCall nodes, wire arguments
- [X] T022 [P] Add `transform()`, `normalizeSDF()`, `sdfMesh_id()`, `udfMesh_id()`, `sdfBeamLattice_id()`, `sampleImage3D_id()` parsing in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp`
- [X] T023 Add `gradient_name_id()` parsing in snippet→graph parser in `gladius/src/ExpressionToGraphConverter.cpp` — extract function ResourceId, create Resource + FunctionGradient node

### Foundational Tests

- [X] T024 [P] Write graph→snippet tests for vector types (ComposeVector, ConstantVector, VectorFromScalar) in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T025 [P] Write graph→snippet tests for matrix types (ConstantMatrix, Transpose, Inverse, MatrixVectorMultiplication) in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T026 [P] Write graph→snippet tests for FunctionCall and FunctionGradient nodes in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T027 [P] Write graph→snippet tests for resource-backed nodes (SDF, ImageSampler, Transformation, NormalizeDistanceField) in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T028 [P] Write snippet→graph parsing tests for `vec3()`, `mat4()`, `dot()`, `cross()`, `matmul()`, `transpose()`, `inverse()` in `gladius/tests/unittests/SnippetToGraph_tests.cpp`
- [X] T029 [P] Write snippet→graph parsing tests for function calls with ResourceId extraction in `gladius/tests/unittests/SnippetToGraph_tests.cpp`
- [X] T030 [P] Write snippet→graph parsing tests for resource-backed nodes (`sdfMesh_id()`, `sampleImage3D_id()`, `transform()`, `normalizeSDF()`) in `gladius/tests/unittests/SnippetToGraph_tests.cpp`
- [X] T031 [P] Extend existing idempotency tests in `gladius/tests/unittests/SnippetGraphIdempotency_tests.cpp` with new node types: ComposeVector, FunctionCall, dot, cross, mat4, resource-backed nodes

**Checkpoint**: Converter engine supports all node types. All foundational tests pass. User story implementation can begin.

---

## Phase 3: User Story 1 — View a Function as Code (Priority: P1) 🎯 MVP

**Goal**: User opens a function and clicks the Code tab to see GLSL-like code generated from the node graph.

**Independent Test**: Open any function graph → switch to Code tab → verify syntactically consistent snippet representing the full graph.

### Tests for User Story 1

- [X] T032 [P] [US1] Write CodeView unit test: given a model with arithmetic nodes, `convertGraphToSnippet` produces valid GLSL-like output in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T033 [P] [US1] Write CodeView unit test: given FunctionCall node, output contains `functionName_id(...)` call syntax in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T034 [P] [US1] Write CodeView unit test: given empty graph (Begin+End only), output is minimal valid snippet (`return 0;`) in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T035 [P] [US1] Write CodeView unit test: given unsupported node, output contains `/* unsupported: TypeName */` comment in `gladius/tests/unittests/CodeView_tests.cpp`

### Implementation for User Story 1

- [X] T036 [US1] Implement `CodeView::render()` in `gladius/src/ui/CodeView.cpp` — on first open for a function, call `convertGraphToSnippet`, store result in per-function `CodeBuffer`, display via `ImGui::InputTextMultiline` (read-only initially)
- [X] T037 [US1] Implement `CodeView::setFunction()` in `gladius/src/ui/CodeView.cpp` — manage `m_currentFunctionId`, lazily populate buffer on first open (FR-007)
- [X] T038 [US1] Modify tab bar in `gladius/src/ui/ModelEditor.cpp` — show tab bar for all functions (not just Image3D), add "Code" tab item, render `m_codeView` when `TabMode::Code` is active
- [X] T039 [US1] Wire `CodeView` into `ModelEditor::showAndEdit()` in `gladius/src/ui/ModelEditor.cpp` — call `m_codeView.setFunction()` with current model and assembly, route to `m_codeView.render()` when Code tab active

**Checkpoint**: User Story 1 complete — users can view any function as code via the Code tab.

---

## Phase 4: User Story 2 — Edit Code and Sync Back to Graph (Priority: P1)

**Goal**: User edits code in the Code tab, clicks Sync, and the graph updates. If errors, the graph is unchanged and user sees diagnostics.

**Independent Test**: Modify code → press Sync → verify graph updates (visible in Graph tab). Introduce error → Sync → verify error message and graph unchanged.

### Tests for User Story 2

- [X] T040 [P] [US2] Write sync test: valid code syncs and produces normalized output in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T041 [P] [US2] Write sync test: syntax error preserves original graph and returns error message in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T042 [P] [US2] Write sync test: code with `/* unsupported: ... */` comment is rejected in `gladius/tests/unittests/CodeView_tests.cpp`
- [X] T043 [P] [US2] Write strict parsing tests: `for`, `if/else`, `struct`, `while` rejected with line number in `gladius/tests/unittests/SnippetToGraph_tests.cpp`

### Implementation for User Story 2

- [X] T044 [US2] Make `ImGui::InputTextMultiline` editable in `CodeView::render()` in `gladius/src/ui/CodeView.cpp` — track dirty state by comparing buffer to `syncedText`
- [X] T045 [US2] Implement Sync button in `CodeView::render()` in `gladius/src/ui/CodeView.cpp` — call `convertSnippetToGraph`, on success replace model and regenerate code (normalized), on failure show error and preserve graph (FR-006)
- [X] T046 [US2] Add unsupported-node-comment detection in sync path in `gladius/src/ui/CodeView.cpp` — scan for `/* unsupported:` pattern before parsing, reject with clear error listing node types (FR-011)
- [X] T047 [US2] Add strict parsing rejection for unsupported GLSL constructs (`for`, `if/else`, `struct`, `while`) in `gladius/src/ExpressionToGraphConverter.cpp` — detect keywords, reject with line number (FR-016)
- [X] T048 [US2] Implement `hasUnsavedChanges()` and unsaved-changes warning in `gladius/src/ui/CodeView.cpp` and `gladius/src/ui/ModelEditor.cpp` — when switching from Code to Graph tab with dirty buffer, show ImGui confirmation dialog (FR-015)

**Checkpoint**: User Story 2 complete — users can edit code and sync back to graph with full error handling.

---

## Phase 5: User Story 3 — AI Agent Reads/Writes Functions via MCP (Priority: P2)

**Goal**: AI agents can get/set function code via MCP tools `get_function_snippet` and `set_function_snippet`.

**Independent Test**: Call MCP tool to get snippet for a function → modify → send back via set tool → verify graph changed.

### Tests for User Story 3

- [X] T049 [P] [US3] Write MCP test: `get_function_snippet` returns valid snippet for existing function in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T050 [P] [US3] Write MCP test: `get_function_snippet` returns error for non-existent function ID in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T051 [P] [US3] Write MCP test: `set_function_snippet` replaces graph and returns normalized snippet in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T052 [P] [US3] Write MCP test: `set_function_snippet` with parse error returns error and preserves graph in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

### Implementation for User Story 3

- [X] T053 [US3] Add `getFunctionSnippet(ResourceId)` method to `gladius/src/mcp/tools/FunctionOperationsTool.h` and `gladius/src/mcp/tools/FunctionOperationsTool.cpp` — look up model, call `convertGraphToSnippet`, return snippet + metadata
- [X] T054 [US3] Add `setFunctionSnippet(ResourceId, snippet, outputType, arguments)` method to `gladius/src/mcp/tools/FunctionOperationsTool.h` and `gladius/src/mcp/tools/FunctionOperationsTool.cpp` — clear existing graph, call `convertSnippetToGraph`, on error restore original, return normalized snippet
- [X] T055 [US3] Add delegation methods to `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp` for `getFunctionSnippet` and `setFunctionSnippet`
- [X] T056 [US3] Register `get_function_snippet` tool in `gladius/src/mcp/MCPServer.cpp` — add schema (function_id required), lambda calling `m_application->getFunctionSnippet()`
- [X] T057 [US3] Register `set_function_snippet` tool in `gladius/src/mcp/MCPServer.cpp` — add schema (function_id, snippet required; output_type, arguments optional), lambda calling `m_application->setFunctionSnippet()`

**Checkpoint**: User Story 3 complete — AI agents can read/write function code via MCP.

---

## Phase 6: User Story 4 — View Entire Program as Code (Priority: P3)

**Goal**: Convert all functions in the document to a single code listing in dependency order (topological sort). Expose via MCP tool.

**Independent Test**: Request whole-program snippet for a document with multiple functions → verify all functions appear in dependency order with correct cross-references.

### Tests for User Story 4

- [X] T058 [P] [US4] Write test: `convertProgramToSnippet` produces all functions in dependency order in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T059 [P] [US4] Write test: `convertProgramToSnippet` detects circular dependencies and throws in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T060 [P] [US4] Write test: functions with same display name get distinct unique names in `gladius/tests/unittests/GraphToSnippet_tests.cpp`
- [X] T061 [P] [US4] Write MCP test: `get_program_snippet` returns all functions in order in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

### Implementation for User Story 4

- [X] T062 [US4] Implement `convertProgramToSnippet(Assembly&)` in `gladius/src/ExpressionToGraphConverter.h` and `gladius/src/ExpressionToGraphConverter.cpp` — iterate `assembly.getFunctions()`, build FunctionCall dependency graph, topological sort (Kahn's algorithm), detect cycles, convert each function with unique name header
- [X] T063 [US4] Add `getProgramSnippet()` method to `gladius/src/mcp/tools/FunctionOperationsTool.h` and `gladius/src/mcp/tools/FunctionOperationsTool.cpp`
- [X] T064 [US4] Add delegation method to `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp` for `getProgramSnippet`
- [X] T065 [US4] Register `get_program_snippet` tool in `gladius/src/mcp/MCPServer.cpp` — add schema (no required params), lambda calling `m_application->getProgramSnippet()`

**Checkpoint**: User Story 4 complete — whole-program view available via MCP.

---

## Phase 7: User Story 5 — Update Entire Program from Code (Priority: P3)

**Goal**: AI agent sends a complete multi-function program as code via MCP; system parses all function definitions, creates/updates graphs, and wires up FunctionCall references.

**Independent Test**: Send multi-function snippet via MCP → verify all functions and cross-references created.

### Tests for User Story 5

- [X] T066 [P] [US5] Write test: multi-function snippet creates all functions with correct cross-references in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`
- [X] T067 [P] [US5] Write test: removing a function that still has callers is rejected with error in `gladius/tests/unittests/MCPSnippetTool_tests.cpp`

### Implementation for User Story 5

- [X] T068 [US5] Implement `setProgramSnippet(snippet)` in `gladius/src/mcp/tools/FunctionOperationsTool.h` and `gladius/src/mcp/tools/FunctionOperationsTool.cpp` — parse multi-function listing, create/update each function graph, resolve cross-references, validate no dangling refs, rollback on error
- [X] T069 [US5] Add delegation method to `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp` for `setProgramSnippet`
- [X] T070 [US5] Register `set_program_snippet` tool in `gladius/src/mcp/MCPServer.cpp` — add schema (snippet required), lambda calling `m_application->setProgramSnippet()`

**Checkpoint**: User Story 5 complete — full program round-trips via MCP.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T071 [P] Add Doxygen comments for all new public APIs in `gladius/src/ExpressionToGraphConverter.h`, `gladius/src/ui/CodeView.h`
- [X] T072 [P] Verify all new/modified files stay under 400 lines per constitution Principle III
- [X] T073 Run quickstart.md validation — verify all acceptance scenarios from spec.md pass
- [X] T074 [P] Update `specs/021-graph-code-view/` documentation with final implementation notes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on T001 (name utility) — BLOCKS all user stories
- **US1 (Phase 3)**: Depends on Phase 2 + T003/T004 (CodeView scaffolding)
- **US2 (Phase 4)**: Depends on Phase 3 (needs Code tab to exist)
- **US3 (Phase 5)**: Depends on Phase 2 (converter engine) — can run in parallel with US1/US2
- **US4 (Phase 6)**: Depends on Phase 2 (converter) + T062 (program converter)
- **US5 (Phase 7)**: Depends on Phase 6 (whole-program read must work before write)
- **Polish (Phase 8)**: Depends on all preceding phases

### User Story Dependencies

- **US1 (P1)**: Converter engine (Phase 2) + CodeView scaffold (T003/T004) — MVP target
- **US2 (P1)**: US1 (Code tab must exist to edit)
- **US3 (P2)**: Converter engine only — independent of UI stories, can parallelize with US1/US2
- **US4 (P3)**: Converter engine + `convertProgramToSnippet` — can parallelize with US1/US2/US3
- **US5 (P3)**: US4 (read-before-write)

### Within Each User Story

- Tests written FIRST, verified to FAIL before implementation
- Converter changes before UI/MCP integration
- Core logic before error handling
- Story checkpoint verifies independent functionality

### Parallel Opportunities

- **Phase 1**: T002, T003, T004 can all run in parallel (different files)
- **Phase 2**: T005–T017 graph→snippet tasks can mostly run in parallel (different node types in same file, but logically independent cases); T18–T23 snippet→graph similar; T24–T31 test files fully parallel
- **Phase 3–5**: US3 (MCP tools) can run in parallel with US1/US2 (UI) since they touch different files
- **Phase 6**: Can start as soon as Phase 2 completes, parallel with US1/US2/US3

---

## Parallel Example: User Story 1

```text
# Parallel: Write all US1 tests (different test cases in same file)
T032: CodeView test — arithmetic nodes produce valid output
T033: CodeView test — FunctionCall node output
T034: CodeView test — empty graph minimal snippet
T035: CodeView test — unsupported node comment

# Sequential: Implementation (depends on tests + Phase 2)
T036: Implement CodeView::render()
T037: Implement CodeView::setFunction()
T038: Modify ModelEditor tab bar
T039: Wire CodeView into ModelEditor
```

## Parallel Example: Foundational Phase

```text
# Parallel batch 1: Graph→Snippet node types (all add cases to nodeToExpression)
T005: ComposeVector
T006: ConstantVector
T007: VectorFromScalar
T008: DotProduct, CrossProduct
T009: ConstantMatrix
T010: Transpose, Inverse
T011: MatrixVectorMultiplication
T013: FunctionGradient
T014: Resource-backed SDF nodes
T015: ImageSampler
T016: Transformation, NormalizeDistanceField
T017: Matrix compose/decompose

# Sequential: FunctionCall (depends on name utility T001)
T012: FunctionCall → snippet

# Parallel batch 2: Snippet→Graph parsing
T018: vec3() constructors
T019: dot, cross, matmul, transpose, inverse
T020: mat4() constructor
T022: Resource-backed node parsing
T023: gradient parsing

# Sequential: Function call parsing (depends on T012 pattern)
T021: Function call parsing with ResourceId
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2)

1. Complete Phase 1: Setup (T001–T004)
2. Complete Phase 2: Foundational converter engine (T005–T031)
3. Complete Phase 3: US1 — View as Code (T032–T039)
4. Complete Phase 4: US2 — Edit and Sync (T040–T048)
5. **STOP and VALIDATE**: Both P1 stories independently testable
6. Build and run full test suite

### Incremental Delivery

1. Setup + Foundational → Converter engine ready
2. US1 → Code tab read-only → validate
3. US2 → Bidirectional sync → validate (P1 complete)
4. US3 → MCP tools → validate (P2 complete)
5. US4 → Whole-program view → validate
6. US5 → Whole-program write → validate (P3 complete)
7. Each story adds value without breaking previous stories

---

## Notes

- [P] tasks = different files or independent additions, no dependencies
- [Story] label maps task to specific user story for traceability
- Build with VS Code task "Build ALL (linux-releaseWithDebug)" — never run cmake/ninja manually
- Run tests with VS Code task "Run Unit Tests (Fast)" for quick iteration
- All test files are auto-discovered by `file(GLOB_RECURSE)` in `gladius/tests/unittests/CMakeLists.txt`
- Commit after each completed phase or logical group
