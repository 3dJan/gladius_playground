# Feature Specification: MCP Snippet Tool Extensions

**Feature Branch**: `022-mcp-snippet-extensions`  
**Created**: 2026-02-26  
**Status**: Draft  
**Input**: User description: "We want to improve the function creation tools and the bidirectional conversion between graph and code. The latest addition of a snippet tool for function creation improved the MCP tool a lot, because communicating math expressions through code is much easier for an LLM when accessing a function as a graph. In the end we want to replace all the graph-based MCP tools: We now want to extend the snippet tools to allow the definition of the function arguments (names, types). We also need a way to query the code for the whole assembly, so that an agent can see the flow between the functions."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Get Full Function Signature via Snippet Tool (Priority: P1)

An AI agent queries a function's code representation and receives the complete function signature — including the argument names, their types, and the output type — alongside the snippet body. This allows the agent to understand a function's interface without inspecting the graph JSON or guessing parameter details.

**Why this priority**: Without the full signature in the snippet response, agents cannot reliably round-trip functions. They must guess or separately query the argument list, which makes every subsequent tool call fragile. This is the minimum fix to enable reliable automated workflows.

**Independent Test**: Can be tested by calling `get_function_snippet` for a function with known arguments and verifying the response includes `arguments` and `output_type` fields matching the function's actual inputs and output.

**Acceptance Scenarios**:

1. **Given** a function with arguments `pos: vec3` and output type `float`, **When** an agent calls the get-function-snippet tool, **Then** the response includes `arguments: [{name: "pos", type: "vec3"}]` and `output_type: "float"` alongside the snippet text.
2. **Given** a function with multiple arguments `pos: vec3, radius: float`, **When** an agent calls the get-function-snippet tool, **Then** all arguments are returned with correct names and types.
3. **Given** a function with no explicit arguments (only the implicit Begin node), **When** an agent calls the get-function-snippet tool, **Then** the arguments array is empty and the output type is still returned.

---

### User Story 2 - Define Function Arguments via Snippet Tools (Priority: P1)

An AI agent creates or updates a function and specifies the function arguments (names and types) as part of the snippet tool call. The system creates corresponding input parameter nodes in the graph. When modifying an existing function, the agent can add, remove, or rename arguments through the snippet tool without touching the graph directly.

**Why this priority**: Defining function arguments is essential for creating parameterized functions (e.g., an SDF that takes a position and a radius). Without this, agents are limited to hard-coded constants or must fall back to low-level graph tools to wire up inputs.

**Independent Test**: Can be tested by calling `set_function_snippet` with an explicit arguments list and verifying the resulting graph has matching input parameter nodes on the Begin node.

**Acceptance Scenarios**:

1. **Given** a snippet `return length(pos) - radius;` with arguments `[{name: "pos", type: "vec3"}, {name: "radius", type: "float"}]`, **When** an agent calls the set-function-snippet tool, **Then** the graph's Begin node has two output ports: `pos` (vec3) and `radius` (float).
2. **Given** an existing function with argument `pos: vec3`, **When** an agent calls set-function-snippet with arguments `[{name: "pos", type: "vec3"}, {name: "scale", type: "float"}]`, **Then** the graph is updated to include both arguments, and the new `scale` argument is available as an input node.
3. **Given** a snippet that references a variable not listed in the arguments, **When** an agent calls set-function-snippet, **Then** the system returns a clear error identifying the undefined variable.

---

### User Story 3 - Query Assembly-Level Code View (Priority: P1)

An AI agent queries the entire assembly (document) as a code listing and receives all functions in dependency order, including their full signatures and call relationships. This gives the agent a complete overview of the program's data flow — which functions exist, what they accept as input, what they return, and how they call each other.

**Why this priority**: Understanding the flow between functions is critical for any non-trivial modeling task. Without an assembly-level view, agents must query functions one by one and manually reconstruct the dependency graph, which is slow and error-prone.

**Independent Test**: Can be tested by calling `get_program_snippet` on a document with multiple interconnected functions and verifying the response contains all functions with correct signatures and call references in dependency order.

**Acceptance Scenarios**:

