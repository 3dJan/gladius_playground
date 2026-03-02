# Feature Specification: Graph ↔ Code View

**Feature Branch**: `021-graph-code-view`  
**Created**: 2026-02-23  
**Status**: Draft  
**Input**: User description: "As a user I want to be able to switch between graph representation and code representation (both directions). The graph representation will always be the ground truth. The code/snippet representation must support all node types of the graph representation, including function call nodes. The syntax should be kept close to GLSL. Function call nodes should look like method calls in the snippet presentation. Since the display name of a function is not unique, we need a way to generate unique function names based on the display name and resource ID. In a first version we want only to be able to switch/sync one function graph (representing one function), later we want to be able to work on the whole program and convert it back to the graph representation. One important use case is to extend the MCP tools, so that an AI agent can modify and see the whole program, which is usually easier for LLMs than figuring out how to use the graph representation. Since there are many SDF examples as GLSL code, converting GLSL code to 3MF/Gladius might be a common task. In the UI I just want to have tabs to switch between the graph representation/node editor and the code editor, and a button for syncing (graph gets updated and if successful the code gets regenerated based on the graph (normalized))."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - View a Function as Code (Priority: P1)

A user opens a function in the Model Editor and wants to see the equivalent code/snippet representation. They click the "Code" tab and the system generates GLSL-like code from the current node graph, displaying it in a read-only code view. The code uses the function's unique name (derived from display name + resource ID) and represents all nodes, links, and parameter values as readable statements.

**Why this priority**: This is the foundation—graph-to-code conversion. Without it, no other code-view features work. It provides immediate value by letting users and AI agents read function logic as text.

**Independent Test**: Can be tested by opening any existing function graph and switching to the Code tab; the generated code should be syntactically consistent and represent the full graph.

**Acceptance Scenarios**:

1. **Given** a function graph with arithmetic nodes (e.g., Addition, Multiplication, Sine), **When** the user switches to the Code tab, **Then** the code view displays a GLSL-like snippet that represents all nodes and their connections.
2. **Given** a function graph containing a FunctionCall node referencing another function, **When** the user switches to the Code tab, **Then** the FunctionCall appears as a method call using the referenced function's unique name (e.g., `myFunction_42(...)`).
3. **Given** a function graph with scalar constants and vector decompose/compose nodes, **When** the user switches to the Code tab, **Then** constants appear as literal values and vector operations use GLSL-style component access (`.x`, `.y`, `.z`).
4. **Given** an empty function graph (only Begin and End nodes), **When** the user switches to the Code tab, **Then** a minimal valid snippet is displayed (e.g., `return 0;`).

---

### User Story 2 - Edit Code and Sync Back to Graph (Priority: P1)

A user edits the code in the Code tab (e.g., changes a constant, adds a new math operation, or pastes GLSL from an online SDF example) and clicks the "Sync" button. The system parses the code, updates the underlying node graph, and if the sync succeeds, regenerates the code view from the updated graph to produce a normalized version. If the code has errors, the user sees clear diagnostics and the graph remains unchanged.

**Why this priority**: Bidirectional sync is the core value proposition. Without it, the code view is read-only and the feature loses half its usefulness, especially for the GLSL import use case.

**Independent Test**: Can be tested by modifying code in the Code tab, pressing Sync, and verifying that the node graph updates accordingly (visible when switching back to the Graph tab).

**Acceptance Scenarios**:

1. **Given** valid code in the Code tab, **When** the user clicks Sync, **Then** the graph is replaced with a new graph matching the code, and the code view is regenerated (normalized) from the resulting graph.
2. **Given** code with a syntax error (e.g., unmatched parentheses), **When** the user clicks Sync, **Then** an error message is displayed indicating the problem location, and the existing graph remains unchanged.
3. **Given** code that references a function call (e.g., `mySphere_42(pos)`), **When** the user clicks Sync, **Then** the graph creates a FunctionCall node linked to the function with resource ID 42.
4. **Given** that the user makes a change in code and syncs, **When** they switch to the Graph tab, **Then** the graph view reflects the new nodes and connections.

