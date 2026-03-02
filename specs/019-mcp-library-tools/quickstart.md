# Quickstart: MCP Library Tools Implementation

## Overview

This feature adds 6 new MCP tools for library management, fixes stdio transport issues, and adds agent-friendly error messages. Implementation touches 4 existing files and adds 3 new files.

## Prerequisites

- Branch `019-mcp-library-tools` checked out
- Build succeeds with "Build ALL (linux-releaseWithDebug)" task
- All existing MCP tests pass: `ctest --preset ApiTests`

## Implementation Order

### Step 1: Fix Stdio Transport (FR-001, FR-002, FR-003)

**Goal**: Make stdio transport work for real MCP clients before adding new tools.

**Files to modify**:

1. **`gladius/src/main.cpp` (~line 185)**: Remove the duplicate `gladius::Application app(headless)` line inside the stdio branch. This creates a second Application that shadows the outer one.

2. **`gladius/src/mcp/MCPServer.cpp`**:
   - `registerTool()` (~line 142): Change `std::cout` to `std::cerr` for "Registered MCP tool" message, or remove the guard and always use stderr.
   - `stop()` (~line 240): Guard `"MCP Server stopped"` with `if (m_transportType != TransportType::STDIO)` or use stderr.

**Verify**: Launch `gladiusmcp --mcp-stdio --headless`, send a JSON-RPC request on stdin, verify no non-protocol output on stdout.

### Step 2: Extend MCPApplicationInterface (FR-004–FR-013, FR-017)

**Files to modify**:

1. **`gladius/src/mcp/MCPApplicationInterface.h`**: Add virtual methods:
   ```cpp
   virtual nlohmann::json listLibrary(std::string const& category = "") const = 0;
   virtual nlohmann::json getLibraryEntryInfo(std::string const& category, std::string const& name) const = 0;
   virtual nlohmann::json createLibraryEntry(std::string const& name, std::string const& category,
       std::string const& expression, std::string const& description,
       std::vector<FunctionArgument> const& arguments = {}, bool overwrite = false) = 0;
   virtual nlohmann::json exportToLibrary(uint32_t functionId, std::string const& category,
       std::string const& name, std::string const& description, bool overwrite = false) = 0;
   virtual nlohmann::json importLibraryEntry(std::string const& category, std::string const& name) = 0;
   virtual nlohmann::json deleteLibraryEntry(std::string const& category, std::string const& name) = 0;
   ```

2. **`gladius/src/mcp/ApplicationMCPAdapter.h`**: Declare override methods.

3. **`gladius/src/mcp/ApplicationMCPAdapter.cpp`**: Implement using existing `gladius::io` functions:
   - `listLibrary`: Scan user library dir with `std::filesystem::directory_iterator`, open each 3MF with lib3mf, call `io::readLibraryMetadata()`.
   - `getLibraryEntryInfo`: Open specific 3MF, read metadata, iterate resources for function signatures.
   - `createLibraryEntry`: Create temp document, call `createFunctionFromExpression`, write metadata, save to library path.
   - `exportToLibrary`: Follow LibraryExportDialog pattern — stamp metadata on active doc, save to library, remove metadata.
   - `importLibraryEntry`: Open library 3MF into temp model, compute closure, prune, merge into active doc.
   - `deleteLibraryEntry`: Check path is in user library, `std::filesystem::remove()`.

### Step 3: Register New Tools in MCPServer (FR-004–FR-013, FR-017)

**File to modify**: `gladius/src/mcp/MCPServer.cpp` — `setupBuiltinTools()`

Add 6 `registerTool()` calls following the existing pattern. Use schemas from `contracts/mcp-tools.json`.

### Step 4: Add Agent-Friendly Error Messages (FR-014, FR-015, FR-016)

**Approach**: Add a helper function (free function or in MCPToolBase) that generates structured error responses with usage examples. Use it in all new library tool lambdas and optionally retrofit to existing tools.

```cpp
nlohmann::json createToolError(std::string const& error,
    nlohmann::json const& usageExample = {},
    nlohmann::json const& additionalInfo = {});
```

### Step 5: Write Tests

**New file**: `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Mock updates**: Add new library methods to `MockMCPApplication` in `MCP_tests.cpp`.

**Test cases** (following `UnitOfWork_StateUnderTest_ExpectedBehavior` naming):
- `ListLibrary_EmptyDirectory_ReturnsEmptyCategories`
- `ListLibrary_WithEntries_ReturnsCategoriesAndMetadata`
- `ListLibrary_InvalidCategory_ReturnsErrorWithAvailableCategories`
- `GetLibraryEntryInfo_ValidEntry_ReturnsFunctionSignatures`
- `GetLibraryEntryInfo_NonexistentEntry_ReturnsErrorWithAvailableEntries`
- `CreateLibraryEntry_ValidExpression_CreatesFileWithMetadata`
- `CreateLibraryEntry_InvalidExpression_ReturnsErrorWithSyntaxHelp`
- `CreateLibraryEntry_MissingParams_ReturnsErrorWithUsageExample`
- `CreateLibraryEntry_ExistingFile_ReturnsConflictError`
- `CreateLibraryEntry_OverwriteTrue_ReplacesExistingFile`
- `ExportToLibrary_ValidFunction_ExportsWithMetadata`
- `ExportToLibrary_InvalidFunctionId_ReturnsErrorWithAvailableIds`
- `ImportLibraryEntry_ValidEntry_MergesFunctionsIntoDocument`
- `ImportLibraryEntry_NoActiveDocument_ReturnsError`
- `DeleteLibraryEntry_UserEntry_DeletesFile`
- `DeleteLibraryEntry_ShippedEntry_ReturnsReadOnlyError`

## Key References

| What | Where |
|---|---|
| Existing MCP tool registration | `MCPServer.cpp:setupBuiltinTools()` |
| Library metadata API | `io/3mf/LibraryMetadata.{h,cpp}` |
| Export workflow reference | `ui/LibraryExportDialog.cpp:performExport()` |
| Mock infrastructure | `tests/apitests/MCP_tests.cpp:MockMCPApplication` |
| Tool schemas | `specs/019-mcp-library-tools/contracts/mcp-tools.json` |
| Spec (acceptance criteria) | `specs/019-mcp-library-tools/spec.md` |

## Build & Test

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run MCP tests only
cd gladius && ctest --preset ApiTests --output-on-failure

# Run full unit tests
cd gladius && ctest --preset UnitTests --output-on-failure

# Manual stdio smoke test
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | ./gladiusmcp --mcp-stdio --headless 2>/dev/null
```
