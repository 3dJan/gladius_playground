# Implementation Plan: MCP Snippet Tool Extensions

**Branch**: `022-mcp-snippet-extensions` | **Date**: 2026-02-26 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/022-mcp-snippet-extensions/spec.md`

## Summary

Extend the existing MCP snippet tools to fully expose function signatures (arguments + output types) in both read and write directions, add assembly-level metadata (build-item root annotations), and mark graph-based MCP tools as deprecated. This feature builds entirely on top of the 021 (Graph ↔ Code View) implementation, requiring mostly incremental additions to existing code paths rather than new subsystems.

## Technical Context

**Language/Version**: C++20 (Clang on Linux)  
**Primary Dependencies**: lib3mf (3MF file handling), ImGui (UI), nlohmann::json (MCP protocol), GTest/GMock (testing)  
**Storage**: In-memory graph model (`nodes::Assembly`, `nodes::Model`) persisted as 3MF files  
**Testing**: GTest/GMock via "Run Gladius Tests" VS Code task  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single monorepo (gladius/)  
**Performance Goals**: <2s for single function snippet operations, <3s for whole-program (100+ node) operations  
**Constraints**: Must maintain backward compatibility with all existing MCP tool calls  
**Scale/Scope**: ~5 files modified, ~200–400 lines of new/changed code, ~30–50 new tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | PASS | All changes use C++20 idioms, STL containers, const correctness, east-side const |
| II. Test-First Development | PASS | Each increment has corresponding GTest tests; naming follows convention |
| III. Simplicity First (KISS/DRY/YAGNI) | PASS | Extends existing code paths (no new subsystems); deprecation is annotation-only |
| IV. Consistent Code Style | PASS | Follows established patterns in FunctionOperationsTool.cpp and MCPServer.cpp |
| V. Documentation and Comments | PASS | Public API changes documented with Doxygen; MCP tool descriptions updated |
| VI. UI Responsiveness | N/A | No UI changes in this feature; MCP tools are synchronous backend calls |

**Gate result**: PASS — no violations.

## Project Structure

### Documentation (this feature)

```text
specs/022-mcp-snippet-extensions/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (via /speckit.tasks)
```

### Source Code (affected files)

```text
gladius/src/
├── ExpressionToGraphConverter.h/.cpp   # Program snippet format: add signature + build-item annotations
├── mcp/
│   ├── MCPServer.cpp                   # Tool descriptions: add deprecation notices to graph tools
│   ├── tools/
│   │   └── FunctionOperationsTool.cpp  # get/set snippet: extend response/validation
│   ├── ApplicationMCPAdapter.h/.cpp    # Delegation layer: pass-through (minimal changes)
│   └── MCPApplicationInterface.h       # Interface: no changes needed (already sufficient)
└── nodes/
    └── Assembly.h/.cpp                 # Build-item query: expose which functions are roots

gladius/tests/unittests/
├── MCPSnippetTool_tests.cpp            # Extend: signature round-trip, argument validation, build-item metadata
└── GraphToSnippet_tests.cpp            # Extend: program format with signatures/annotations
```

**Structure Decision**: All changes fit within existing file structure. No new source files needed.

## Complexity Tracking

No constitution violations — this section is empty.