---

### User Story 3 - AI Agent Reads/Writes Functions via MCP (Priority: P2)

An AI agent connected via MCP requests the code representation of a function (or the entire program) and receives GLSL-like text. The agent modifies the code and sends it back, and the system updates the function graph from the new code.

**Why this priority**: This is the primary programmatic use case. AI agents find it far easier to read and write code than to manipulate graph JSON. This unblocks a much more natural workflow for LLM-based tooling.

**Independent Test**: Can be tested by calling the MCP tool to get the snippet for a function, modifying it, sending it back via the set-snippet MCP tool, and verifying the graph changed.

**Acceptance Scenarios**:

1. **Given** an existing function, **When** an AI agent calls the "get function as snippet" MCP tool with the function's resource ID, **Then** the response contains the complete GLSL-like snippet for that function.
2. **Given** a valid snippet, **When** an AI agent calls the "set function from snippet" MCP tool, **Then** the function graph is replaced accordingly and the response confirms success.
3. **Given** a snippet with errors, **When** an AI agent calls the "set function from snippet" MCP tool, **Then** the response contains a clear error message and the original graph is preserved.

---

### User Story 4 - View Entire Program as Code (Priority: P3)

A user (or AI agent) wants to see all functions in the document as a single code listing. Each function appears as a named block using its unique name. Function call nodes appear as calls to these named functions. This gives a holistic, readable view of the full implicit model.

**Why this priority**: This extends the single-function view to the full program. It is very valuable for understanding complex multi-function models but depends on single-function conversion working first.

**Independent Test**: Can be tested by requesting the "whole program" snippet for a document with multiple functions and verifying each function is present with correct cross-references.

**Acceptance Scenarios**:

1. **Given** a document with three functions (A calls B, B calls C), **When** the user requests the whole-program code view, **Then** all three functions appear in dependency order with correct call references.
2. **Given** two functions with the same display name but different resource IDs, **When** the whole-program code is generated, **Then** each function has a distinct unique name (e.g., `sphere_10`, `sphere_15`).

---

### User Story 5 - Update Entire Program from Code (Priority: P3)

An AI agent sends a complete multi-function program as code via MCP. The system parses all function definitions, creates or updates the corresponding function graphs, and wires up FunctionCall references between them.

**Why this priority**: This is the most advanced scenario, enabling full program round-trips. It depends on all previous stories and is targeted for a later version.

**Independent Test**: Can be tested by sending a multi-function snippet via MCP and verifying all functions and their cross-references are created in the document.

**Acceptance Scenarios**:

1. **Given** a multi-function code listing, **When** the agent sends it via the "set program from snippet" MCP tool, **Then** all functions are created/updated and FunctionCall nodes reference the correct targets.
2. **Given** code that removes a function that still has callers, **When** the agent sends the update, **Then** the system reports an error about unresolved references and does not apply partial changes.

---

### Edge Cases

- What happens when the graph contains node types that the code generator does not yet support? The code view emits a comment like `/* unsupported: NodeTypeName */` as a read-only indicator. Syncing code that contains such comments back to the graph is rejected with a clear error — the user must edit those nodes in the Graph view.
- What happens when a function calls itself recursively? The code generator should represent this as a recursive call using the function's own unique name, even though Gladius graphs may not support recursion at runtime.
- What happens when two functions call each other (circular dependency)? The whole-program listing rejects circular references with a clear error listing the cycle. Single-function code view is unaffected.
- What happens when the user edits code in the Code tab but switches to the Graph tab without syncing? The user should be warned about unsaved code changes.
- What happens when two functions have identical display names? Their unique code names differ because the resource ID is part of the name (e.g., `sphere_10` vs `sphere_15`).
- How does the system handle ConstantMatrix or other complex parameter types? They should be represented as constructor-like syntax (e.g., `mat4(...)`) in the code view.

## Clarifications

### Session 2026-02-23

