# Tasks: Library Metadata for Selective Function Import

**Input**: Design documents from `/specs/018-library-metadata/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Included — spec requires test-first development (Constitution Principle II).

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Exact file paths included in descriptions

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the LibraryMetadata utilities shared by all user stories

- [x] T001 Create LibraryMetadata struct and parseResourceIds() in gladius/src/io/3mf/LibraryMetadata.h — define LibraryMetadata struct (libraryFunctions as string, libraryDescription as string), declare readLibraryMetadata(Lib3MF::PModel) returning std::optional\<LibraryMetadata\>, writeLibraryMetadata(Lib3MF::PModel, LibraryMetadata const&), parseResourceIds(std::string const&) returning std::vector\<Lib3MF_uint32\>, serializeResourceIds(std::vector\<Lib3MF_uint32\> const&) returning std::string
- [x] T002 Implement LibraryMetadata functions in gladius/src/io/3mf/LibraryMetadata.cpp — parseResourceIds splits on semicolons and trims whitespace; readLibraryMetadata wraps GetMetaDataByKey in try/catch (returns std::nullopt when key missing); writeLibraryMetadata calls AddMetaData with namespace "gladius", type "xs:string", mustPreserve=true; serializeResourceIds joins IDs with semicolons

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Unit tests for shared infrastructure — must pass before user story work begins

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T003 [P] Create metadata parsing unit tests in gladius/tests/unittests/LibraryMetadata_test.cpp — tests: ParseResourceIds_WithSingleId_ReturnsOneElement, ParseResourceIds_WithMultipleIds_ReturnsAll, ParseResourceIds_WithWhitespace_TrimsCorrectly, ParseResourceIds_WithEmptyString_ReturnsEmpty, SerializeResourceIds_WithMultipleIds_RoundTrips
- [x] T004 [P] Create metadata read/write unit tests in gladius/tests/unittests/LibraryMetadata_test.cpp — tests: ReadMetadata_WithBothKeys_ReturnsPopulated, ReadMetadata_WithMissingKeys_ReturnsNullopt, ReadMetadata_WithOnlyDescription_ReturnsNullopt (library-functions is required), WriteMetadata_RoundTrip_PreservesValues (create model → write → read → compare)

**Checkpoint**: LibraryMetadata utilities tested and ready — user story implementation can begin

---

## Phase 3: User Story 1 — Selective Import from Library File (Priority: P1) 🎯 MVP

**Goal**: When a library 3MF file has `gladius:library-functions` metadata, merge() imports only the tagged functions and their transitive dependencies. Without metadata, full merge is preserved (backward compatibility).

**Independent Test**: Create a 3MF file with metadata tagging one function plus an unrelated build item. Double-click imports only the tagged function; the example mesh and build item are not imported.

### Tests for User Story 1

> **Write these tests FIRST — ensure they FAIL before implementation**

- [x] T005 [P] [US1] Create selective import test file in gladius/tests/unittests/SelectiveImport_test.cpp — test fixture that creates a minimal 3MF model with two functions (A depends on B via FunctionCall) plus an unrelated mesh/build-item, writes gladius:library-functions metadata tagging only function A
- [x] T006 [US1] Add selective import test cases in gladius/tests/unittests/SelectiveImport_test.cpp — tests: MergeSelective_WithMetadata_ImportsOnlyTaggedFunction (tagged function present, unrelated mesh absent), MergeSelective_WithMetadata_ImportsDependencies (dependency function B also imported), MergeSelective_WithoutMetadata_FallsToFullMerge (no metadata → everything imported), MergeSelective_WithInvalidFunctionId_FallsToFullMerge (nonexistent ID → warning + full merge), MergeSelective_WithMultipleFunctions_ImportsAll (two tagged functions both imported)

### Implementation for User Story 1

- [x] T007 [US1] Read library metadata in Importer3mf::merge() in gladius/src/io/3mf/Importer3mf.cpp — after opening source model but before MergeFromModel, call readLibraryMetadata(sourceModel); if metadata present, parse resource IDs and store for filtering; include LibraryMetadata.h in gladius/src/io/3mf/Importer3mf.h
- [x] T008 [US1] Build dependency closure on source model in gladius/src/io/3mf/Importer3mf.cpp — when metadata present, construct ResourceDependencyGraph on source model; for each tagged function model resource ID, call getAllRequiredResources() to compute transitive closure; collect all required resource IDs into an std::unordered_set
- [x] T009 [US1] Filter resources during import in gladius/src/io/3mf/Importer3mf.cpp — extend the existing skip logic in loadImplicitFunctionsFiltered to also skip resources not in the dependency closure; after merge, remove non-closure build items and mesh objects from the target model; if no valid tagged function IDs found in source, log warning and fall back to full merge
- [x] T010 [US1] Verify existing tests still pass — run full unit test suite to confirm backward compatibility (no metadata → full merge path unchanged)

**Checkpoint**: Selective import works — tagged functions and dependencies imported, unrelated resources excluded, legacy files work as before

---

## Phase 4: User Story 2 — Description Display in Library Browser (Priority: P2)

**Goal**: Library Browser shows a short description below each file entry's name when `gladius:library-description` metadata is present. No description → no change in layout.

**Independent Test**: Add `gladius:library-description` metadata to a 3MF file in a library folder and verify the text appears in the Library Browser UI below the filename.

### Implementation for User Story 2

- [x] T011 [P] [US2] Add description field to ThreemfFileInfo in gladius/src/ui/ThreemfFileViewer.h — add std::string description member and bool hasLibraryMetadata member to the ThreemfFileInfo struct
- [x] T012 [US2] Read description metadata during scan in gladius/src/ui/ThreemfFileViewer.cpp — in extractThumbnail() (or companion method), after loading the model for thumbnail extraction, call readLibraryMetadata(); if present, populate fileInfo.description and set fileInfo.hasLibraryMetadata = true; include LibraryMetadata.h
- [x] T013 [US2] Display description in file cards in gladius/src/ui/ThreemfFileViewer.cpp — in render(), when hasLibraryMetadata is true and description is non-empty, display description text below filename; truncate at 80 characters with "..." ellipsis; show full description via ImGui::SetItemTooltip(); increase card height by ~30px when metadata is present; when hasLibraryMetadata is false, layout is identical to current behavior

**Checkpoint**: Library Browser shows descriptions for annotated files, clean layout for unannotated files

---

## Phase 5: User Story 3 — Function Names Display in Library Browser (Priority: P2)

**Goal**: Library Browser shows which importable functions each entry offers as labels near the thumbnail.

**Independent Test**: Add `gladius:library-functions` metadata listing two function resource IDs. Verify both resolved display names appear in the Library Browser entry.

### Implementation for User Story 3

- [x] T014 [P] [US3] Add libraryFunctionNames field to ThreemfFileInfo in gladius/src/ui/ThreemfFileViewer.h — add std::vector\<std::string\> libraryFunctionNames member to the ThreemfFileInfo struct
- [x] T015 [US3] Resolve function names during scan in gladius/src/ui/ThreemfFileViewer.cpp — in extractThumbnail() (alongside T012 metadata reading), when gladius:library-functions metadata is present, parse resource IDs with parseResourceIds(); iterate model implicit functions matching model resource IDs to GetDisplayName(); populate fileInfo.libraryFunctionNames with resolved names
- [x] T016 [US3] Display function name labels in file cards in gladius/src/ui/ThreemfFileViewer.cpp — in render(), when libraryFunctionNames is non-empty, display function names as small labels in the file card (e.g., comma-separated or as individual tags); when no function names, show nothing

**Checkpoint**: Library Browser shows both description and function names for annotated library entries

---

## Phase 6: User Story 4 — Export to Library Wizard (Priority: P3)

**Goal**: Wizard UI lets users export a function from their document to a library folder with metadata stamped.

**Independent Test**: Create document with multiple functions and build items, export one function via wizard, verify resulting 3MF has correct metadata and only necessary resources.

### Implementation for User Story 4

- [x] T017 [P] [US4] Create LibraryExportDialog header in gladius/src/ui/LibraryExportDialog.h — declare LibraryExportDialog class with: LibraryExportConfig struct (selectedFunctionIds, description, categoryName, buildItemIndex, fileName), render() method, bool isConfirmed() const, void open(), private members for ImGui state (function dropdown, description buffer, category combo, filename input)
- [x] T018 [US4] Implement LibraryExportDialog UI in gladius/src/ui/LibraryExportDialog.cpp — ImGui modal dialog with: function selector dropdown (lists all implicit functions by display name), description text input (multi-line, 256 char max), category picker (combo listing existing subfolders + free-text for new), filename input with .3mf extension, build item selector (auto-selected via dependency graph, only shown when ambiguous — multiple build items reference the function), Export and Cancel buttons
- [x] T019 [US4] Implement export flow (buffer clone + prune + stamp) in gladius/src/ui/LibraryExportDialog.cpp — on confirm: WriteToBuffer the current model, ReadFromBuffer into working copy; remove all build items except the selected one; call removeUnusedResources() on working copy; call writeLibraryMetadata() to stamp function IDs and description; write working copy to libraryRoot / categoryName / fileName; if target file exists, prompt for overwrite confirmation; create category subfolder if it doesn't exist; original document is never modified
- [x] T020 [US4] Add "Export to Library" menu entry in gladius/src/ui/MainWindow.cpp — add menu item under File menu (e.g., "Export to Library..."); instantiate LibraryExportDialog; pass current document model and library root directory; wire up dialog open/render/confirm flow
- [x] T021 [US4] Refresh Library Browser after export in gladius/src/ui/MainWindow.cpp — after successful export, trigger a rescan of the library directory so the newly exported entry appears in the Library Browser immediately

**Checkpoint**: Users can export functions to library, and the exported file can be re-imported via selective import (round-trip)

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [x] T022 [P] Add Doxygen documentation for LibraryMetadata public API in gladius/src/io/3mf/LibraryMetadata.h — document readLibraryMetadata, writeLibraryMetadata, parseResourceIds, serializeResourceIds with /// comments
- [x] T023 Run quickstart.md validation — follow instructions in specs/018-library-metadata/quickstart.md to verify the full workflow end-to-end (build, scan, selective import, export wizard round-trip)
- [x] T024 Verify all existing tests pass — run full test suite via "Run Unit Tests (Fast)" task to confirm zero regressions

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 — BLOCKS all user stories
- **US1 Selective Import (Phase 3)**: Depends on Phase 2 — core MVP
- **US2 Description Display (Phase 4)**: Depends on Phase 2 — can run parallel with US1
- **US3 Function Names Display (Phase 5)**: Depends on Phase 2 — can run parallel with US1/US2 (same file as US2 so serialize if single developer)
- **US4 Export Wizard (Phase 6)**: Depends on Phase 2 — can run parallel with US1-US3 (different files)
- **Polish (Phase 7)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational → No dependencies on other stories
- **User Story 2 (P2)**: Can start after Foundational → Shares ThreemfFileViewer.h/cpp with US3
- **User Story 3 (P2)**: Can start after Foundational → Shares ThreemfFileViewer.h/cpp with US2; serialize T011/T014 (same file) if single developer
- **User Story 4 (P3)**: Can start after Foundational → Independent files (LibraryExportDialog, MainWindow)

### Within Each User Story

- Tests (US1) written FIRST, must FAIL before implementation
- Models/utilities before services
- Core implementation before integration
- Story complete before moving to next priority

### Parallel Opportunities

- T003 and T004 (foundational tests) can run in parallel (same file but independent test functions)
- T005 and T011 and T014 and T017 can all run in parallel (different files)
- US1 (Importer3mf) and US2/US3 (ThreemfFileViewer) and US4 (LibraryExportDialog) touch different files — fully parallelizable across developers

---

## Parallel Example: User Story 1

```bash
# Write tests first (T005 in parallel with other US tests):
Task T005: "Create selective import test file in gladius/tests/unittests/SelectiveImport_test.cpp"

