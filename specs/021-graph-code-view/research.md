# Research: Graph ↔ Code View

**Phase 0 output for [plan.md](plan.md)**

## R1: Converter Node Coverage Gap

**Decision**: Extend `ExpressionToGraphConverter` incrementally — add missing node types to existing `nodeToExpression` (graph→snippet) and snippet→graph pathways.

**Rationale**: The converter already handles ~30 of ~58 node types. The architecture (recursive `nodeToExpression` + `expressionToNode` dispatch) is clean and extensible. Adding new cases follows the established pattern.

**Alternatives considered**:
- New converter class → rejected: duplicates infrastructure, violates DRY
- AST intermediate representation → rejected: YAGNI for V1, adds complexity without clear benefit

### Current coverage

| Category | Node Types | Graph→Snippet | Snippet→Graph |
|----------|-----------|:---:|:---:|
| Binary arithmetic | Addition, Subtraction, Multiplication, Division | ✅ | ✅ |
| Unary math | Sin, Cos, Tan, ArcSin, ArcCos, ArcTan, Exp, Log, Log2, Log10, Sqrt, Abs, Sign, Floor, Ceil, Round, Fract, Length, SinH, CosH, TanH | ✅ | ✅ |
| Binary math | Pow, ArcTan2, Fmod, Mod, Min, Max | ✅ | ✅ |
| Ternary | Clamp | ✅ | ✅ |
| Quaternary | Select | ✅ | ✅ |
| Constants | ConstantScalar | ✅ | ✅ |
| Special | Begin (input ports), End (output) | ✅ | ✅ |
| Vector access | DecomposeVector (.x/.y/.z) | ✅ | ✅ |
| **Vector construction** | ComposeVector | ❌ | ❌ |
| **Vector constant** | ConstantVector | ❌ | partial |
| **Vector broadcast** | VectorFromScalar | ❌ | ❌ |
| **Matrix** | ConstantMatrix | ❌ | ❌ |
| **Matrix ops** | Transpose, Inverse, MatrixVectorMultiplication | ❌ | ❌ |
| **Matrix compose** | ComposeMatrix, ComposeMatrixFromColumns, ComposeMatrixFromRows | ❌ | ❌ |
| **Matrix decompose** | DecomposeMatrix | ❌ | ❌ |
| **Function calls** | FunctionCall + Resource | ❌ | ❌ |
| **Gradient** | FunctionGradient | ❌ | ❌ |
| **Transform** | Transformation | ❌ | ❌ |
| **Cross/Dot** | DotProduct, CrossProduct | ❌ | ❌ |
| **Mesh SDF** | SignedDistanceToMesh, UnsignedDistanceToMesh | ❌ | ❌ |
| **Beam SDF** | SignedDistanceToBeamLattice | ❌ | ❌ |
| **Image sampling** | ImageSampler | ❌ | ❌ |
| **Normalization** | NormalizeDistanceField | ❌ | ❌ |
| **Internal** | BoxMinMax | ❌ (internal, skip) | ❌ |

### Proposed code syntax for gap types

| Node Type | Proposed Syntax | Notes |
|-----------|----------------|-------|
| ComposeVector | `vec3(exprX, exprY, exprZ)` | 3-arg vec3 constructor |
| ConstantVector | `vec3(x, y, z)` | Same syntax, literal args → ConstantVector; expression args → ComposeVector |
| VectorFromScalar | `vec3(scalar)` | 1-arg vec3 constructor (GLSL broadcast) |
| ConstantMatrix | `mat4(m00, m01, ..., m33)` | 16-arg constructor, row-major |
| DotProduct | `dot(a, b)` | GLSL built-in |
| CrossProduct | `cross(a, b)` | GLSL built-in |
| MatrixVectorMultiplication | `matmul(m, v)` or `m * v` | Named function preferred for clarity |
| Transpose | `transpose(m)` | GLSL built-in |
| Inverse | `inverse(m)` | GLSL built-in |
| FunctionCall | `functionName_42(arg1, arg2, ...)` | Display name + resource ID |
| FunctionGradient | `gradient_functionName_42(pos)` | Special prefix |
| Transformation | `transform(pos, matrix)` | Custom built-in |
| SignedDistanceToMesh | `sdfMesh_42(pos)` | Resource reference by ID |
| UnsignedDistanceToMesh | `udfMesh_42(pos)` | Resource reference by ID |
| SignedDistanceToBeamLattice | `sdfBeamLattice_42(pos)` | Resource reference by ID |
| ImageSampler | `sampleImage3D_42(pos)` | Resource reference by ID |
| NormalizeDistanceField | `normalizeSDF(expr)` | Wrapping function |
| ComposeMatrix | `mat4(row0, row1, row2, row3)` | 4-arg variant |
| DecomposeMatrix | `.row0` / `.row1` / `.row2` / `.row3` | Component access |

## R2: FunctionCall Node Implementation

**Decision**: Represent FunctionCall nodes as named function calls using the pattern `sanitizedDisplayName_resourceId(args...)`.

**Rationale**: The FunctionCall node in DerivedNodes.h has a `FunctionId` input linked through a `Resource` node. The name generation uses display name sanitization (replace non-alnum with `_`, collapse consecutive) + underscore + resource ID, producing unique valid identifiers.