- Q: How should display names with spaces/special characters be sanitized to produce valid identifiers? → A: Replace non-alphanumeric characters with underscores, collapse consecutive underscores (e.g., `"My Sphere!"` → `My_Sphere__42`).
- Q: What should happen when code containing unsupported-node comments is synced back to the graph? → A: Reject sync — treat unsupported-node comments as parse errors; require user to remove them or stay in graph mode for those functions.
- Q: Should code be auto-generated every time the user switches to the Code tab, or only on first open? → A: Generate on first open only; subsequent tab switches preserve the editor contents until the user explicitly syncs.
- Q: What should happen when the document contains circular function call references for whole-program listing? → A: Reject with error — report circular dependency and refuse to generate the whole-program listing until resolved.
- Q: How should the parser handle unsupported GLSL syntax (for loops, if/else, custom structs)? → A: Strict — reject with a clear error naming the unsupported construct and its line number.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST convert any single function graph to a GLSL-like code snippet that represents all nodes, links, and parameter values in the graph.
- **FR-002**: The system MUST parse a GLSL-like code snippet and produce a valid function graph from it, supporting all node types that the graph-to-code direction supports.
- **FR-003**: FunctionCall nodes MUST be represented as method calls in the code view, using a unique function name derived from the display name and resource ID (format: `displayName_resourceId`, e.g., `mySphere_42`). Non-alphanumeric characters in the display name MUST be replaced with underscores; consecutive underscores MUST be collapsed to a single underscore. The result MUST be a valid GLSL identifier.
- **FR-004**: The code syntax MUST be kept close to GLSL, using GLSL-style types (`float`, `vec3`), operators, built-in function names (`sin`, `cos`, `pow`, `clamp`, etc.), and component access (`.x`, `.y`, `.z`).
- **FR-005**: The graph MUST remain the ground truth — the code view is always a derived representation. Edits to code only take effect when the user explicitly syncs.
- **FR-006**: The Sync operation MUST be atomic: if parsing or graph construction fails, the original graph MUST remain unchanged and the user MUST receive a clear error message.
- **FR-007**: The UI MUST provide a tab-based interface in the Model Editor to switch between the Graph view (node editor) and the Code view (text editor), for the currently selected function. Code MUST be generated from the graph only when the Code tab is first opened for a function; subsequent tab switches MUST preserve the editor contents (including unsaved edits) until the user explicitly syncs.
- **FR-008**: The UI MUST provide a Sync button (in the Code tab) that parses the code, updates the graph, and regenerates the normalized code from the resulting graph.
- **FR-009**: The system MUST provide an MCP tool to retrieve the code snippet for a given function by resource ID.
- **FR-010**: The system MUST provide an MCP tool to update a function graph from a code snippet.
- **FR-011**: Node types not yet supported by the code generator MUST be emitted as structured comments (e.g., `/* unsupported: NodeTypeName */`) rather than silently dropped. If such comments are present when the user syncs code back to the graph, the sync MUST be rejected with a clear error listing the unsupported node types. The user must resolve these in the Graph view.
- **FR-012**: The code generator MUST handle intermediate variables (nodes with fan-out > 1) by assigning them to named variables to avoid duplicate computation in the code.
- **FR-013**: The system MUST support converting the entire document (all functions) to a single code listing (whole-program view) with each function as a named block. If circular function call references are detected (e.g., A calls B, B calls A), the system MUST reject the request with a clear error listing the cycle, rather than producing an incomplete or arbitrarily ordered listing.
- **FR-014**: The system MUST provide an MCP tool to retrieve the whole-program code listing.
- **FR-015**: If the user has unsaved changes in the Code tab and attempts to switch to the Graph tab, the system MUST warn about unsaved changes.
- **FR-016**: The snippet parser MUST use strict parsing: if the code contains GLSL constructs not supported by the graph model (e.g., `for` loops, `if/else` branching, custom `struct` types), the sync MUST be rejected with a clear error naming the unsupported construct and its line number.

### Key Entities

