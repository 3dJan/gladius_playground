# Feature Specification: MCP Agent UX Improvements

**Feature Branch**: `024-mcp-agent-ux`  
**Created**: 2026-03-17  
**Status**: Clarified  
**Input**: Improve the Gladius MCP server to make it easier for agents to work with it in headless and UI mode: remove unnecessary tools, improve existing tools, enable agents to load/create/extend 3MF files, inspect and extend the library, evaluate functions via rendering or point sampling, support collaborative human+agent editing with change notifications, and update the MCP skills.

## Background & Motivation

The Gladius MCP server allows AI agents to create and manipulate 3MF volumetric models using implicit modeling (SDF functions). Several rounds of tool additions have left the API with gaps, redundancies and rough edges that force agents into multi-step workarounds, blind iteration (no numerical feedback), and inability to safely collaborate with a human working in the UI simultaneously.

This feature addresses those problems holistically: consolidating and sharpening the API surface, adding a function-evaluation capability for iterative feedback, exposing richer library metadata, and giving agents a view into user-driven changes.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Complete Function Round-Trip (Priority: P1)

An agent reads an existing function's code, modifies it, writes it back, and verifies the change — all without losing the function signature (argument names, types, output type).

**Why this priority**: This is the core authoring loop. Currently `get_function_snippet` returns only the snippet body, omitting `arguments` and `output_type`, so agents cannot safely re-submit a modified function without guessing the signature or performing extra queries. This breaks the most fundamental workflow.

**Independent Test**: Agent calls get-snippet → mutates snippet text → calls set-snippet with the returned metadata → result compiles without error. Can be tested end-to-end without UI or library.

**Acceptance Scenarios**:

1. **Given** a function exists in the document, **When** the agent calls `get_function_snippet`, **Then** the response includes `snippet`, `arguments` (array of `{name, type}`) and `output_type`, matching the format accepted by `set_function_snippet`.
2. **Given** the agent has the response from `get_function_snippet`, **When** it passes `arguments` and `output_type` unchanged to `set_function_snippet` with a modified `snippet`, **Then** the function compiles and the model renders correctly.
3. **Given** `get_program_snippet` is called, **When** the agent calls `set_program_snippet` with the unmodified output, **Then** the operation succeeds with no functional changes (lossless round-trip).

---

### User Story 2 — Numerical Function Evaluation (Priority: P1)

An agent evaluates a function at a set of input points and receives numeric output values, enabling it to verify correctness and iterate without depending on visual rendering.

**Why this priority**: Rendering takes several seconds and produces a picture the agent must interpret. For debugging signed-distance functions, the agent needs exact values at specific points (e.g., is the surface at the origin? is the gradient unit-length?). This is the missing feedback channel that currently forces expensive render-and-inspect cycles.

**Independent Test**: Agent creates a sphere SDF (`length(pos) - 1.0`) and evaluates it at `[0,0,0]` (expects ≈ –1), `[1,0,0]` (expects ≈ 0), `[2,0,0]` (expects ≈ +1). No rendering required.

**Acceptance Scenarios**:

1. **Given** a function with a `vec3` input named `pos` and `float` output, **When** the agent calls `evaluate_function` with a list of `vec3` sample points, **Then** the response contains one float value per point.
2. **Given** a function that accepts multiple named arguments, **When** the agent provides a list of argument-value maps, **Then** the response contains one output value per sample.
3. **Given** an invalid function (compile error), **When** `evaluate_function` is called, **Then** the response returns a structured error with a human-readable message, not a crash or silent wrong value.
4. **Given** a batch of sample points, **When** the agent evaluates, **Then** all results are returned before the MCP request times out and the response contains one output value per sample.

---

### User Story 3 — Rich Document Inspection (Priority: P1)

An agent opens an existing 3MF file and immediately understands what it contains: build items, functions with their signatures, parameter values, and snippet previews — without prior knowledge of the file's structure.

