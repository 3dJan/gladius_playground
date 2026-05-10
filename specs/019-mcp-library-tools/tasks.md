# Tasks: MCP Library Tools — Agent-Driven SDF Library Authoring

**Input**: Design documents from `/specs/019-mcp-library-tools/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/mcp-tools.json, quickstart.md

**Tests**: Included — spec requires test coverage (SC-006, Constitution Principle II: Test-First Development).

**Organization**: Tasks grouped by user story to enable independent implementation and testing. User stories reordered for implementation: transport fix (US5) first as prerequisite, then library tools by priority.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story (`US1`–`US6`) this task belongs to
- Exact file paths included in every task

---

## Phase 1: Setup

**Purpose**: Verify baseline and create scaffolding for new files

- [X] T001 Verify all existing MCP tests pass by running `ctest --preset ApiTests --output-on-failure`
- [X] T002 [P] Create `gladius/src/mcp/tools/LibraryTool.h` with class declaration, include guards, and forward declarations
- [X] T003 [P] Create `gladius/src/mcp/tools/LibraryTool.cpp` with empty method stubs
- [X] T004 [P] Create `gladius/tests/apitests/MCP_LibraryTool_tests.cpp` with test fixture and includes

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Extend interfaces and fix transport — MUST complete before any user story implementation

**CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 [US5] Remove duplicate `gladius::Application app(headless)` construction in `gladius/src/main.cpp` (~line 185)
- [X] T006 [US5] Redirect `registerTool()` stdout log to stderr in `gladius/src/mcp/MCPServer.cpp` (~line 142)
- [X] T007 [US5] Guard `stop()` stdout message for stdio mode in `gladius/src/mcp/MCPServer.cpp` (~line 240)
- [X] T008 Add 6 library virtual methods to `gladius/src/mcp/MCPApplicationInterface.h` (`listLibrary`, `getLibraryEntryInfo`, `createLibraryEntry`, `exportToLibrary`, `importLibraryEntry`, `deleteLibraryEntry`)
- [X] T009 Add 6 library method override declarations to `gladius/src/mcp/ApplicationMCPAdapter.h`
- [X] T010 Add 6 library MOCK_METHOD entries to MockMCPApplication in `gladius/tests/apitests/MCP_tests.cpp`
- [X] T011 [P] Add `createToolError()` helper function for structured error responses with usage examples in `gladius/src/mcp/tools/MCPToolBase.h` and `MCPToolBase.cpp`
- [X] T012 Verify build succeeds after interface changes (all new methods can be pure virtual with adapter stubs returning empty JSON)

**Checkpoint**: Foundation ready — interfaces extended, transport fixed, mock updated, build green

---

## Phase 3: User Story 5 — Fix MCP Transport (Priority: P1) 🎯 MVP

**Goal**: Stdio transport produces zero non-protocol bytes on stdout. Real MCP clients can connect.

**Independent Test**: Launch `gladiusmcp --mcp-stdio --headless`, pipe JSON-RPC, verify no pollution.

### Tests for User Story 5

- [X] T013 [P] [US5] Add test `StdioTransport_ToolRegistration_NoStdoutPollution` verifying no "Registered MCP tool" on stdout in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T014 [P] [US5] Add test `StdioTransport_ServerStop_NoStdoutPollution` verifying no "MCP Server stopped" on stdout in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 5

- [X] T015 [US5] Verify stdio smoke test passes: `echo '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | ./gladiusmcp --mcp-stdio --headless 2>/dev/null` produces only JSON on stdout

**Checkpoint**: Stdio transport is clean — real MCP clients can connect without parse errors

---

## Phase 4: User Story 1 — Browse and Discover Library Contents (Priority: P1)

**Goal**: Agent can list all library categories/entries and inspect a specific entry's functions.

**Independent Test**: Call `list_library` and `get_library_entry_info` against a library directory with test 3MF files, verify response structure.

### Tests for User Story 1

- [X] T016 [P] [US1] Add test `ListLibrary_EmptyDirectory_ReturnsEmptyCategories` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T017 [P] [US1] Add test `ListLibrary_WithEntries_ReturnsCategoriesAndMetadata` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T018 [P] [US1] Add test `ListLibrary_InvalidCategory_ReturnsErrorWithAvailableCategories` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T019 [P] [US1] Add test `GetLibraryEntryInfo_ValidEntry_ReturnsFunctionSignatures` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T020 [P] [US1] Add test `GetLibraryEntryInfo_NonexistentEntry_ReturnsErrorWithAvailableEntries` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 1

- [X] T021 [US1] Implement `ApplicationMCPAdapter::listLibrary()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — scan library dirs, read metadata per entry, return structured JSON
- [X] T022 [US1] Implement `ApplicationMCPAdapter::getLibraryEntryInfo()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — open 3MF, read metadata + function signatures, return without changing active document
- [X] T023 [US1] Register `list_library` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`
- [X] T024 [US1] Register `get_library_entry_info` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`
- [X] T025 [US1] Verify all US1 tests pass