**Key implementation details**:
- `FunctionCall::resolveFunctionId()` follows: FunctionCall → FunctionId input → Resource node → ResourceId param
- `FunctionCall::getArguments()` returns mirrored parameters from the referenced function
- `FunctionCall::updateInputsAndOutputs(referencedModel)` dynamically syncs ports
- In snippet→graph: parse `name_42(args)` → extract ResourceId from trailing number → create Resource node + FunctionCall node → wire them → connect arguments

**Alternatives considered**:
- Use display name only → rejected: not unique when multiple functions share a name
- Use only resource ID → rejected: not human-readable

## R3: ModelEditor Tab Pattern

**Decision**: Add `TabMode::Code = 2` following the existing tab pattern. Show tab bar for all functions (not just Image3D).

**Rationale**: The existing tab bar at ModelEditor.cpp:1367 uses `ImGui::BeginTabBar("FunctionTabs")` with two `ImGui::BeginTabItem` calls. Currently shown only for Image3D functions. The Code tab should be available for all functions.

**Key findings**:
- Tab switching: `m_currentTabMode` member, set by ImGui's tab state
- Graph state preserved: per-function `EditorContext` in `m_editorContexts` map — survives tab switches
- No existing unsaved-changes pattern — must design from scratch
- Function selection via `switchToFunction(ResourceId)` does NOT reset tab mode
- Properties tab is stateless (rebuilds from model each frame)

**Design implications**:
- Code tab needs per-function buffer state (the edited text) — store in a map keyed by ResourceId
- "Generate on first open" (FR-007): populate buffer only when Code tab opened for a function that has no buffer yet
- Unsaved-changes warning (FR-015): check if code buffer differs from last-synced version when switching to Graph tab
- Extract Code tab logic into `CodeView` class to keep ModelEditor under 400 lines

## R4: MCP Tool Registration

**Decision**: Add three new MCP tools following the existing registration pattern in MCPServer::setupBuiltinTools().

**Rationale**: The pattern is well-established: `registerTool(name, description, schema, lambda)`. The lambda captures `this` and delegates to `m_application->method()`. FunctionOperationsTool already handles `create_function_from_snippet` and provides the model for new snippet tools.

**New tools**:
1. `get_function_snippet` — calls `convertGraphToSnippet` on a function's model
2. `set_function_snippet` — calls `convertSnippetToGraph` on existing function (replace)
3. `get_program_snippet` — iterates all functions via `assembly.getFunctions()`, converts each

**Key findings**:
- No existing graph→snippet MCP tool (confirmed gap)
- Tool access chain: MCPServer → ApplicationMCPAdapter → FunctionOperationsTool → Application → Document → Assembly
- Thread safety: sequential in stdio mode, no explicit locks in tool layer
- Error handling pattern: check inputs, try/catch around converter, return JSON with success/error
- `create_function_from_snippet` provides template: validate → create model → call converter → handle error by deleting partial model → persist with `update3mfModel()`

## R5: Code Editor Widget

**Decision**: Use `ImGui::InputTextMultiline` for V1 code editor. No third-party editor widget.

**Rationale**: The codebase already uses `ImGui::InputTextMultiline` in ExpressionDialog.cpp and LibraryExportDialog.cpp. Adding a third-party syntax-highlighting editor (e.g., ImGuiColorTextEdit) would add a dependency and complexity. The spec states: "a plain text editor with basic syntax highlighting; a full IDE experience is out of scope for V1."

**Alternatives considered**:
- ImGuiColorTextEdit → deferred to V2: adds dependency, overkill for V1
- Custom syntax highlighting via ImGui render callbacks → deferred: complexity without core value

**Implementation**: Monospace font, large InputTextMultiline area, Sync button below. Code buffer stored per-function.

## R6: Whole-Program Listing

**Decision**: Iterate `assembly.getFunctions()` (returns `std::map<ResourceId, SharedModel>`), topologically sort by call dependencies, convert each function, concatenate.

**Rationale**: Functions form a DAG via FunctionCall nodes. Topological sort ensures callees appear before callers. Circular dependencies detected during sort → reject with error listing the cycle (FR-013).

**Key details**:
- `Assembly::getFunctions()` gives all functions as a sorted map
- Build dependency graph: for each function, find FunctionCall nodes → resolve their target ResourceId
- Topological sort (Kahn's algorithm or DFS) — detect cycles
- Each function emitted as: `// Function: displayName (ID: resourceId)\nfloat functionName_id(vec3 pos, ...) {\n  ...\n}\n`

## R7: Unique Name Generation

**Decision**: `sanitize(displayName) + "_" + resourceId`. Sanitization: replace non-alphanumeric chars with `_`, collapse consecutive `_`, strip leading `_` if result starts with digit → prepend `f_`.

**Rationale**: Must produce valid GLSL identifiers (letter or underscore first, then alphanumeric/underscore). Resource ID suffix guarantees uniqueness. Matches FR-003 clarification.

**Examples**:
- `"My Sphere"`, ID 42 → `My_Sphere_42`
- `"gyroid"`, ID 10 → `gyroid_10`
- `"123invalid"`, ID 5 → `f_123invalid_5`
- `"a  !@#  b"`, ID 7 → `a_b_7`
