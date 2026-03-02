# Quickstart: Library Metadata for Selective Function Import

**Feature**: 018-library-metadata

## What This Feature Does

Adds metadata to 3MF library files that tags which functions should be imported when a user double-clicks a library entry. Library entries also display a short description and the names of importable functions. An export wizard lets users create new library entries from their documents.

## Architecture Overview

```
┌─────────────────┐    ┌──────────────────────┐    ┌─────────────────────┐
│  Library Browser │    │   Importer3mf        │    │  Export Wizard      │
│  (UI display)    │    │   (selective merge)   │    │  (create entries)   │
│                  │    │                       │    │                     │
│ ThreemfFileInfo  │    │ merge()               │    │ LibraryExportConfig │
│ + description    │    │ + read metadata       │    │ + buffer clone      │
│ + functionNames  │    │ + build dep graph     │    │ + removeUnused      │
│ + hasLibraryMeta │    │ + filter by closure   │    │ + stamp metadata    │
└────────┬─────────┘    └───────────┬───────────┘    └──────────┬──────────┘
         │                          │                            │
         └──────────────────────────┴────────────────────────────┘
                                    │
                          3MF Model-level Metadata
                          gladius:library-functions
                          gladius:library-description
```

## Key Files to Touch

| File | Change Type | Purpose |
|------|-------------|---------|
| `gladius/src/ui/ThreemfFileViewer.h` | Modify | Add metadata fields to `ThreemfFileInfo` |
| `gladius/src/ui/ThreemfFileViewer.cpp` | Modify | Read metadata during scan, display in render |
| `gladius/src/io/3mf/Importer3mf.h` | Modify | Add `mergeSelective()` signature, `LibraryMetadata` struct |
| `gladius/src/io/3mf/Importer3mf.cpp` | Modify | Implement selective merge, read metadata in merge() |
| `gladius/src/io/3mf/LibraryMetadata.h` | **New** | Metadata reading/writing utilities |
| `gladius/src/io/3mf/LibraryMetadata.cpp` | **New** | Metadata parsing, serialization |
| `gladius/src/ui/LibraryExportDialog.h` | **New** (P3) | Export wizard UI |
| `gladius/src/ui/LibraryExportDialog.cpp` | **New** (P3) | Export wizard implementation |
| `gladius/src/ui/MainWindow.cpp` | Modify (P3) | Add "Export to Library" menu entry |
| `gladius/tests/unittests/LibraryMetadata_test.cpp` | **New** | Unit tests for metadata parsing |
| `gladius/tests/unittests/SelectiveImport_test.cpp` | **New** | Unit tests for selective import |

## Implementation Phases

### Phase 1 — Metadata Infrastructure + Selective Import (P1)
1. Create `LibraryMetadata.h/.cpp` — parse/write metadata keys
2. Modify `Importer3mf::merge()` — read metadata, build dep graph on source, filter
3. Unit tests for metadata parsing and selective import

### Phase 2 — UI Display (P2)
4. Extend `ThreemfFileInfo` with metadata fields
5. Read metadata during scan in `ThreemfFileViewer`
6. Display description + function names in file cards

### Phase 3 — Export Wizard (P3)
7. Create `LibraryExportDialog` UI
8. Implement buffer-clone + prune + stamp flow
9. Add menu entry in MainWindow

## Build & Test

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run unit tests
# Use VS Code task: "Run Unit Tests (Fast)"

# Run specific test suite
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=LibraryMetadata*
./gladius_test --gtest_filter=SelectiveImport*
```