**Checkpoint**: Agent can discover library contents — `list_library` and `get_library_entry_info` work end-to-end

---

## Phase 5: User Story 2 — Create Library Entry from Expression (Priority: P1)

**Goal**: Agent can create a new library entry from a math expression with proper metadata.

**Independent Test**: Call `create_library_entry`, verify 3MF file created with correct metadata, verify it appears in `list_library`.

### Tests for User Story 2

- [X] T026 [P] [US2] Add test `CreateLibraryEntry_ValidExpression_CreatesFileWithMetadata` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T027 [P] [US2] Add test `CreateLibraryEntry_InvalidExpression_ReturnsErrorWithSyntaxHelp` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T028 [P] [US2] Add test `CreateLibraryEntry_MissingParams_ReturnsErrorWithUsageExample` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T029 [P] [US2] Add test `CreateLibraryEntry_ExistingFile_ReturnsConflictError` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T030 [P] [US2] Add test `CreateLibraryEntry_OverwriteTrue_ReplacesExistingFile` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 2

- [X] T031 [US2] Implement `ApplicationMCPAdapter::createLibraryEntry()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — create temp doc, create function from expression, write metadata, save to library path
- [X] T032 [US2] Register `create_library_entry` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`
- [X] T033 [US2] Verify all US2 tests pass and `list_library` shows newly created entry

**Checkpoint**: Agent can create new library entries from expressions — full create + verify workflow works

---

## Phase 6: User Story 3 — Export Function to Library (Priority: P2)

**Goal**: Agent can export a function from the current document to the library with metadata, preserving the current document.

**Independent Test**: Create document with function graph, call `export_to_library`, verify library file created, verify current document unchanged.

### Tests for User Story 3

- [X] T034 [P] [US3] Add test `ExportToLibrary_ValidFunction_ExportsWithMetadata` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T035 [P] [US3] Add test `ExportToLibrary_InvalidFunctionId_ReturnsErrorWithAvailableIds` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T036 [P] [US3] Add test `ExportToLibrary_NoActiveDocument_ReturnsError` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 3

- [X] T037 [US3] Implement `ApplicationMCPAdapter::exportToLibrary()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — follow LibraryExportDialog pattern: stamp metadata, save full doc, remove metadata
- [X] T038 [US3] Register `export_to_library` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`
- [X] T039 [US3] Verify all US3 tests pass

**Checkpoint**: Agent can export composed functions to the library

---

## Phase 7: User Story 4 — Import Library Function into Document (Priority: P2)

**Goal**: Agent can import a library entry's tagged functions into the active document and get new resource IDs.

**Independent Test**: Create document, import library entry, verify function appears in `get_3mf_structure` with new ID.

### Tests for User Story 4

- [X] T040 [P] [US4] Add test `ImportLibraryEntry_ValidEntry_MergesFunctionsIntoDocument` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T041 [P] [US4] Add test `ImportLibraryEntry_NoActiveDocument_ReturnsError` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T042 [P] [US4] Add test `ImportLibraryEntry_NonexistentEntry_ReturnsErrorWithAvailableEntries` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 4

- [X] T043 [US4] Implement `ApplicationMCPAdapter::importLibraryEntry()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — open temp model, compute closure, prune, merge into active doc, return new IDs
- [X] T044 [US4] Register `import_library_entry` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`
- [X] T045 [US4] Verify all US4 tests pass

**Checkpoint**: Agent can import library functions and compose them with existing functions

---

## Phase 8: User Story 6 — Agent-Friendly Error Messages (Priority: P2)

**Goal**: All library tools return structured error responses with usage examples on misuse.

**Independent Test**: Call each library tool with missing/invalid params, verify error includes usage example.

### Tests for User Story 6

- [X] T046 [P] [US6] Add test `ExportToLibrary_MissingParams_ReturnsUsageExample` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T047 [P] [US6] Add test `ImportLibraryEntry_MissingParams_ReturnsUsageExample` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T048 [P] [US6] Add test `DeleteLibraryEntry_MissingParams_ReturnsUsageExample` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

### Implementation for User Story 6

- [X] T049 [US6] Add usage examples to all 6 library tool error handlers using `createToolError()` helper in `gladius/src/mcp/MCPServer.cpp`
- [X] T050 [US6] Enhance unknown-tool error in `gladius/src/mcp/MCPServer.cpp` `handleCallTool()` to return tools grouped by domain (library, document, graph, rendering, utilities)
- [X] T051 [US6] Verify all US6 tests pass

**Checkpoint**: All tools return self-documenting error messages with concrete usage examples

---

## Phase 9: Library Management (delete) & Polish

**Purpose**: Delete tool + cross-cutting improvements

### Delete Tool

