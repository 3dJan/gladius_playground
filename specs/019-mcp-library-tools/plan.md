# Implementation Plan: MCP Library Tools — Agent-Driven SDF Library Authoring

**Branch**: `019-mcp-library-tools` | **Date**: 2026-02-15 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/019-mcp-library-tools/spec.md`

## Summary

Add library-specific MCP tools so AI agents can browse, create, import, export, and manage Gladius library entries (3MF files containing reusable SDF/implicit functions). Fix MCP stdio transport for real client compatibility. Add agent-friendly error messages with usage examples to all tools.

The feature adds ~6 new MCP tools (`list_library`, `get_library_entry_info`, `create_library_entry`, `export_to_library`, `import_library_entry`, `delete_library_entry`) built on the existing library metadata system and MCP tool infrastructure. Transport fixes address stdout pollution, duplicate Application construction, and framing compatibility in the stdio path.

## Technical Context

**Language/Version**: C++20 (`CMAKE_CXX_STANDARD 20`)  
**Primary Dependencies**: lib3mf (3MF I/O), nlohmann-json (JSON), muparser (expression parsing), cpp-httplib (HTTP transport), libcoro (coroutines), platform-folders (OS directory paths)  
**Storage**: File system — 3MF files in user library directory (`~/.local/share/gladius/library/`)  
**Testing**: GTest/GMock, via `ctest --preset ApiTests` or direct `gladius_test` execution  
**Target Platform**: Linux (primary), Windows (MSVC)  
**Project Type**: Single C++ project with `gladiusmcp` executable target  
**Performance Goals**: Library listing within 2 seconds, single tool calls under 1 second for non-I/O operations  
**Constraints**: No UI dependency (headless mode); must not break existing 31 MCP tools; stdio transport must produce zero non-protocol bytes on stdout  
**Scale/Scope**: ~6 new tools, ~1,200 lines new code (tool implementations + tests), ~100 lines modifications to existing code (transport fix, interface extension)

### Existing Code Inventory

| Component | Files | Lines | Relevance |
|---|---|---|---|
| MCP Server core | `MCPServer.{h,cpp}` | 1,836 | Tool registration, stdio loop, HTTP routes |
| MCP Application Interface | `MCPApplicationInterface.h` | 439 | Virtual interface — needs ~8 new methods |
| Application MCP Adapter | `ApplicationMCPAdapter.{h,cpp}` | 1,343 | Adapter impl — needs matching implementations |
| Tool base classes | `MCPToolBase.{h,cpp}`, `AsyncMCPToolBase.{h,cpp}` | 174 | Base class for parameter validation |
| Existing tool impls | `tools/*.{h,cpp}` | ~4,300 | Pattern to follow for new tools |
| Library metadata | `io/3mf/LibraryMetadata.{h,cpp}` | 400 | Reuse: metadata read/write, selective import closure |
| Library UI | `LibraryBrowser.{h,cpp}`, `LibraryExportDialog.{h,cpp}` | 737 | Reference: directory scanning, export workflow |
| MCP tests | `MCP_tests.cpp` + adapter tests | ~2,200 | Mock infrastructure, test patterns |
| Entry point | `main.cpp` | ~200 | Stdio path bug fix |

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| **I. Modern C++** | PASS | All new code will use C++20, smart pointers, east-side const, exceptions |
| **II. Test-First** | PASS | GTest/GMock tests for all new tools; MockMCPApplication already exists |
| **III. Simplicity (KISS/DRY/YAGNI)** | PASS | Reuse existing LibraryMetadata + selective import; new tool files <400 lines each |
| **IV. Consistent Style** | PASS | Follow existing MCP tool patterns (Allman braces, camelCase, PascalCase types) |
| **V. Documentation** | PASS | Doxygen on new public methods; tool descriptions serve as agent-facing docs |
| **VI. UI Responsiveness** | N/A | Library tools operate in headless mode; no UI thread interaction |
| **File size <400 lines** | PASS | Each new tool class in its own file; LibraryTool.cpp estimated ~350 lines |
| **No global/static vars** | PASS | No new globals; tool state via MCPToolBase members |

No constitution violations. Gate passes.

## Constitution Check — Post-Design

*Re-evaluated after Phase 1 design completion.*

| Principle | Status | Post-Design Notes |
|---|---|---|
| **I. Modern C++** | PASS | C++20 features: `std::optional`, `std::filesystem`, east-side const. Smart pointers for temp lib3mf models. |
| **II. Test-First** | PASS | 16 test cases defined covering success and error paths. MockMCPApplication extended. |
| **III. Simplicity (KISS/DRY/YAGNI)** | PASS | Reuses existing `io::readLibraryMetadata`, `io::writeLibraryMetadata`, `io::computeSelectiveImportClosure`. Export follows LibraryExportDialog pattern. `update_library_entry` consolidated with `overwrite` flag. |
| **IV. Consistent Style** | PASS | Tool registration follows existing `setupBuiltinTools()` lambda pattern. |
| **V. Documentation** | PASS | Tool descriptions serve as agent-facing docs. Doxygen on new public methods. |
| **VI. UI Responsiveness** | N/A | Headless mode only. No UI thread involved. |
| **File size <400 lines** | PASS | New files ~350 lines each. ApplicationMCPAdapter.cpp will grow to ~1,500 lines — monitor, but acceptable given it's an adapter with thin methods. |

No violations. Gate passes post-design.

## Project Structure

### Documentation (this feature)

```text
specs/019-mcp-library-tools/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0: unknowns resolution
├── data-model.md        # Phase 1: entity model
├── quickstart.md        # Phase 1: implementation guide
├── contracts/           # Phase 1: tool schemas
│   └── mcp-tools.json   # JSON schemas for new MCP tools
└── tasks.md             # Phase 2 output (via /speckit.tasks)
```

### Source Code (modifications to existing structure)

```text
gladius/src/
├── main.cpp                              # FIX: remove duplicate Application construction
├── mcp/
│   ├── MCPServer.cpp                     # MOD: register new library tools, fix stdio stdout
│   ├── MCPApplicationInterface.h         # MOD: add ~8 library virtual methods
│   ├── ApplicationMCPAdapter.h           # MOD: declare library method overrides
│   ├── ApplicationMCPAdapter.cpp         # MOD: implement library methods using LibraryMetadata
│   └── tools/
│       ├── LibraryTool.h                 # NEW: library tool class declaration
│       ├── LibraryTool.cpp               # NEW: list, info, create, import, export, delete
│       └── MCPToolBase.h                 # MOD: add usage example helper for error responses
└── io/3mf/
    └── LibraryMetadata.h                 # MOD: possibly add new convenience methods

gladius/tests/
└── apitests/
    ├── MCP_tests.cpp                     # MOD: add mock methods, new library tool tests
    └── MCP_LibraryTool_tests.cpp         # NEW: dedicated library tool test file
```

**Structure Decision**: Follows existing MCP tool pattern — one tool class per domain area. Library operations consolidated into a single `LibraryTool` class (like `FunctionOperationsTool` or `ResourceManagementTool`) to keep related functionality together while staying under the 400-line file limit.

## Complexity Tracking

No constitution violations detected. No complexity justification needed.
