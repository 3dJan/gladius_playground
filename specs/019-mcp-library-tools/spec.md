# Feature Specification: MCP Library Tools — Agent-Driven SDF Library Authoring

**Feature Branch**: `019-mcp-library-tools`  
**Created**: 2026-02-15  
**Status**: Draft  
**Input**: User description: "Make Gladius MCP server usable to cover the following use case: an agent should be able to extend Gladius library of implicit/SDF functions (stored as 3MF) using the MCP tools (e.g. primitives, TPMS structures) and also be able to query what functions are already present in the library. Consider that the tools need to be easy to understand for the agent, and that they should return helpful instructions with short examples when used incorrectly. The complete set of tools should be comprehensive and versatile. To make the MCP server usable, consider the previous findings."

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Browse and Discover Library Contents (Priority: P1)

An AI agent connects to the Gladius MCP server and asks "what library entries exist?" The system returns a structured listing of all categories (subdirectories) and their entries, including each entry's name, description, and available functions. The agent can then drill into a specific entry to inspect its function graphs, understand the parameters, and decide whether to reuse or extend it.

**Why this priority**: Discovery is the foundation — the agent cannot extend the library if it cannot see what already exists. This prevents duplicate work and enables informed composition.

**Independent Test**: Can be fully tested by calling the listing and inspection tools against a library directory containing at least two categories with entries, and verifying the returned structure matches the file system contents.

**Acceptance Scenarios**:

1. **Given** a library directory with categories "primitives" and "lattices" containing 3MF files, **When** the agent calls `list_library`, **Then** the response contains both category names and the entries within each, including file name and description metadata.
2. **Given** a library entry "gyroid.3mf" with metadata tagging function IDs 5 and 12, **When** the agent calls `list_library` with a category filter, **Then** the response includes the tagged function IDs and the description for that entry.
3. **Given** the agent wants to inspect a library entry's function graph, **When** the agent calls `get_library_entry_info` with the category and entry name, **Then** the response includes the entry's full function list with names, types, input/output signatures, and the description metadata.
4. **Given** the agent calls `list_library` with an invalid category name, **Then** the response includes a clear error message listing available categories and a usage example.

---

### User Story 2 — Create a New Library Entry from an Expression (Priority: P1)

An AI agent creates a new SDF primitive or TPMS structure by providing a math expression (e.g., a gyroid formula), a name, a category, and a description. The system creates the 3MF file with proper library metadata, validates it, and saves it to the user library directory so it immediately appears in the library browser.

**Why this priority**: This is the core creative action — extending the library with new functions. Without it, the agent cannot contribute new content.

**Independent Test**: Can be fully tested by calling the creation tool, then verifying the resulting 3MF file exists in the correct category directory, contains the correct metadata, and can be loaded back with the correct function graph.

**Acceptance Scenarios**:

1. **Given** a running MCP server in headless mode, **When** the agent calls `create_library_entry` with name "schwarz-p", category "lattices", expression "cos(x*2*pi/10) + cos(y*2*pi/10) + cos(z*2*pi/10)", and description "Schwarz-P minimal surface", **Then** a valid 3MF file is created at `<user-library>/lattices/schwarz-p.3mf` with the correct metadata.
2. **Given** the agent provides an invalid expression (e.g., using unsupported `^` operator), **When** the agent calls `create_library_entry`, **Then** the response includes a clear error message explaining the syntax issue, lists supported operators/functions, and shows a corrected example.
3. **Given** the agent omits required parameters like "name", **When** it calls `create_library_entry`, **Then** the response includes a structured error listing all required parameters, their types, and a complete usage example.
4. **Given** a file with the same name already exists in that category, **When** the agent calls `create_library_entry`, **Then** the system rejects the request with an error explaining the conflict and suggesting alternatives (e.g., use a different name or use `update_library_entry`).

---

### User Story 3 — Create a Library Entry from a Node Graph (Priority: P2)

An AI agent builds a more complex function by composing nodes (e.g., combining a sphere SDF with a gyroid using intersection) via the **existing** `create_node`, `create_link`, `create_function_call_node`, and `set_parameter_value` tools, then saves the result as a library entry using the new `export_to_library` tool. This covers functions that cannot be expressed as a single math expression.

**Why this priority**: Expression-based creation covers many cases, but the most interesting library entries involve compositions and transformations that require the full node graph. The graph-building tools already exist — only the "export to library with metadata" step is new.