**Why this priority**: Agents regularly need to extend or audit existing models. The current `get_3mf_structure` returns identifiers but not function signatures; agents must follow up with multiple individual calls to build a usable picture.

**Independent Test**: Agent opens any example 3MF and with a single query obtains function names, argument lists, output types, and current parameter values — enough to understand the model and plan edits.

**Acceptance Scenarios**:

1. **Given** a 3MF document is open, **When** `get_3mf_structure` is called, **Then** each function resource includes `display_name`, `arguments`, `output_type`, and a `snippet_preview` (first 3 lines of the snippet or the full snippet if shorter).
2. **Given** the document contains constant nodes, **When** the structure is inspected, **Then** their current numeric values are included in the function's node list.
3. **Given** a document with zero functions, **When** the structure is inspected, **Then** a `functions: []` entry appears and the response is valid.

---

### User Story 4 — Library Discovery and Metadata Search (Priority: P2)

An agent searching for a sphere primitive or a smooth-union combinator queries the library with keywords and receives matching entries with full signatures, so it can import or reference them without browsing the entire catalog.

**Why this priority**: Without search, agents must list every entry and reason about names alone — inefficient and error-prone for large libraries.

**Independent Test**: Agent calls `list_library` with `query: "sphere"` and receives only matching entries. Agent imports one entry and confirms the function appears in the document.

**Acceptance Scenarios**:

1. **Given** the library contains at least one entry tagged or named "sphere", **When** `list_library` is called with a keyword filter, **Then** only matching entries are returned.
2. **Given** a library entry, **When** `get_library_entry_info` is called, **Then** the response includes full `arguments`, `output_type`, `description`, `tags`, and `snippet`.
3. **Given** no entries match the keyword, **When** `list_library` is called, **Then** an empty list is returned without error.
4. **Given** an entry has no tags, **When** `set_library_metadata` is called with a `tags` list, **Then** tags are persisted and returned by subsequent `get_library_entry_info` calls.

---

### User Story 5 — One-Step Library Entry Creation from Snippet (Priority: P2)

An agent creates a reusable library entry from a multi-line snippet in a single tool call, without manually creating a function, levelset, metadata, and saving in separate steps.

**Why this priority**: The current four-step workaround is fragile and verbose. A single call that accepts snippets reduces errors and cognitive overhead significantly.

**Independent Test**: Agent calls `create_library_entry` with a `snippet` parameter and a multi-line SDF. The entry appears in the library and can be imported and rendered.

**Acceptance Scenarios**:

1. **Given** a valid multi-line snippet and argument list, **When** `create_library_entry` is called with `snippet`, **Then** the entry is created and persisted in the library directory.
2. **Given** the `expression` parameter is used (legacy), **When** `create_library_entry` is called, **Then** existing behavior is preserved (backward compatible).
3. **Given** the snippet contains a syntax error, **When** `create_library_entry` is called, **Then** the error message identifies the problem; no partial entry is written to disk.

---

### User Story 6 — Collaborative Editing with Change Notifications (Priority: P2)

While an agent and a human work on the same model simultaneously in UI mode, the agent can ask "what has changed since I last looked?" and receive a structured summary of the user's edits, so it can adapt its next actions accordingly.

**Why this priority**: Without change awareness, agents and humans can overwrite each other's work silently. A lightweight change-query lets agents stay in sync without aggressive polling or document locking.

**Independent Test**: Human changes a constant value in the node editor, then the agent calls `get_changes_since` with the timestamp before the edit. The response lists the changed node and its new value.

**Acceptance Scenarios**:

1. **Given** the user modifies a parameter value in the UI, **When** the agent calls `get_changes_since` with a prior timestamp, **Then** the response includes a changelog entry identifying the affected resource (type, ID, name) with `type: modified`. The entry does not include a parameter-level diff; the agent calls `get_function_snippet` or equivalent to inspect changes.
2. **Given** no changes were made since the timestamp, **When** `get_changes_since` is called, **Then** an empty changes list is returned, not an error.
3. **Given** the user adds a new function to the document, **When** `get_changes_since` is called, **Then** the new function's name and resource ID appear in the additions list.
4. **Given** headless mode (no user), **When** `get_changes_since` is called, **Then** the tool returns correctly, reflecting only agent-initiated changes if any.

---

### User Story 7 — API Surface Cleanup (Priority: P3)

Developers and agents encounter a clean, minimal MCP tool set: each tool has a single clear purpose, debug-only tools are removed, and all descriptions and schemas are complete and accurate.

**Why this priority**: Tool-list bloat increases cost per `tools/list` call and increases the chance an agent selects the wrong tool. Removing noise reduces agent errors.

**Independent Test**: After cleanup, `tools/list` returns no debug-only tools. All remaining tools have non-empty descriptions and complete input schemas.

**Acceptance Scenarios**:

1. **Given** the MCP server starts, **When** `tools/list` is called, **Then** no debug-only tools (`ping`, `test_computation`, `list_tools`) appear in the response.
2. **Given** any tool is called with missing required parameters, **When** the call is made, **Then** the error response includes a `usage_example` JSON fragment.
3. **Given** a tool call succeeds, **When** the response arrives, **Then** it contains a `success: true` top-level field for easy programmatic checking.

---

### User Story 8 — Updated MCP Skills Documentation (Priority: P3)

The existing MCP-related agent skills are updated to reflect the improved tool set, so agents loaded with those skills use new tools correctly from the start.

**Why this priority**: Outdated skills lead agents to use deprecated workarounds. Skills are the primary knowledge-transfer mechanism for agent capabilities.

**Independent Test**: An agent loaded only with the updated skill can create, inspect, evaluate, and export a library entry without consulting additional sources or hitting deprecated endpoints.

**Acceptance Scenarios**:

1. **Given** the updated `creating-library-items` skill, **When** an agent follows its instructions to create a snippet-based library entry, **Then** it succeeds with the new one-step `create_library_entry` call.
2. **Given** the skill documents `evaluate_function`, **When** an agent follows the evaluation workflow, **Then** it receives numeric results without needing rendering.
3. **Given** the skill documents `get_changes_since`, **When** an agent follows the collaboration workflow, **Then** it correctly interprets the change summary and avoids overwriting user edits.

---

### Edge Cases

- What happens when `evaluate_function` is called on a function with a runtime fault (e.g., division by zero in the SDF evaluation)? → Resolved: the result for the faulting sample point is `null`, and a `warnings` array lists the affected indices (FR-006b).
- How does the change log behave if the document is closed and reopened — does history reset?
- If `set_function_snippet` is called while the user is actively editing the same function in the code view, the last write wins. No conflict detection is specified; agents are expected to call `get_changes_since` before writing to detect recent user activity.
- What if the library directory is unwritable when `create_library_entry` is called?
- If thumbnail rendering fails during `create_library_entry` (e.g., the function evaluates to an empty or degenerate surface), the entry MUST NOT be written; the response MUST include `success: false`, `reason: "thumbnail_render_failed"`, and a `message` with guidance (e.g., confirm the function produces a non-empty SDF before adding to the library).
- If the bounding box is invalid (e.g., degenerate, NaN extents, or zero volume) during `create_library_entry`, the entry MUST NOT be written; the response MUST include `success: false`, `reason: "invalid_bounding_box"`, and a `message` explaining the issue.
- How does `get_changes_since` handle a timestamp that predates the current session (before the document was opened)?
- How does `evaluate_function` handle `vec3` output functions (color, displacement)?
- What happens when two parameters share the same name across different functions in `get_program_snippet`?

---

## Requirements *(mandatory)*

### Functional Requirements

