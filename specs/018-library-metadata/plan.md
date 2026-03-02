# Implementation Plan: Library Metadata for Selective Function Import

**Branch**: `018-library-metadata` | **Date**: 2026-02-14 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/018-library-metadata/spec.md`

## Summary

Add model-level 3MF metadata (`gladius:library-functions`, `gladius:library-description`) to library files so the import operation can selectively merge only tagged functions and their transitive dependencies, instead of the entire file. The Library Browser displays descriptions and function names read eagerly during scan. An export wizard creates library entries from the current document by cloning to buffer, pruning unused resources, and stamping metadata.

**Technical approach**: Use lib3mf's `CMetaDataGroup` API at model level. Build `ResourceDependencyGraph` on the source model before merge to compute the dependency closure. Filter resources during import. For export, use `WriteToBuffer`/`ReadFromBuffer` for a deep copy, then `removeUnusedResources` + metadata stamping.

## Technical Context

**Language/Version**: C++20  
**Primary Dependencies**: lib3mf (3MF format), ImGui (UI), OpenCL 1.2+ (GPU compute)  
**Storage**: 3MF files (model-level metadata groups), filesystem directories for library categories  
**Testing**: GTest/GMock, VS Code tasks for build/test  
**Target Platform**: Linux (primary), Windows (MSVC)  
**Project Type**: Single C++ project (CMake + vcpkg + Ninja)  
**Performance Goals**: Library scan with metadata adds <10ms overhead per file; import latency unchanged  
**Constraints**: `GetMetaDataByKey` throws on missing key (must wrap in try/catch); model resource IDs only (not container-scoped unique IDs)  
**Scale/Scope**: ~15 library entries typical; hundreds at most

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Principle I — Modern C++ Standards ✅ PASS

- Uses STL containers (`std::vector`, `std::string`, `std::unordered_set`)
- Smart pointers for lib3mf wrappers (already managed by lib3mf's `PModel`, `PMetaDataGroup`)
- `constexpr` not applicable (metadata values are runtime strings)
- East-side const used throughout
- Exceptions used for error handling; `GetMetaDataByKey` throws on missing key

### Principle II — Test-First Development ✅ PASS

- Unit tests for metadata parsing (`LibraryMetadata_test.cpp`)
- Unit tests for selective import logic (`SelectiveImport_test.cpp`)
- Test naming: `ReadMetadata_WithLibraryFunctions_ReturnsIds`, `MergeSelective_WithMetadata_ImportsOnlyTagged`
- Arrange-Act-Assert pattern; tests in `namespace::tests`
- No GPU dependency for metadata/import tests

### Principle III — Simplicity First ✅ PASS

- No new abstractions — extends existing `merge()` flow and `ThreemfFileInfo` struct
- Metadata reading is 2 API calls added to an existing scan function
- Export wizard reuses `removeUnusedResources()` + `WriteToBuffer`/`ReadFromBuffer`
- New files kept small (<400 lines each)

### Principle IV — Code Style ✅ PASS

- `camelCase` for functions (`readLibraryMetadata`, `mergeSelective`)
- `PascalCase` for types (`LibraryMetadata`, `LibraryExportConfig`)
- `m_` prefix for new members (`m_description`, `m_hasLibraryMetadata`)
- Allman braces, 4-space indent
- `#pragma once` for new headers

### Principle V — Documentation ✅ PASS

- Doxygen `///` for public API (`readLibraryMetadata`, `writeLibraryMetadata`)
- Comments only where adding value (metadata key format, try/catch rationale)

### Principle VI — UI Responsiveness ✅ PASS

- Metadata reading is synchronous but negligible cost (model already open)
- Export wizard: buffer clone + prune is fast for typical library files (<100ms)
- If export proves slow for large models, async with progress can be added later (YAGNI)

### Post-Design Re-evaluation ✅ PASS (no changes from pre-design)

## Project Structure

### Documentation (this feature)

```text
specs/018-library-metadata/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 research findings
├── data-model.md        # Entity definitions
├── quickstart.md        # Developer quickstart
├── checklists/
│   └── requirements.md  # Requirements checklist
└── tasks.md             # Phase 2 output (NOT created by /speckit.plan)
```

### Source Code

```text
gladius/src/io/3mf/
├── LibraryMetadata.h          # NEW — metadata read/write utilities
├── LibraryMetadata.cpp        # NEW — metadata parsing, serialization
├── Importer3mf.h              # MODIFY — add mergeSelective(), LibraryMetadata include
└── Importer3mf.cpp            # MODIFY — implement selective merge in merge()

gladius/src/ui/
├── ThreemfFileViewer.h        # MODIFY — extend ThreemfFileInfo struct
├── ThreemfFileViewer.cpp      # MODIFY — read metadata during scan, display in render
├── LibraryExportDialog.h      # NEW (P3) — export wizard UI
├── LibraryExportDialog.cpp    # NEW (P3) — export wizard implementation
└── MainWindow.cpp             # MODIFY (P3) — add "Export to Library" menu entry

gladius/tests/unittests/
├── LibraryMetadata_test.cpp   # NEW — metadata parsing tests
└── SelectiveImport_test.cpp   # NEW — selective import integration tests
```

