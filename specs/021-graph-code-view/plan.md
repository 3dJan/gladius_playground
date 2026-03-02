# Implementation Plan: Graph ↔ Code View

**Branch**: `021-graph-code-view` | **Date**: 2026-02-23 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/021-graph-code-view/spec.md`

## Summary

Add bidirectional conversion between node-graph and GLSL-like code representations of implicit functions in Gladius. The graph remains ground truth; a Code tab in ModelEditor lets users view/edit code and sync back. MCP tools expose snippet read/write for AI agents. The implementation extends `ExpressionToGraphConverter` to support all ~58 node types (currently ~30 supported), adds FunctionCall representation as named function calls, adds a Code tab to ModelEditor with sync button, and exposes `get_function_snippet` / `set_function_snippet` / `get_program_snippet` MCP tools.

## Technical Context

**Language/Version**: C++20 (Clang on Linux, MSVC on Windows)
**Primary Dependencies**: ImGui (UI), ax::NodeEditor (graph editor), lib3mf (file format), OpenCL 1.2+ (GPU compute)
**Storage**: 3MF files (XML-based, lib3mf API)
**Testing**: GTest/GMock, VS Code tasks for build/test
**Target Platform**: Linux (primary), Windows
**Project Type**: Single C++ application with library components
**Performance Goals**: <2s graph↔code conversion for 100-node graphs (SC-002); <3s whole-program listing for 10+ functions (SC-005)
**Constraints**: UI must remain responsive (Constitution Principle VI); files <400 lines (Principle III); no blocking main thread during conversion
**Scale/Scope**: ~58 node types to support; typical documents contain 1–20 functions with 5–100 nodes each

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|-----------|------|--------|
| I. Modern C++ | Use C++20 features, smart pointers, const correctness, east-side const, exceptions for errors | PASS |
| II. Test-First | Unit tests for all new converter node types, sync logic, name generation, MCP tools. Existing 44 idempotency tests provide baseline. | PASS |
| III. Simplicity (KISS/DRY/YAGNI) | Extend existing `ExpressionToGraphConverter` rather than creating a new converter. Reuse existing MCP tool patterns. Keep new files <400 lines. P3 stories (whole-program) are clearly scoped for later. | PASS |
| IV. Code Style | Allman braces, camelCase functions, PascalCase types, m_ prefix, 4-space indent | PASS |
| V. Documentation | Doxygen for public APIs on new/modified methods | PASS |
| VI. UI Responsiveness | Graph↔code conversion for single functions is fast (<2s for 100 nodes) — no async needed for V1. Whole-program listing may need async for large documents. | PASS — monitor for large documents |

**Gate result: ALL PASS** — no violations requiring justification.

## Project Structure

### Documentation (this feature)

```text
specs/021-graph-code-view/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (MCP tool schemas)
└── tasks.md             # Phase 2 output (not created by /speckit.plan)
```

### Source Code (repository root)

```text
gladius/src/
├── ExpressionToGraphConverter.h       # MODIFY: add FunctionCall, ComposeVector, ConstantVector, etc.
├── ExpressionToGraphConverter.cpp     # MODIFY: extend nodeToExpression/convertGraphToSnippet
├── ui/
│   ├── ModelEditor.h                  # MODIFY: add TabMode::Code, code buffer state
│   ├── ModelEditor.cpp                # MODIFY: add Code tab rendering, sync button
│   └── CodeView.h / CodeView.cpp     # NEW: code editor widget (ImGui::InputTextMultiline + sync logic)
├── mcp/
│   ├── MCPServer.cpp                  # MODIFY: register new snippet tools
│   └── tools/
│       └── FunctionOperationsTool.cpp # MODIFY: add get/set snippet tool handlers
└── nodes/
    └── Assembly.h                     # READ: iterate functions for whole-program view

gladius/tests/unittests/
├── SnippetGraphIdempotency_tests.cpp  # EXISTS: 44 idempotency tests (extend for new node types)
├── GraphToSnippet_tests.cpp           # NEW: node-type-specific conversion tests
├── SnippetToGraph_tests.cpp           # NEW: parsing tests (error cases, FunctionCall, strict mode)
├── CodeView_tests.cpp                 # NEW: sync logic, name generation, unsaved-changes guard
└── MCPSnippetTool_tests.cpp           # NEW: MCP tool integration tests
```

**Structure Decision**: Single-project layout. Feature touches 4 areas: converter engine, UI, MCP tools, and tests. New UI code isolated in `CodeView.h/.cpp` to keep ModelEditor manageable. Test files split by concern.

## Complexity Tracking

No violations to justify — all constitution gates pass.