#### Function Round-Trip
- **FR-001**: `get_function_snippet` MUST return `snippet`, `arguments` (array of `{name, type}` objects), and `output_type` in every successful response.
- **FR-002**: `get_function_snippet` and `set_function_snippet` MUST use the same schema for `arguments` and `output_type` (symmetric interface).
- **FR-003**: `get_program_snippet` / `set_program_snippet` MUST perform a lossless round-trip for any syntactically valid program.

#### Function Evaluation
- **FR-004**: A new `evaluate_function` tool MUST accept a `function_id` and a list of input sample records (one per evaluation), returning a corresponding list of output values.
- **FR-005**: Each sample record MUST allow named arguments matching the function's parameter names and types (`float` scalars and `vec3` arrays).
- **FR-006**: `evaluate_function` MUST return a structured error (not an uncaught exception) if the function has a compile error or if argument types are mismatched.
- **FR-006b**: If a sample point causes a runtime fault during OpenCL evaluation (e.g., division by zero producing NaN/Inf), the result for that point MUST be `null` in the results array and a top-level `warnings` array MUST list the affected sample indices.
- **FR-007**: `evaluate_function` MUST support `float` and `vec3` output types, and MUST evaluate using the existing OpenCL compute infrastructure so that inter-function calls and resource references are resolved correctly.

#### Document Inspection
- **FR-008**: `get_3mf_structure` MUST include `arguments`, `output_type`, and a `snippet_preview` (≥ 3 lines or full snippet if shorter) for every function resource.
- **FR-009**: `get_3mf_structure` MUST include current parameter values for constant nodes in each function's graph.

#### Library
- **FR-010**: `list_library` MUST accept an optional `query` string; when provided, only entries whose name, description, or tags contain the query text (case-insensitive) are returned.
- **FR-011**: `get_library_entry_info` MUST return `arguments`, `output_type`, `description`, `tags`, and `snippet` for the named entry.
- **FR-012**: `set_library_metadata` MUST accept and persist a `tags` list (array of strings) alongside `description`.
- **FR-013**: `create_library_entry` MUST accept either `expression` (simple) or `snippet` + `arguments` (complex) as mutually exclusive options.
- **FR-014**: `create_library_entry` MUST be atomic: no partial file is written if the operation fails mid-way.
- **FR-014b**: `create_library_entry` MUST automatically render a thumbnail and compute the bounding box before completing. If either step fails, the entry MUST NOT be written to the library and the response MUST return `success: false` with a `reason` field that clearly identifies which step failed (e.g., `"thumbnail_render_failed"`, `"invalid_bounding_box"`) and a human-readable `message` explaining the likely cause and how the caller might resolve it. The caller should expect a longer response time on success; no separate thumbnail call is required.

#### Collaborative Editing
- **FR-015**: A new `get_changes_since` tool MUST accept a `since` timestamp (ISO 8601 string) and return a structured list of additions, modifications, and deletions that occurred in the open document after that time.
- **FR-016**: Each change entry MUST include `type` (added/modified/deleted), `resource_type`, `resource_id`, and `display_name`. No parameter-level diff is included; the agent re-inspects the resource via existing tools if needed.
- **FR-017**: `get_changes_since` MUST return an empty `changes` list (not an error) when no changes occurred.
- **FR-018**: `get_changes_since` MUST work correctly in headless mode.
- **FR-019**: `get_status` MUST include a `server_time` field (ISO 8601 UTC) to allow agents to obtain a synchronised timestamp for subsequent `get_changes_since` calls.

#### API Cleanup
- **FR-020**: The tools `ping`, `test_computation`, and `list_tools` MUST be removed from the registered tool set.
- **FR-021**: Every tool's error response MUST include `success: false` and a `usage_example` JSON fragment.
- **FR-022**: Every tool's success response MUST include `success: true`.

#### Skills
- **FR-023**: The `creating-library-items` skill MUST be updated to reflect snippet-based creation, `evaluate_function` usage, and the `get_changes_since` collaboration workflow.
- **FR-024**: The updated skill MUST include a complete tool inventory table with one-line descriptions.