# Then sequential implementation (same file - Importer3mf.cpp):
Task T007: "Read library metadata in merge()"
Task T008: "Build dependency closure on source model"
Task T009: "Filter resources during import"
```

## Parallel Example: Full Feature (Multi-Developer)

```bash
# After Phase 2 (Foundational) completes:

# Developer A (Importer3mf):
T005 → T006 → T007 → T008 → T009 → T010

# Developer B (ThreemfFileViewer):
T011 → T012 → T013 → T014 → T015 → T016

# Developer C (LibraryExportDialog):
T017 → T018 → T019 → T020 → T021
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T002)
2. Complete Phase 2: Foundational (T003-T004)
3. Complete Phase 3: User Story 1 — Selective Import (T005-T010)
4. **STOP and VALIDATE**: Test selective import independently
5. Deploy/demo if ready — core value delivered

### Incremental Delivery

1. Setup + Foundational → LibraryMetadata utilities ready
2. Add US1 (Selective Import) → Test → Core feature working (**MVP!**)
3. Add US2 + US3 (Browser Display) → Test → Better discoverability
4. Add US4 (Export Wizard) → Test → Library growth enabled
5. Each story adds value without breaking previous stories

---

## Summary

| Metric | Value |
|--------|-------|
| **Total tasks** | 24 |
| **Setup tasks** | 2 (T001-T002) |
| **Foundational tasks** | 2 (T003-T004) |
| **US1 tasks** | 6 (T005-T010) |
| **US2 tasks** | 3 (T011-T013) |
| **US3 tasks** | 3 (T014-T016) |
| **US4 tasks** | 5 (T017-T021) |
| **Polish tasks** | 3 (T022-T024) |
| **Parallel opportunities** | US1/US2+US3/US4 fully parallelizable after Foundational |
| **MVP scope** | Phases 1-3 (T001-T010): 10 tasks |
| **Format validation** | ✅ All 24 tasks follow checklist format (checkbox, ID, labels, file paths) |
