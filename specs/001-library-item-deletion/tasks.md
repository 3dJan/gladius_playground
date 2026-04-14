# Tasks: Library Item Deletion with Bin Recovery

**Input**: Design documents from `/specs/001-library-item-deletion/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/mcp-bin-tools.json, quickstart.md

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Exact file paths included in all descriptions

## Path Conventions

- **Source**: `gladius/src/` (headers `.h`, implementations `.cpp`)
- **Tests**: `gladius/tests/apitests/` (MCP integration), `gladius/tests/unittests/` (unit)

---

## Phase 1: Setup

**Purpose**: Add foundational filesystem utilities that all user stories depend on

- [X] T001 Add `getBinDir()` helper that returns `getUserLibraryDir() / ".bin"` in `gladius/src/FileSystemUtils.h` and `gladius/src/FileSystemUtils.cpp`
- [X] T002 Add `isShippedEntry(category, name)` helper that checks if a corresponding file exists in `getShippedLibraryDir()` in `gladius/src/FileSystemUtils.h` and `gladius/src/FileSystemUtils.cpp`
- [X] T003 Add `disambiguateFilename(directory, stem, extension)` helper that returns a non-colliding path using numeric suffix (`name_1.3mf`, `name_2.3mf`) in `gladius/src/FileSystemUtils.h` and `gladius/src/FileSystemUtils.cpp`
- [X] T004 [P] Add unit tests for `getBinDir()`, `isShippedEntry()`, and `disambiguateFilename()` in `gladius/tests/unittests/FileSystemUtils_test.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Modify existing library infrastructure so it correctly distinguishes shipped entries and hides the `.bin/` folder

**CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 Update `resolveEntryPath()` in `gladius/src/mcp/tools/LibraryTool.cpp` to set `isShipped = true` when the user-dir entry has a corresponding file in the shipped dir (use `isShippedEntry()` from T002)
- [X] T006 Filter dot-prefixed directories (`.bin`, etc.) from `getAvailableCategories()` in `gladius/src/mcp/tools/LibraryTool.cpp` by skipping names starting with `.`
- [X] T007 Filter dot-prefixed directories from `getAvailableEntries()` category scan in `gladius/src/mcp/tools/LibraryTool.cpp` (same guard as T006 but for the entries lambda)
- [X] T008 Add tests for shipped-detection update (`resolveEntryPath` returns `isShipped=true` for synced entries) in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T009 Add tests verifying `getAvailableCategories()` excludes `.bin` directory in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: `resolveEntryPath()` correctly detects synced shipped entries; `.bin/` is invisible to library browsing

---

## Phase 3: User Story 1 — Delete a User Library Entry (Priority: P1) MVP

**Goal**: Soft-delete user-created library entries by moving them to `.bin/<category>/` with filename disambiguation

**Independent Test**: Create a user entry, delete it via MCP tool, verify it's gone from listing and present in `.bin/`

### Implementation

- [X] T010 [US1] Modify `deleteLibraryEntry()` in `gladius/src/mcp/tools/LibraryTool.cpp` to move the file to `getBinDir() / category /` using `std::filesystem::rename()` instead of `std::filesystem::remove()`, with auto-creation of bin category subfolder and filename disambiguation via `disambiguateFilename()`
- [X] T011 [US1] Update the `delete_library_entry` MCP tool description and success response in `gladius/src/mcp/MCPServer.cpp` to reflect soft-delete behavior (add `bin_path` field per contracts/mcp-bin-tools.json)
- [X] T012 [US1] Add tests for soft-delete: entry moved to bin, entry removed from listing, bin folder auto-created, filename disambiguated on collision, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T013 [US1] Add test for filesystem error handling: verify original file intact if move fails, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: `delete_library_entry` MCP tool performs soft-delete; all US1 acceptance scenarios pass

---

## Phase 4: User Story 2 — Shipped Library Entries Are Protected (Priority: P1)

**Goal**: Reject deletion of shipped entries (including synced copies) with a clear error message

**Independent Test**: Attempt to delete a shipped entry via MCP tool, verify rejection with `is_shipped: true`

### Implementation

- [X] T014 [US2] Verify that the updated `resolveEntryPath()` from T005 correctly blocks deletion of synced shipped entries in `deleteLibraryEntry()` — no code change needed if T005 is correct; add explicit acceptance test in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T015 [US2] Add test for synced-shipped-entry rejection: create entry with same name as shipped, attempt delete, verify `is_shipped=true` error, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: Shipped entries (including synced copies) cannot be deleted; US2 acceptance scenarios pass

---

## Phase 5: User Story 4 — Browse Bin Contents (Priority: P2)

**Goal**: List all entries in the bin, optionally filtered by category

**Independent Test**: Delete several entries, browse bin via MCP, verify all deleted entries listed with correct categories

**Note**: Implementing US4 before US3 because restore (US3) needs browse to be useful

### Implementation