1. **Given** a document with functions `sphere(pos: vec3) → float` and `shell(pos: vec3) → float` where shell calls sphere, **When** an agent calls the get-program-snippet tool, **Then** the response lists sphere before shell, and the shell body contains a call to the sphere function using its unique name.
2. **Given** a document with 5+ functions forming a dependency DAG, **When** an agent calls get-program-snippet, **Then** each function listing includes its full signature (arguments with names and types, output type) in the function header.
3. **Given** a document with build items (objects referencing functions), **When** an agent calls get-program-snippet, **Then** the response includes metadata indicating which functions are used as root shapes in the scene.

---

### User Story 4 - Update Assembly-Level Code (Priority: P2)

An AI agent sends a modified multi-function program back to the system via the set-program-snippet tool. The agent can add new functions, modify existing ones, change function signatures (argument names, types), and update cross-function call references — all through a single code submission. The system parses all function definitions, validates the dependency graph, and applies changes atomically.

**Why this priority**: Modifying the full program through code is the end-goal workflow that makes graph-based tools unnecessary for AI agents. It depends on the assembly query (Story 3) and argument handling (Stories 1–2) being in place first.

**Independent Test**: Can be tested by retrieving a program snippet, modifying a function's body and arguments, sending it back, and verifying the graph reflects all changes.

**Acceptance Scenarios**:

1. **Given** a program snippet with function `sphere(pos: vec3) → float`, **When** an agent modifies its arguments to `sphere(pos: vec3, radius: float) → float` and sends the updated program, **Then** the sphere function's graph is updated with a new `radius` input parameter node.
2. **Given** a program snippet, **When** an agent adds a new function definition to the code and sends it back, **Then** the system creates a new function resource in the document with the corresponding graph.
3. **Given** a program snippet where an agent removes a function that is still called by another function, **When** the agent sends the update, **Then** the system rejects the change with an error about unresolved references and leaves the document unchanged.

---

### User Story 5 - Deprecate Graph-Based MCP Tools (Priority: P3)

Once the snippet tools fully cover function creation, modification, argument management, and assembly-level operations, the low-level graph manipulation tools (`create_node`, `delete_node`, `create_link`, `delete_link`, `set_parameter_value`, `create_function_call_node`) are marked as deprecated. New AI agent workflows use only snippet-based tools. The deprecated tools remain functional for backward compatibility but are hidden from tool discovery by default.

**Why this priority**: This is the strategic goal that drives all preceding stories. It simplifies the MCP tool surface for AI agents from ~10 graph tools to ~5 snippet tools. It depends on the snippet tools being feature-complete first.

**Independent Test**: Can be tested by verifying that every operation previously requiring graph tools can be accomplished through snippet tools alone, and that the deprecated tools still function when explicitly called.

**Acceptance Scenarios**:

1. **Given** the snippet tools support all function creation and modification operations, **When** an agent queries available tools, **Then** the graph manipulation tools are marked as deprecated in their descriptions.
2. **Given** an agent that previously used `create_node` + `create_link` to build a function, **When** the same operation is attempted via `create_function_from_snippet` with arguments, **Then** the result is functionally identical.
3. **Given** backward-compatible code using deprecated tools, **When** those tools are called, **Then** they still function correctly and return results with a deprecation notice.

---

### Edge Cases