- **Function Graph**: The node-based representation of a single function. Contains Begin/End nodes, operation nodes, links, and parameter values. This is the source of truth.
- **Code Snippet**: A GLSL-like text representation of a single function graph. Derived from the graph and can be parsed back into one.
- **Unique Function Name**: A name generated from a function's display name and resource ID, used to identify functions in code (e.g., `gyroid_42`). Must be valid as an identifier (alphanumeric + underscore).
- **Whole-Program Listing**: A concatenation of all function snippets in dependency order, representing the entire document as code.

## Assumptions

- The existing `ExpressionToGraphConverter` (specifically `convertSnippetToGraph` and `convertGraphToSnippet`) provides a strong foundation that will be extended to support all node types (currently it handles math operations, constants, Begin/End, DecomposeVector, and ComposeVector but not FunctionCall or all node categories).
- The "pos" vec3 argument is implicitly available in all functions as a spatial coordinate, consistent with current Gladius behavior.
- Functions with the same display name but different resource IDs are distinct and will receive distinct unique code names.
- Round-trip fidelity is defined as: `graph → code → graph → code` produces identical code on the second step. The intermediate graphs may differ in internal IDs but must be functionally equivalent.
- The code editor in the UI will be a plain text editor with basic syntax highlighting; a full IDE experience (autocomplete, inline errors) is out of scope for V1.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of node types present in any test graph can be converted to code and back without loss of semantics (round-trip fidelity).
- **SC-002**: Users can switch between Graph and Code tabs and sync changes within 2 seconds for graphs with up to 100 nodes.
- **SC-003**: AI agents can read and modify functions via the MCP snippet tools, completing a typical SDF creation task (e.g., creating a gyroid function from GLSL code) in a single tool-call round-trip.
- **SC-004**: At least 90% of common GLSL SDF examples (sphere, torus, gyroid, box, smooth union) can be pasted into the Code tab and successfully synced to a valid graph.
- **SC-005**: The whole-program listing for a document with 10+ functions is generated in under 3 seconds.
- **SC-006**: All graph modifications made through the Code tab or MCP snippet tools produce valid, compilable function graphs (no dangling links or type mismatches).

---

## Implementation Notes

**Status**: Implemented  
**Branch**: `021-graph-code-view`  
**Tests**: 840 total (834 passed, 5 skipped, 1 pre-existing disabled)

### Files Created
- `gladius/src/ui/CodeView.h` / `.cpp` — Code editor widget with per-function buffers, sync, dirty detection
- `gladius/tests/unittests/GraphToSnippet_tests.cpp` — Converter engine + program snippet tests
- `gladius/tests/unittests/MCPSnippetTool_tests.cpp` — MCP tool integration tests

### Files Modified
- `gladius/src/ExpressionToGraphConverter.h/.cpp` — `generateUniqueFunctionName`, `convertProgramToSnippet`, `setProgramSnippet`, extended `nodeToExpression` for vector/matrix/FunctionCall/resource nodes
- `gladius/src/ui/ModelEditor.h/.cpp` — Tab bar with Graph/Code modes, unsaved-changes popup
- `gladius/src/mcp/tools/FunctionOperationsTool.h/.cpp` — `getFunctionSnippet`, `setFunctionSnippet`, `getProgramSnippet`, `setProgramSnippet`
- `gladius/src/mcp/ApplicationMCPAdapter.h/.cpp` — Delegation methods for all 4 snippet tools
- `gladius/src/mcp/MCPApplicationInterface.h` — Virtual interface declarations
- `gladius/src/mcp/MCPServer.cpp` — Tool registrations: `get_function_snippet`, `set_function_snippet`, `get_program_snippet`, `set_program_snippet`

### Key Design Decisions (implemented)
- **Name generation**: `sanitize(displayName)_resourceId` produces unique, valid GLSL identifiers
- **Program ordering**: Kahn's algorithm topological sort on FunctionCall/FunctionGradient/NormalizeDistanceField dependency DAG
- **Cycle detection**: Throws `std::runtime_error` with descriptive message listing involved functions
- **Two-pass program parsing**: First pass creates all models, second pass parses bodies (enables forward references)
- **Dangling reference validation**: After parsing, verifies all FunctionCall targets exist in the assembly