- [X] T052 [P] Add test `DeleteLibraryEntry_UserEntry_DeletesFile` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T053 [P] Add test `DeleteLibraryEntry_ShippedEntry_ReturnsReadOnlyError` in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T054 Implement `ApplicationMCPAdapter::deleteLibraryEntry()` in `gladius/src/mcp/ApplicationMCPAdapter.cpp` — verify user library path, `std::filesystem::remove()`, return confirmation
- [X] T055 Register `delete_library_entry` tool in `gladius/src/mcp/MCPServer.cpp` `setupBuiltinTools()` with schema from `contracts/mcp-tools.json`

### Polish

- [X] T056 Add Doxygen comments to all new public methods in `gladius/src/mcp/MCPApplicationInterface.h`
- [X] T057 Run full test suite: `ctest --preset ApiTests --output-on-failure` — verify zero regressions on existing 31 tools
- [X] T058 Run stdio smoke test: end-to-end workflow (list → create → list → import → validate) via `gladiusmcp --mcp-stdio --headless`
- [X] T059 Agent live-tests the MCP server by connecting via the `mcp_gladius_*` tools and exercising the full library workflow: call `list_library` to discover categories, call `create_library_entry` with an expression, call `get_library_entry_info` on the new entry, call `import_library_entry` into a document, call `delete_library_entry` to clean up — verify each response is well-formed and errors are actionable (NOTE: requires MCP server restart to pick up new tools)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 — BLOCKS all user stories
- **US5/Transport (Phase 3)**: Depends on Phase 2 (T005–T007 are in Phase 2, verification in Phase 3)
- **US1/Discovery (Phase 4)**: Depends on Phase 2
- **US2/Create (Phase 5)**: Depends on Phase 2 (independent of US1 but benefits from it for verification)
- **US3/Export (Phase 6)**: Depends on Phase 2
- **US4/Import (Phase 7)**: Depends on Phase 2 (benefits from US2 for test data)
- **US6/Errors (Phase 8)**: Depends on all library tool implementations (Phase 4–7)
- **Polish (Phase 9)**: Depends on all desired user stories

### User Story Dependencies

- **US5 (Transport)**: Independent — prerequisite for real usage but not for other tool implementations
- **US1 (Discovery)**: Independent — used to verify US2,US3 results but not a code dependency
- **US2 (Create)**: Independent — but creates test data useful for US4
- **US3 (Export)**: Independent — requires active document (existing MCP tools)
- **US4 (Import)**: Independent — benefits from library entries created by US2/US3
- **US6 (Errors)**: Depends on US1–US5 tool implementations being complete

### Parallel Opportunities

Within each user story phase, all tests marked [P] can run in parallel. User stories can be developed in parallel by different developers after Phase 2 completes, except US6 which depends on all other stories.

---

## Parallel Example: User Story 1

```text
# All tests can be written in parallel (different test names, same file):
T016: ListLibrary_EmptyDirectory_ReturnsEmptyCategories
T017: ListLibrary_WithEntries_ReturnsCategoriesAndMetadata
T018: ListLibrary_InvalidCategory_ReturnsErrorWithAvailableCategories
T019: GetLibraryEntryInfo_ValidEntry_ReturnsFunctionSignatures
T020: GetLibraryEntryInfo_NonexistentEntry_ReturnsErrorWithAvailableEntries

# Implementation is sequential (T021 before T023, T022 before T024):
T021: Implement listLibrary adapter method
T022: Implement getLibraryEntryInfo adapter method
T023: Register list_library tool
T024: Register get_library_entry_info tool
```

---

## Implementation Strategy

### MVP First (US5 + US1 + US2)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL — blocks everything)
3. Complete Phase 3: US5 Transport fix (enables real client testing)
4. Complete Phase 4: US1 Library discovery
5. Complete Phase 5: US2 Create library entries
6. **STOP and VALIDATE**: Agent can discover, create, and verify library entries
7. Deploy/demo if ready

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add US5 → stdio clean → real clients work
3. Add US1 → list + inspect → discovery works (MVP read-only)
4. Add US2 → create entries → library can grow (MVP read-write)
5. Add US3 → export graphs → advanced authoring
6. Add US4 → import entries → composition workflows
7. Add US6 → error guidance → agent self-recovery
8. Add delete + polish → complete feature

### Suggested MVP Scope

**US5 + US1 + US2** (Phases 1–5, tasks T001–T033): Transport fix + discovery + expression-based creation. This is the minimum set for an agent to meaningfully extend the library.

---

## Notes

- All paths are relative to repository root (`/home/jan/projects/gladius/`)
- Build via VS Code task "Build ALL (linux-releaseWithDebug)" — never run cmake/ninja manually
- Test via `ctest --preset ApiTests` for MCP tests or `ctest --preset UnitTests` for fast unit tests
- The `overwrite` flag on `create_library_entry` consolidates FR-018 (update) with FR-007 (create)
- Export follows LibraryExportDialog pattern: full doc with metadata, selective import at import time
- Commit after each task or logical group; verify build after each phase