- What happens when a function's arguments change type (e.g., `radius: float` becomes `radius: vec3`) during a set-snippet call? The system must rebuild the argument nodes and any downstream links that are now type-incompatible must be reported as errors.
- What happens when an agent provides arguments in the snippet tool that don't match the variables used in the snippet body? The system must report undefined variables or unused arguments as warnings or errors.
- What happens when get-program-snippet is called on an empty document with no functions? The system returns an empty program listing (no functions) without error.
- What happens when the assembly contains functions that are not reachable from any build item? They are still included in the program listing, marked as unused/orphan.
- What happens when two functions are defined with the same unique name in a set-program-snippet call? The system rejects the submission with a duplicate name error.
- What happens when a function argument name conflicts with a GLSL keyword or built-in function name? The system rejects the argument name with a clear error message.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The `get_function_snippet` tool MUST return the function's `arguments` (as an array of name-type pairs) and `output_type` alongside the snippet text, enabling complete round-trip workflows without external queries.
- **FR-002**: The `set_function_snippet` and `create_function_from_snippet` tools MUST accept an `arguments` parameter that defines function input parameters (name and type for each), and create corresponding input parameter nodes in the function graph.
- **FR-003**: The `get_program_snippet` tool MUST return all functions in the document with their full signatures (argument names, argument types, output type) in the function header comments or declarations.
- **FR-004**: The program snippet format MUST include metadata indicating which functions serve as root shapes (are referenced by build items in the scene), so agents can understand the assembly structure.
- **FR-005**: The `set_program_snippet` tool MUST support adding new functions, modifying existing function bodies and signatures, and updating cross-function call references in a single atomic operation.
- **FR-006**: When a function's arguments are changed via snippet tools (added, removed, renamed, or type-changed), the system MUST update the graph's Begin node accordingly and report any downstream type incompatibilities as errors.
- **FR-007**: The snippet tools MUST validate that all variables referenced in a function body are either defined locally (as intermediate variables) or declared as function arguments. Undefined variable references MUST produce clear error messages.
- **FR-008**: The program snippet format MUST list functions in topological dependency order so that each function appears after all functions it depends on (callees before callers).
- **FR-009**: Argument names MUST be validated against reserved keywords and built-in function names. Invalid names MUST be rejected with a descriptive error.
- **FR-010**: The graph-based MCP tools (`create_node`, `delete_node`, `create_link`, `delete_link`, `set_parameter_value`, `create_function_call_node`, `createConstantNodesForMissingParameters`) MUST be marked as deprecated once snippet tools provide equivalent coverage.
- **FR-011**: Deprecated tools MUST remain functional for backward compatibility but SHOULD include a deprecation notice in their response, pointing users to the snippet-based alternative.
- **FR-012**: The `get_program_snippet` response MUST include functions that are not referenced by any build item, clearly distinguishable from root/active functions (e.g., via a comment annotation).

### Key Entities

- **Function Signature**: The combination of a function's unique name, its input arguments (each with a name and type), and its output type. This is the contract that callers depend on.
- **Assembly**: The complete set of functions in a document, their dependency relationships (call graph), and the mapping from build items to root functions. Represents the full program.
- **Build Item Root**: A function that is directly referenced by a build item (scene object). These are the entry points of the implicit model.
- **Argument**: A named, typed input to a function. Corresponds to an output port on the function graph's Begin node. Supported types: `float`, `vec3`.

## Assumptions

- Feature 021 (Graph ↔ Code View) is fully implemented and provides the foundation: graph-to-snippet conversion, snippet-to-graph parsing, program-level snippet tools, unique function name generation, and topological ordering.
- The existing `arguments` parameter in `set_function_snippet` and `create_function_from_snippet` already works for specifying arguments during write operations — the gap is primarily on the read side (`get_function_snippet` not returning them) and in program-level signatures.
- The `pos: vec3` argument remains the most common input pattern for SDF functions, but functions may have arbitrary argument lists (e.g., `pos: vec3, radius: float, height: float`).
- Argument types are limited to `float` and `vec3` for the initial implementation, matching the current graph model's type system.
- Deprecation of graph-based tools is a phased process: tools are first annotated, then hidden from default discovery, and eventually removed in a future major version. This spec covers the annotation phase only.
- The program snippet format already uses comment headers (e.g., `// Function: name (ID: 42)`) that can be extended to include signature information and build-item annotations.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: An AI agent can retrieve a function's complete signature (arguments + output type) and resubmit it unchanged without loss — 100% round-trip fidelity for function signatures.
- **SC-002**: An AI agent can create a parameterized function (e.g., a sphere SDF with `pos: vec3` and `radius: float` arguments) in a single tool call via `create_function_from_snippet`.
- **SC-003**: An AI agent can read the full assembly as code, understand all function signatures and their call relationships, and make targeted modifications — all without using any graph-based tool.
- **SC-004**: The program snippet includes clear annotations for which functions are build-item roots and which are helper/utility functions, enabling agents to understand the scene structure at a glance.
- **SC-005**: All operations previously requiring graph-based tools (creating nodes, linking, setting parameters) can be accomplished through snippet tools alone for standard SDF modeling workflows.
- **SC-006**: 100% of existing MCP integration tests continue to pass with deprecated graph tools, ensuring backward compatibility.