**Structure Decision**: Single project layout. New files placed in existing directories (`io/3mf/` for metadata logic, `ui/` for export wizard). No new directories needed. Test files follow existing pattern in `tests/unittests/`.

## Implementation Phases

### Phase 1 — Metadata Infrastructure (P1/P2 foundation)

**Goal**: Create `LibraryMetadata` utilities for reading/writing metadata keys.

**Files**:
- Create `gladius/src/io/3mf/LibraryMetadata.h` — `LibraryMetadata` struct, `readLibraryMetadata()`, `writeLibraryMetadata()`, `parseResourceIds()`
- Create `gladius/src/io/3mf/LibraryMetadata.cpp` — implementation
- Create `gladius/tests/unittests/LibraryMetadata_test.cpp` — unit tests

**Key design decisions**:
- `readLibraryMetadata(Lib3MF::PModel)` returns `std::optional<LibraryMetadata>` (empty if no metadata found)
- `parseResourceIds(std::string const&)` returns `std::vector<Lib3MF_uint32>` from semicolon-separated string
- `writeLibraryMetadata(Lib3MF::PModel, LibraryMetadata const&)` stamps both keys
- `GetMetaDataByKey` throws on missing key → wrap in try/catch, return `std::nullopt`

**Tests**:
- `ParseResourceIds_WithSingleId_ReturnsOneElement`
- `ParseResourceIds_WithMultipleIds_ReturnsAll`
- `ParseResourceIds_WithWhitespace_TrimsCorrectly`
- `ParseResourceIds_WithEmptyString_ReturnsEmpty`
- `ReadMetadata_WithBothKeys_ReturnsPopulated`
- `ReadMetadata_WithMissingKeys_ReturnsNullopt`
- `WriteMetadata_RoundTrip_PreservesValues`

### Phase 2 — Selective Import (P1)

**Goal**: Modify `merge()` to use metadata for selective import.

**Files**:
- Modify `gladius/src/io/3mf/Importer3mf.cpp` — read metadata, build dep graph on source, compute closure, filter
- Modify `gladius/src/io/3mf/Importer3mf.h` — include `LibraryMetadata.h`
- Create `gladius/tests/unittests/SelectiveImport_test.cpp` — integration tests

**Key design decisions**:
- In `merge()`, after opening the source model but before `MergeFromModel`:
  1. Call `readLibraryMetadata(sourceModel)`.
  2. If metadata present, build `ResourceDependencyGraph` on source model.
  3. Compute dependency closure via `getAllRequiredResources()` for each tagged function ID.
  4. After merge, additionally filter out resources not in the closure (extend the existing skip logic in `loadImplicitFunctionsFiltered`).
  5. Remove non-closure build items and mesh objects after merge.
- If metadata absent, existing full-merge behavior is unchanged.
- Fallback: if tagged function IDs don't exist in the file, log warning and fall back to full merge.

**Tests**:
- `MergeSelective_WithMetadata_ImportsOnlyTaggedFunction`
- `MergeSelective_WithMetadata_ImportsDependencies`
- `MergeSelective_WithoutMetadata_FallsToFullMerge`
- `MergeSelective_WithInvalidFunctionId_FallsToFullMerge`
- `MergeSelective_WithMultipleFunctions_ImportsAll`

### Phase 3 — Library Browser UI (P2)

**Goal**: Display description and function names in the Library Browser.

**Files**:
- Modify `gladius/src/ui/ThreemfFileViewer.h` — add fields to `ThreemfFileInfo`
- Modify `gladius/src/ui/ThreemfFileViewer.cpp` — read metadata in scan, render metadata in cards

**Key design decisions**:
- `ThreemfFileInfo` gets three new fields: `std::string description`, `std::vector<std::string> libraryFunctionNames`, `bool hasLibraryMetadata`
- In `extractThumbnail()` (or companion method), after loading the model, call `readLibraryMetadata()`. If present, populate the new fields. Resolve function names by iterating model functions matching resource IDs to `GetDisplayName()`.
- In `render()`, increase card height by ~30px when metadata is present. Show description below filename (truncated at 80 chars with `...`). Show function names as small labels. Full description via `ImGui::SetItemTooltip()`.
- When `hasLibraryMetadata` is false, layout is identical to current behavior.

### Phase 4 — Export Wizard (P3)

**Goal**: Wizard UI to export a function as a library entry.

**Files**:
- Create `gladius/src/ui/LibraryExportDialog.h` — dialog class
- Create `gladius/src/ui/LibraryExportDialog.cpp` — implementation
- Modify `gladius/src/ui/MainWindow.cpp` — add menu entry

**Key design decisions**:
- ImGui modal dialog with: function selector (dropdown), description text input, category picker (combo + free-text), filename input, build item selector (auto-selected, shown only when ambiguous).
- Export flow:
  1. `WriteToBuffer` the current model → `ReadFromBuffer` into working copy.
  2. On working copy, remove all build items except the selected one.
  3. Call `removeUnusedResources()` on working copy (removes resources not needed by remaining build item).
  4. Call `writeLibraryMetadata()` to stamp function IDs and description.
  5. Write working copy to `libraryRoot / categoryName / fileName`.
- Menu entry: "File → Export to Library…" (or context menu on a function node).
- Original document is never modified.

## Complexity Tracking

> No constitution violations identified. All principles pass.