- [X] T016 [US4] Implement `browseBin(category)` method in `gladius/src/mcp/tools/LibraryTool.h` and `gladius/src/mcp/tools/LibraryTool.cpp` that scans `.bin/` subfolders and returns JSON array of entries per contracts/mcp-bin-tools.json
- [X] T017 [US4] Add `browseBin` virtual method to `gladius/src/mcp/MCPApplicationInterface.h` and implement forwarding in `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp`
- [X] T018 [US4] Register `browse_bin` MCP tool in `gladius/src/mcp/MCPServer.cpp` with JSON schema from contracts/mcp-bin-tools.json
- [X] T019 [US4] Add tests for browse_bin: returns entries grouped by category, empty bin returns message, category filter works, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: `browse_bin` MCP tool lists bin contents; US4 acceptance scenarios pass

---

## Phase 6: User Story 3 — Restore a Deleted Library Entry (Priority: P2)

**Goal**: Move a bin entry back to its original library category with conflict disambiguation

**Independent Test**: Delete an entry, restore it via MCP, verify it reappears in the original category

### Implementation

- [X] T020 [US3] Implement `restoreBinEntry(category, name)` method in `gladius/src/mcp/tools/LibraryTool.h` and `gladius/src/mcp/tools/LibraryTool.cpp` that moves file from `.bin/<category>/` back to `<userLib>/<category>/` using `disambiguateFilename()` for conflicts
- [X] T021 [US3] Add `restoreBinEntry` virtual method to `gladius/src/mcp/MCPApplicationInterface.h` and implement forwarding in `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp`
- [X] T022 [US3] Register `restore_bin_entry` MCP tool in `gladius/src/mcp/MCPServer.cpp` with JSON schema from contracts/mcp-bin-tools.json
- [X] T023 [US3] Add tests for restore: entry reappears in category, conflict renamed with numeric suffix, not-found error, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: `restore_bin_entry` MCP tool works; US3 acceptance scenarios pass

---

## Phase 7: User Story 5 — Permanently Discard Bin Contents (Priority: P3)

**Goal**: Permanently delete individual bin entries or empty the entire bin

**Independent Test**: Delete entries to bin, permanently delete one, empty rest, verify files gone from disk

### Implementation

- [X] T024 [P] [US5] Implement `deleteBinEntry(category, name)` method in `gladius/src/mcp/tools/LibraryTool.h` and `gladius/src/mcp/tools/LibraryTool.cpp` using `std::filesystem::remove()`
- [X] T025 [P] [US5] Implement `emptyBin(category)` method in `gladius/src/mcp/tools/LibraryTool.h` and `gladius/src/mcp/tools/LibraryTool.cpp` that removes all files (optionally filtered by category) and removes empty subdirectories
- [X] T026 [US5] Add `deleteBinEntry` and `emptyBin` virtual methods to `gladius/src/mcp/MCPApplicationInterface.h` and implement forwarding in `gladius/src/mcp/ApplicationMCPAdapter.h` and `gladius/src/mcp/ApplicationMCPAdapter.cpp`
- [X] T027 [US5] Register `delete_bin_entry` and `empty_bin` MCP tools in `gladius/src/mcp/MCPServer.cpp` with JSON schemas from contracts/mcp-bin-tools.json
- [X] T028 [US5] Add tests for permanent delete: single entry removed, empty bin removes all, empty on already-empty bin succeeds, category filter works, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`

**Checkpoint**: `delete_bin_entry` and `empty_bin` MCP tools work; US5 acceptance scenarios pass

---

## Phase 8: UI — Library Browser Integration (Cross-Story)

**Goal**: Wire all MCP operations into the library browser UI with context menus and a bin tab

### Delete Context Menu (US1 + US2 UI)

- [X] T029 [US1] Add a delete callback (`std::function<void(std::string const& category, std::string const& name)>`) to `ThreemfFileViewer` constructor in `gladius/src/ui/ThreemfFileViewer.h` and `gladius/src/ui/ThreemfFileViewer.cpp`
- [X] T030 [US1] Implement right-click context menu in `ThreemfFileViewer::renderThumbnailItem()` in `gladius/src/ui/ThreemfFileViewer.cpp` using `ImGui::OpenPopup()` + `ImGui::BeginPopup()` with "Delete" menu item (disabled with tooltip for shipped entries)
- [X] T031 [US1] Wire delete callback in `LibraryBrowser` in `gladius/src/ui/LibraryBrowser.h` and `gladius/src/ui/LibraryBrowser.cpp` — pass a lambda that calls the soft-delete operation and shows inline notification on completion

### Bin Tab (US3 + US4 + US5 UI)

- [X] T032 [US4] Add a "Bin" tab at the end of the `LibraryBrowser` tab bar in `gladius/src/ui/LibraryBrowser.cpp` that lists bin entries by scanning `.bin/` subfolders, showing "Bin is empty" message when applicable
- [X] T033 [US3] Add right-click context menu on bin tab entries in `gladius/src/ui/LibraryBrowser.cpp` with "Restore" option that calls `restoreBinEntry()` and refreshes the library view
- [X] T034 [US5] Add right-click context menu "Delete permanently" option on bin tab entries in `gladius/src/ui/LibraryBrowser.cpp` that shows a confirmation dialog before calling `deleteBinEntry()`
- [X] T035 [US5] Add "Empty Bin" button in the bin tab header area in `gladius/src/ui/LibraryBrowser.cpp` that shows a confirmation dialog before calling `emptyBin()`

**Checkpoint**: Full UI integration complete — all user stories accessible via library browser

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, edge-case handling, documentation

- [X] T036 [P] Add Doxygen `///` comments to all new public methods in `gladius/src/FileSystemUtils.h`, `gladius/src/mcp/tools/LibraryTool.h`, `gladius/src/mcp/MCPApplicationInterface.h`
- [X] T037 [P] Add edge-case tests: bin folder manually deleted externally then re-delete works; filesystem permission error returns graceful error, in `gladius/tests/apitests/MCP_LibraryTool_tests.cpp`
- [X] T038 Run full test suite to verify no regressions
- [X] T039 Validate against quickstart.md scenarios (manual smoke test of full UI + MCP workflow)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — start immediately
- **Phase 2 (Foundational)**: Depends on Phase 1 — BLOCKS all user stories
- **Phase 3 (US1)**: Depends on Phase 2
- **Phase 4 (US2)**: Depends on Phase 2 (can run in parallel with US1)
- **Phase 5 (US4)**: Depends on Phase 2
- **Phase 6 (US3)**: Depends on Phase 5 (needs browse_bin for context)
- **Phase 7 (US5)**: Depends on Phase 2 (can run in parallel with US3/US4)
- **Phase 8 (UI)**: Depends on Phases 3-7 (all MCP backend must be complete)
- **Phase 9 (Polish)**: Depends on all previous phases