**Independent Test**: Can be tested by creating a document with a function graph via existing MCP tools, then calling `export_to_library`, and verifying the resulting library file round-trips correctly.

**Acceptance Scenarios**:

1. **Given** the agent has created a document with a function containing multiple nodes (e.g., sphere + gyroid intersection), **When** the agent calls `export_to_library` specifying the function ID, category, name, and description, **Then** a library entry is created containing only that function and its transitive dependencies.
2. **Given** the function references another function (function call node), **When** the agent exports it, **Then** the exported 3MF includes all transitively referenced functions.
3. **Given** the agent specifies a function ID that does not exist, **When** it calls `export_to_library`, **Then** the response lists the available function IDs in the current document and explains how to find the correct ID.

---

### User Story 4 — Import a Library Function into the Current Document (Priority: P2)

An AI agent selects a function from the library and imports it into the currently active document so it can be used as a building block for more complex designs (e.g., importing a "gyroid" lattice to combine with a custom bounding shape).

**Why this priority**: Importing existing library functions enables composition and reuse, which is essential for building complex models from curated primitives.

**Independent Test**: Can be tested by creating a document, importing a library entry, and verifying the function appears in the document's resource list.

**Acceptance Scenarios**:

1. **Given** a library entry "gyroid.3mf" in the "lattices" category, **When** the agent calls `import_library_entry` with category "lattices" and name "gyroid", **Then** the tagged functions from that entry are merged into the current document with all their dependencies, and the response includes the new resource IDs.
2. **Given** the agent tries to import from a non-existent category or entry, **Then** the response lists available categories and entries, and provides a usage example.
3. **Given** the current document already contains a function with the same name, **When** the agent imports the library entry, **Then** the system handles the name collision gracefully (either renaming or skipping with explanation).

---

### User Story 5 — Fix MCP Transport for Real Client Compatibility (Priority: P1)

An AI agent (e.g., VS Code Copilot, Claude Desktop) connects to the Gladius MCP server via stdio transport. The connection works reliably without parse errors, stdout noise, or protocol framing mismatches. The server correctly handles the standard MCP stdio protocol.

**Why this priority**: Without a working transport layer, none of the library tools can be used by real MCP clients. This is a prerequisite for all other stories.

**Independent Test**: Can be tested by launching `gladiusmcp --mcp-stdio --headless` and piping standard MCP-framed requests (Content-Length headers), verifying clean JSON-RPC responses without any non-protocol output mixed in.

**Acceptance Scenarios**:

1. **Given** a client sends a JSON-RPC request with standard MCP framing, **When** the server processes it, **Then** the response is a valid JSON-RPC response without any non-protocol bytes (no device logs, no "Registered MCP tool" lines) on stdout.
2. **Given** the server is started with `--mcp-stdio`, **When** any internal component logs a message, **Then** the log output goes to stderr only, never to stdout.
3. **Given** a client sends multiple sequential requests, **When** each request is processed, **Then** each response is correctly framed and parseable by standard MCP client libraries.

---

### User Story 6 — Agent-Friendly Error Messages with Usage Examples (Priority: P2)

When an AI agent uses any MCP tool incorrectly (wrong parameters, missing arguments, invalid values), the error response includes not just what went wrong, but also how to fix it — with a concrete usage example showing the correct invocation.

**Why this priority**: Agents recover from errors by reading the error message; instructive errors dramatically reduce retry cycles and make the tools self-documenting.

**Independent Test**: Can be tested by deliberately calling each tool with missing/invalid parameters and verifying the error response contains a usage example.

**Acceptance Scenarios**:

1. **Given** the agent calls any tool with missing required parameters, **When** the error is returned, **Then** the response includes: (a) which parameters are missing, (b) the parameter types and descriptions, and (c) a complete example invocation with realistic values.
2. **Given** the agent calls `create_library_entry` with an invalid expression, **When** the error is returned, **Then** the response includes the parse error location, supported syntax reference, and a corrected example expression.
3. **Given** the agent calls a tool with an unknown tool name, **When** the error is returned, **Then** the response includes a list of available tools grouped by category (library, document, rendering, etc.).

---

### Edge Cases

