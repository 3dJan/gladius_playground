# Quickstart: MCP Snippet Tool Extensions

**Feature**: 022-mcp-snippet-extensions  
**Date**: 2026-02-26

## Overview

This feature extends the MCP snippet tools with: (1) test coverage for already-implemented signature features, (2) build-item root annotations in program snippets, (3) reserved keyword validation for argument names, and (4) deprecation annotations on graph-based tools.

## Key Insight from Research

**Most of the spec requirements are already implemented** as part of feature 021 (Graph ↔ Code View). The research phase revealed:
- `get_function_snippet` already returns `arguments` and `output_type` ✓
- `set_function_snippet` already accepts arguments and creates Begin node ports ✓
- `get_program_snippet` already includes full function signatures ✓
- `set_program_snippet` already parses signatures with arguments ✓

The **new work** is limited to:
1. **Build-item root annotations** in `get_program_snippet` response (FR-004, FR-012)
2. **Reserved keyword validation** for argument names (FR-009)
3. **Deprecation annotations** on graph-based tools (FR-010, FR-011)
4. **Comprehensive test coverage** for all the above

## Implementation Priority

### Must Do (P1)
1. Add tests for `get_function_snippet` verifying `arguments` and `output_type` in response
2. Add tests for `set_function_snippet` verifying argument creation on Begin node
3. Add tests for signature round-trip (get → modify args → set → get)
4. Add build-item root metadata to `getProgramSnippet` response
5. Add reserved keyword validation to argument name processing

### Should Do (P2)
6. Mark graph-based tools as deprecated in MCPServer.cpp
7. Add deprecation notice to graph tool responses

### Could Do (P3)
8. Improve error messages for undefined variable references
9. Document that `set_program_snippet` preserves functions not in the snippet

## Files to Modify

| File | Change | Effort |
|------|--------|--------|
| `gladius/src/mcp/tools/FunctionOperationsTool.cpp` | Add root-function metadata to `getProgramSnippet`; add keyword validation | Medium |
| `gladius/src/mcp/MCPServer.cpp` | Update tool descriptions with deprecation notices | Small |
| `gladius/tests/unittests/MCPSnippetTool_tests.cpp` | Add ~15-20 new tests | Medium |
| `gladius/src/FunctionArgument.h` (or new validation) | Add `isReservedKeyword()` utility | Small |

## Build & Test

```bash
# Build (via VS Code task)
# "Build ALL (linux-releaseWithDebug)"

# Run tests (via VS Code task)
# "Run Gladius Tests (linux-releaseWithDebug)"

# Run snippet-specific tests
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter='*Snippet*:*GraphToSnippet*'
```

## Example: Complete Agent Workflow After Feature

```bash
# 1. Agent gets the full program
get_program_snippet → returns all functions with signatures + root annotations

# 2. Agent sees:
# // Function: sphere (ID: 10)
# float sphere_10(vec3 pos) {
#   return length(pos) - 5.0;
# }
#
# // Function: shell (ID: 20) [root]
# float shell_20(vec3 pos) {
#   float v0 = sphere_10(pos);
#   return abs(v0) - 1.0;
# }
# 
# root_functions: [20]

# 3. Agent modifies sphere to accept radius parameter
set_function_snippet(
  function_id: 10,
  snippet: "return length(pos) - radius;",
  arguments: [{name: "pos", type: "vec3"}, {name: "radius", type: "float"}]
)

# 4. Agent verifies
get_function_snippet(function_id: 10) → arguments include radius, output_type = float
```