### Key Entities

- **Function**: A named SDF or color/displacement function defined by a node graph and representable as a code snippet. Has `arguments` (typed named inputs) and an `output_type`.
- **LibraryEntry**: A 3MF file stored in the user library directory containing one or more tagged functions with metadata (`description`, `tags`, `arguments`, `output_type`).
- **ChangeRecord**: A timestamped event recording an addition, modification, or deletion of a resource within an open document.
- **SamplePoint**: A set of named argument values passed to `evaluate_function`; corresponds to one row of inputs to the shader-like function.
- **EvaluationResult**: The output of `evaluate_function` for one `SamplePoint`; a scalar (`float`) or vector (`vec3`) value.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: An agent can read, modify, and write back any function's complete snippet (body + signature) in ≤ 3 tool calls, with zero silent data loss.
- **SC-002**: An agent can verify an SDF's geometric correctness by numerical evaluation via OpenCL, receiving correct output values for an arbitrary batch of sample points without a full render pass.
- **SC-003**: An agent can discover a relevant library function by keyword in ≤ 2 tool calls (search + inspect), without iterating over the full catalog.
- **SC-004**: An agent collaborating with a human in UI mode can detect all user changes to the open document using a single `get_changes_since` call within 1 second of the changes occurring.
- **SC-005**: The `tools/list` response contains no debug-only tools; all remaining tools have descriptions and schemas sufficient for an agent to call them correctly on first attempt without additional documentation.
- **SC-006**: An end-to-end workflow (open file → inspect → evaluate → modify → save → library export) completes via MCP calls alone in both headless and UI modes, without requiring any manual UI action.
- **SC-007**: The updated skill file enables a newly loaded agent to complete the creating-library-items workflow without consulting any source outside the skill file.

---

## Clarifications

### Session 2026-03-17

- Q: What backend should `evaluate_function` use — expression interpreter, native CPU kernel, or OpenCL? → A: OpenCL, reusing the existing GPU compute infrastructure. Functions may call other functions and reference shared resources; the OpenCL path already handles these dependencies correctly and is the simplest approach.
- Q: How granular should `get_changes_since` be — per-parameter, per-resource, or coarse document-level? → A: Per-resource. The response is a changelog listing which resources (functions, build items, etc.) were touched since the given timestamp. No parameter-level diff is included; it is the agent's responsibility to re-inspect any resource it considers relevant.
- Q: What is "reference hardware" for the 5-second evaluation performance target in SC-002? → A: Drop the time requirement entirely. Evaluation time depends too heavily on hardware and model complexity to express as a meaningful spec target.
- Q: If `set_function_snippet` is called while the user is editing the same function, what wins? → A: Last-writer-wins. No conflict detection is implemented. Agents should call `get_changes_since` before writing to detect user activity and decide whether to proceed.
- Q: Should `create_library_entry` auto-generate a render thumbnail, or is thumbnail generation opt-in via a separate call? → A: Auto-generate synchronously. Adding to the library is a final step in component creation; a delayed response is acceptable. A separate explicit thumbnail call is not required.

---

## Assumptions

- `evaluate_function` MUST use the existing OpenCL compute infrastructure (same path used for rendering/meshing), so that function-call graphs and resource dependencies resolve correctly without additional interpreter implementation.
- Change tracking records events from the moment the document is opened in the current session; history does not persist across file reloads.
- Timestamps for `get_changes_since` use ISO 8601 UTC; `get_status` provides a `server_time` field for clock synchronisation.
- Tags are stored in the 3MF file's existing metadata extension; no new file format is introduced.
- `evaluate_function` supports `float` and `vec3` output types initially; other types are out of scope.
- Concurrent multi-agent access is out of scope; single-agent + single-user collaboration is the target.
- Backward compatibility with existing tool names and schemas is required where not explicitly listed for removal.
