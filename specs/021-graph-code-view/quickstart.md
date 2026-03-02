# Quickstart: Graph ↔ Code View

**Phase 1 output for [plan.md](plan.md)**

## Implementation Order

The feature decomposes into 5 implementation phases, ordered by dependency:

### Phase A: Converter Engine — Extend Node Coverage (P1)

**Goal**: Support all node types in `convertGraphToSnippet` and `convertSnippetToGraph`.

**Files to modify**:
- `gladius/src/ExpressionToGraphConverter.cpp` — add cases to `nodeToExpression` and parsing functions
- `gladius/src/ExpressionToGraphConverter.h` — add `generateUniqueFunctionName`, `convertProgramToSnippet`

**Steps**:
1. Add `generateUniqueFunctionName(displayName, resourceId)` utility
2. Extend `nodeToExpression` for vector types: ComposeVector → `vec3(x,y,z)`, ConstantVector → `vec3(x,y,z)`, VectorFromScalar → `vec3(s)`
3. Extend `nodeToExpression` for matrix types: ConstantMatrix → `mat4(...)`, Transpose/Inverse
4. Extend `nodeToExpression` for FunctionCall: resolve name via `resolveFunctionId()` + `findModel()` → emit `name_id(args...)`
5. Extend `nodeToExpression` for resource-backed nodes: SignedDistanceToMesh, ImageSampler, etc.
6. Extend snippet→graph parser: add `vec3()`, `mat4()` constructors, `dot()`, `cross()`, `transform()`, etc.
7. Extend snippet→graph parser: add function call parsing with ResourceId extraction from name suffix
8. Add `convertProgramToSnippet` with topological sort and cycle detection
9. Write tests for each new node type conversion (both directions)
10. Extend existing idempotency tests with new node types

**Tests**: `GraphToSnippet_tests.cpp`, `SnippetToGraph_tests.cpp`, extend `SnippetGraphIdempotency_tests.cpp`

**Estimated scope**: ~300–400 lines of converter changes, ~400 lines of new tests

### Phase B: MCP Tools (P2)

**Goal**: Expose snippet conversion via MCP for AI agents.

**Files to modify**:
- `gladius/src/mcp/MCPServer.cpp` — register 3 new tools
- `gladius/src/mcp/tools/FunctionOperationsTool.h/.cpp` — add handler methods
- `gladius/src/mcp/ApplicationMCPAdapter.h/.cpp` — add delegation methods

**Steps**:
1. Add `getFunctionSnippet(ResourceId)` to FunctionOperationsTool
2. Add `setFunctionSnippet(ResourceId, snippet, outputType, arguments)` to FunctionOperationsTool
3. Add `getProgramSnippet()` to FunctionOperationsTool
4. Register `get_function_snippet`, `set_function_snippet`, `get_program_snippet` in MCPServer
5. Add delegation methods to ApplicationMCPAdapter
6. Write MCP integration tests

**Tests**: `MCPSnippetTool_tests.cpp`

**Dependencies**: Phase A (converter must support all node types)

### Phase C: Code Tab UI (P1)

**Goal**: Add Code tab to ModelEditor with sync button.

**Files to create/modify**:
- `gladius/src/ui/CodeView.h` — new: code editor widget class
- `gladius/src/ui/CodeView.cpp` — new: implementation
- `gladius/src/ui/ModelEditor.h` — add `TabMode::Code`, `CodeView` member
- `gladius/src/ui/ModelEditor.cpp` — add tab bar for all functions, Code tab rendering

**Steps**:
1. Create `CodeView` class with `setFunction()`, `render()`, `hasUnsavedChanges()`
2. Add code buffer management (per-function map, lazy generation on first open)
3. Implement sync button: parse → replace graph → regenerate code → report errors
4. Add `TabMode::Code` to enum, show tab bar for all functions
5. Integrate CodeView into ModelEditor's tab rendering
6. Add unsaved-changes warning when switching from Code to Graph tab with dirty buffer
7. Write UI logic tests

**Tests**: `CodeView_tests.cpp`

**Dependencies**: Phase A (converter engine)

### Phase D: Whole-Program View (P3)

**Goal**: Allow viewing/editing all functions as a single code listing.

**Steps**:
1. `convertProgramToSnippet` already added in Phase A
2. Add whole-program view mode to CodeView (optional — could be MCP-only for V1)
3. Test with multi-function documents

**Dependencies**: Phases A, B, C

### Phase E: Strict Parsing & Error Reporting (P1)

**Goal**: Reject unsupported GLSL constructs with clear errors.

**Steps**:
1. Add strict-mode parsing: detect `for`, `if/else`, `struct`, `while` → reject with line number
2. Add unsupported-node-comment detection in sync path → reject
3. Add parse error location reporting (line + column)

**Dependencies**: Phase A (parser infrastructure)

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Code editor widget | `ImGui::InputTextMultiline` | Existing pattern, no new dependency, sufficient for V1 |
| Code buffer storage | `std::unordered_map<ResourceId, CodeBuffer>` in CodeView | Per-function, lazy, survives tab switches |
| Name generation | `sanitize(displayName)_resourceId` | Unique, readable, valid GLSL identifier |
| Sync direction | Code → parse → graph → regenerate code | Graph is ground truth, normalization ensures consistency |
| Tab visibility | Show tab bar for all functions (not just Image3D) | Code tab applies to every function |
| Whole-program ordering | Topological sort on FunctionCall DAG | Ensures callees defined before callers |
| Thread safety | Synchronous conversion (no async for V1) | Single-function conversion is fast enough (<2s for 100 nodes) |
| vec3 disambiguation | Literal args → ConstantVector, expression args → ComposeVector | Mirrors semantic difference in the graph |

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| FunctionCall parsing ambiguity (name vs built-in) | Medium | Built-in names are reserved; function names always have `_id` suffix |
| Large graphs slow to convert | Low | SC-002 target: <2s for 100 nodes — conversion is O(n) tree walk |
| Unsupported node types in user documents | Medium | `/* unsupported */` comments preserve info; strict sync rejection prevents data loss |
| ModelEditor.cpp exceeding 400 lines after changes | High | CodeView extracted to separate class; only ~20 lines added to ModelEditor |