### User Story Dependencies

```
Phase 1 (Setup)
    │
Phase 2 (Foundational)
    │
    ├── Phase 3 (US1: Delete) ──────────────┐
    ├── Phase 4 (US2: Shipped Protection) ──┤
    ├── Phase 5 (US4: Browse Bin) ──┐       │
    │       │                       │       │
    │   Phase 6 (US3: Restore) ────┤       │
    │                               │       │
    ├── Phase 7 (US5: Permanent Delete) ───┤
    │                                       │
    └───────────────────────────────────────┘
                                            │
                                    Phase 8 (UI)
                                            │
                                    Phase 9 (Polish)
```

### Parallel Opportunities

**Within Phase 1**: T001, T002, T003 can be done in sequence (same file), T004 in parallel (test file)

**Within Phase 2**: T005-T007 are sequential (same file), T008-T009 in parallel after (test file)

**Across User Stories**: US1 (Phase 3) and US2 (Phase 4) can run in parallel. US4 (Phase 5) and US5 (Phase 7) can run in parallel. US3 (Phase 6) must wait for US4.

**Within Phase 8**: T029-T031 (Delete UI) and T032-T035 (Bin tab UI) can be partially parallelized if done by different developers.

---

## Parallel Example: After Foundational Phase

```
Thread A (US1 + US2):           Thread B (US4 + US5):
T010 soft-delete impl           T016 browseBin impl
T011 MCP tool update            T017 interface methods
T012-T013 tests                 T018 MCP registration
T014-T015 shipped tests         T019 browse tests
                                T024-T025 permanent delete impl
                                T026-T027 interface + MCP reg
                                T028 permanent delete tests

                    ↓ merge ↓

Thread A: US3 (T020-T023)      Thread B: UI (T029-T035)
```

---

## Implementation Strategy

### MVP First (User Story 1 + 2 Only)

1. Complete Phase 1: Setup (T001-T004)
2. Complete Phase 2: Foundational (T005-T009)
3. Complete Phase 3: US1 Delete (T010-T013)
4. Complete Phase 4: US2 Shipped Protection (T014-T015)
5. **STOP and VALIDATE**: Soft-delete works via MCP, shipped entries protected
6. This is a usable MVP — entries can be recovered by manually browsing `.bin/` on disk

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. US1 + US2 → **MVP**: Delete with bin + shipped protection
3. US4 + US3 → **Increment 2**: Browse + Restore via MCP
4. US5 → **Increment 3**: Permanent delete via MCP
5. UI → **Increment 4**: Full library browser integration
6. Polish → **Release candidate**

---

## Summary

| Metric | Value |
|--------|-------|
| **Total tasks** | 39 |
| **Phase 1 (Setup)** | 4 tasks |
| **Phase 2 (Foundational)** | 5 tasks |
| **US1 (Delete)** | 4 tasks |
| **US2 (Shipped Protection)** | 2 tasks |
| **US4 (Browse Bin)** | 4 tasks |
| **US3 (Restore)** | 4 tasks |
| **US5 (Permanent Delete)** | 5 tasks |
| **UI (Cross-Story)** | 7 tasks |
| **Polish** | 4 tasks |
| **Parallel opportunities** | T004, T008/T009, US1‖US2, US4‖US5, Delete-UI‖Bin-UI |
| **MVP scope** | Phases 1-4 (15 tasks): soft-delete + shipped protection via MCP |