- What happens when the library directory does not exist or is not writable? → The system creates it on first write if possible, or returns a clear error explaining the path and permission issue.
- What happens when a library 3MF file is corrupted or unreadable? → The entry is listed with an error flag and a message explaining the issue; other entries remain accessible.
- What happens when the agent tries to overwrite a shipped (read-only) library entry? → The system rejects the write with an explanation that shipped entries are read-only and suggests saving to a different name or the user directory.
- What happens when the agent creates a library entry with characters invalid for filenames? → The system sanitizes the name or rejects it with the list of invalid characters.
- What happens when multiple agents or users access the library concurrently? → File-level atomicity (write-to-temp-then-rename) prevents corruption; last-write-wins semantics apply.
- What happens when a library entry has no `gladius:library-functions` metadata? → The system treats all functions in the file as importable (legacy fallback behavior).

## Requirements *(mandatory)*

### Functional Requirements

#### Transport & Protocol Reliability

- **FR-001**: The MCP server MUST NOT emit any non-protocol output (logs, debug info, tool registration messages) to stdout when operating in stdio transport mode. All diagnostic output MUST go to stderr.
- **FR-002**: The MCP server MUST support the standard MCP stdio framing protocol so that standard MCP client libraries can connect without encountering parse errors.
- **FR-003**: The stdio startup path MUST NOT create duplicate application instances or produce duplicate initialization output.

#### Library Discovery

- **FR-004**: The MCP server MUST provide a tool to list all library categories (subdirectories) and their entries, returning for each entry: file name, description, and tagged function IDs.
- **FR-005**: The MCP server MUST provide a tool to get detailed information about a specific library entry, including its function list with names, input/output parameter signatures, and description metadata.
- **FR-006**: Library listing MUST reflect the current state of the user library directory (not a cached snapshot from startup).

#### Library Authoring

- **FR-007**: The MCP server MUST provide a tool to create a new library entry from a math expression, given a name, category, expression, and description.
- **FR-008**: The created library entry MUST be a valid 3MF file with correct `gladius:library-functions` and `gladius:library-description` metadata.
- **FR-009**: The MCP server MUST provide a tool to export a function (by resource ID) from the current document into the library, including all transitive dependencies and correct metadata.
- **FR-010**: The MCP server MUST reject creation of library entries that would overwrite existing files, with an error explaining the conflict.
- **FR-011**: The MCP server MUST validate the created library entry (expression parsing, 3MF structure) before saving and return validation errors if the entry is invalid.

#### Library Import

- **FR-012**: The MCP server MUST provide a tool to import a library entry's tagged functions (and their dependencies) into the current document.
- **FR-013**: The import tool MUST return the new resource IDs of the imported functions in the current document.

#### Error Guidance

- **FR-014**: Every MCP tool MUST return a structured error response when called with missing or invalid parameters, containing: (a) which parameters are wrong, (b) parameter types and descriptions, (c) a complete usage example with realistic values.
- **FR-015**: When a tool receives an invalid expression, the error MUST include the specific syntax issue, the supported syntax reference, and a corrected example.
- **FR-016**: When a tool name is unknown, the error response MUST include the list of available tools grouped by domain (library, document, graph, rendering, utilities).

#### Library Management

- **FR-017**: The MCP server MUST provide a tool to delete a library entry by category and name, with confirmation of what was deleted.
- **FR-018**: The MCP server MUST provide a tool to update (overwrite) an existing library entry, preserving backward compatibility for documents already referencing the old version. *Note: This could be consolidated with `create_library_entry` via an `overwrite` flag rather than being a separate tool.*

### Key Entities

- **Library Category**: A named subdirectory within the user library (e.g., "primitives", "lattices", "custom"). Contains zero or more library entries.
- **Library Entry**: A 3MF file in a library category directory, enriched with `gladius:library-functions` (tagged importable function IDs) and `gladius:library-description` metadata. Represents one or more reusable SDF/implicit functions.
- **Library Function**: A volumetric function within a library entry, identified by its model resource ID. Has a name, input parameters, output type, and a node graph defining its computation.
- **MCP Tool**: A named operation exposed via the MCP JSON-RPC protocol. Has a name, description, input schema, and produces a JSON result. Each tool must be self-documenting through its error responses.

## Relationship to Existing MCP Tools

This section documents how the proposed library tools relate to the 31 existing MCP tools. The goal is to avoid redundancy while adding genuine new capability.

### Tools That Already Exist and Can Be Reused

The existing tools already provide the building blocks for authoring functions. An agent can already:

- **Build function graphs** using `create_node`, `delete_node`, `create_link`, `delete_link`, `set_parameter_value`, `create_function_call_node`
- **Create functions from expressions** using `create_function_from_expression` (supports sin, cos, pow, sqrt, etc.)
- **Inspect documents** using `get_3mf_structure`, `get_function_graph`, `get_node_info`
- **Manage documents** using `create_document`, `open_document`, `save_document`, `save_document_as`
- **Validate models** using `validate_model`
- **Clean up** using `remove_unused_resources`

These tools remain unchanged. The library tools build on top of them.

### What Cannot Be Done with Existing Tools

1. **List library contents** — No tool enumerates the library directory. The agent has no file system browsing capability.
2. **Import into current document** — `open_document` replaces the active document; there is no merge/import-from-file capability.
3. **Selective export with metadata** — `save_document_as` saves the entire document without library metadata tagging or dependency pruning.
4. **Read library entry metadata without side effects** — `open_document` could load a library 3MF for inspection, but it changes the active document (destructive to the agent's current work).
5. **Delete library entries** — No file deletion capability exists.

### Proposed Tools and Their Justification

| Proposed Tool | Why not use existing tools? |
|---|---|
| `list_library` | Entirely new capability — no existing tool can enumerate the library file system |
| `get_library_entry_info` | Although `open_document` + `get_3mf_structure` could inspect a library file, this destroys the current document state. The proposed tool reads metadata and function signatures without opening the file as the active document. |
| `create_library_entry` | Composes `create_document` + `create_function_from_expression` + `save_document_as` into a single atomic operation, adding: library metadata tagging, category directory creation, filename collision detection, and input validation. Reduces a 3-call, error-prone workflow to 1 call. |
| `export_to_library` | `save_document_as` saves the entire document. This tool selectively exports one function with its transitive dependency closure, prunes unrelated resources, and adds library metadata — a fundamentally different operation. |
| `import_library_entry` | No existing equivalent. `open_document` replaces the current document; this tool merges tagged functions from a library file into the current document, returning new resource IDs. |
| `delete_library_entry` | No file deletion capability exists in existing tools. |
| `update_library_entry` | Same justification as `create_library_entry`, but allows overwriting an existing entry. Could alternatively be folded into `create_library_entry` with an `overwrite` flag (implementation decision). |

### Composite Workflow: Existing + New Tools

A typical agent workflow combines existing and new tools:

1. `list_library` *(new)* → discover available functions
2. `create_document` *(existing)* → start a new model
3. `import_library_entry` *(new)* → pull in a gyroid lattice
4. `create_function_from_expression` *(existing)* → create a bounding sphere
5. `create_function_call_node` *(existing)* → reference both functions
6. `create_node` + `create_link` *(existing)* → intersect them
7. `export_to_library` *(new)* → save the intersection as a new library entry
8. `list_library` *(new)* → verify it appears

## Assumptions

- The user library directory follows the existing convention: `~/.local/share/gladius/library/` on Linux, with subdirectories as categories.
- Library entries use the existing `gladius:library-functions` and `gladius:library-description` metadata format for backward compatibility.
- The MCP server operates in headless mode for agent use (no UI required for library operations).
- Expression syntax follows the existing `create_function_from_expression` format (sin, cos, sqrt, pow, etc. — no GLSL).
- File naming uses the entry name with `.3mf` extension, sanitized for filesystem safety.
- The shipped library directory is read-only; all agent-created entries go to the user library directory.
- The existing selective import logic (dependency closure computation, pruning) is reused for the import tool.
- Standard MCP stdio framing means newline-delimited JSON (as used by the current MCP SDK/specification for stdio transport), not HTTP Content-Length headers.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: An agent can discover all library entries across all categories in a single tool call, receiving structured results within 2 seconds.
- **SC-002**: An agent can create a new library entry from a math expression and verify it appears in the library listing within a single session (no restart required).
- **SC-003**: An agent can import a library function into a document and use it in a composition (e.g., create a function call node referencing it) within 5 tool calls or fewer.
- **SC-004**: When any tool is called with incorrect parameters, the error response contains a working usage example that the agent can copy and adjust — verified for all library tools.
- **SC-005**: The MCP server stdio transport produces zero non-protocol bytes on stdout during a full session of library operations (create, list, import, delete).
- **SC-006**: All new library tools have corresponding automated tests achieving 100% coverage of success paths and error paths.
- **SC-007**: An agent can complete a full workflow — list library, create entry, verify it appears, import into document, compose with another function, validate — in under 15 tool calls.
