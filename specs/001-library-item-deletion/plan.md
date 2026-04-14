# Implementation Plan: Library Item Deletion with Bin Recovery

**Branch**: `001-library-item-deletion` | **Date**: 2026-04-14 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-library-item-deletion/spec.md`

## Summary

Add soft-delete for user-created library entries: instead of `fs::remove()`, move entries to a `.bin/` folder mirroring category subfolders inside the user library directory (`~/.local/share/gladius/library/.bin/`). Shipped entries (including synced copies) remain protected. Bin contents can be browsed, restored, and permanently emptied. Changes affect `LibraryTool` (backend/MCP), `ThreemfFileViewer` (UI context menu), and `LibraryBrowser` (bin tab). New MCP tools are added for bin browse/restore/empty operations.

## Technical Context

**Language/Version**: C++20 (Clang, Linux primary target)
**Primary Dependencies**: lib3mf, ImGui (for UI), fmt (for string formatting), nlohmann/json (MCP JSON), sago::platform_folders (user paths), std::filesystem
**Storage**: Local filesystem — user library at `~/.local/share/gladius/library/`, shipped library at `<appDir>/library/`
**Testing**: GTest/GMock — existing fixtures in `MCP_LibraryTool_tests.cpp` and `FileSystemUtils_test.cpp`
**Target Platform**: Linux (primary), Windows (secondary)
**Project Type**: Single C++ project (gladius)
**Performance Goals**: Delete/restore operations complete in <100ms (filesystem I/O only, no GPU)
**Constraints**: No data loss on soft-delete; shipped entries must remain protected; `.bin/` must be invisible to normal library browsing
**Scale/Scope**: ~10-30 library entries typical; bin may accumulate slowly

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | PASS | Uses `std::filesystem`, smart pointers, east-side const, exceptions for errors |
| II. Test-First Development | PASS | New unit tests in GTest for bin operations; extend existing `MCP_LibraryTool_tests` and `FileSystemUtils_test` fixtures |
| III. Simplicity First (KISS/DRY/YAGNI) | PASS | Reuses existing `resolveEntryPath()` with shipped-detection enhancement; bin mirrors category structure (no metadata sidecar needed); no new abstractions beyond what exists |
| IV. Consistent Code Style | PASS | Follows existing Allman brace style, camelCase, `m_` prefix conventions |
| V. Documentation & Comments | PASS | New public API methods documented with Doxygen `///` comments |
| VI. UI Responsiveness | PASS | Filesystem move/delete operations are <100ms; no GPU or long-running compute; inline notifications use existing ImGui patterns |

All gates pass. No violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/001-library-item-deletion/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (MCP tool schemas)
│   └── mcp-bin-tools.json
├── checklists/
│   └── requirements.md
└── tasks.md             # Phase 2 output (NOT created by /speckit.plan)
```

### Source Code (files to modify/create)

```text
gladius/src/
├── FileSystemUtils.h          # Add getBinDir(), isShippedEntry() helpers
├── FileSystemUtils.cpp        # Implement bin path and shipped-detection logic
├── mcp/
│   └── tools/
│       └── LibraryTool.h      # Add bin operation method declarations
│       └── LibraryTool.cpp    # Modify deleteLibraryEntry() → soft-delete;
│                               # add binBrowse(), binRestore(), binEmpty(), binPermanentDelete()
├── mcp/
│   ├── MCPApplicationInterface.h  # Add virtual methods for bin operations
│   ├── ApplicationMCPAdapter.h    # Add forwarding methods
│   ├── ApplicationMCPAdapter.cpp  # Delegate to LibraryTool
│   └── MCPServer.cpp             # Register new MCP tools
└── ui/
    ├── ThreemfFileViewer.h    # Add right-click context menu callback
    ├── ThreemfFileViewer.cpp  # Implement context menu with delete option
    ├── LibraryBrowser.h       # Add bin tab, wire delete/restore/empty callbacks
    └── LibraryBrowser.cpp     # Implement bin browsing UI

gladius/tests/
├── apitests/
│   └── MCP_LibraryTool_tests.cpp  # Extend with bin operation tests
└── unittests/
    └── FileSystemUtils_test.cpp   # Add isShippedEntry() and bin path tests
```

**Structure Decision**: All changes fit within the existing source layout. No new directories or projects needed. The `.bin/` folder is created at runtime inside the user library directory, not in the source tree.

## Complexity Tracking

No constitution violations. Table intentionally empty.

## Post-Design Constitution Re-Check

*Re-evaluated after Phase 1 design completion.*

| Principle | Status | Post-Design Notes |
|-----------|--------|-------------------|
| I. Modern C++ Standards | PASS | `std::filesystem::rename()` for move, `std::error_code` for non-throwing checks |
| II. Test-First Development | PASS | Tests defined for all 5 MCP tools + filesystem utils; extend existing fixtures |
| III. Simplicity First | PASS | No new abstractions; bin = folder; shipped check = `fs::exists()`; dot-prefix filter = 1 line |
| IV. Consistent Code Style | PASS | All proposed code follows Allman braces, camelCase, `m_` prefix conventions |
| V. Documentation & Comments | PASS | MCP tools documented via JSON schema contracts; quickstart.md covers all usage paths |
| VI. UI Responsiveness | PASS | All operations are synchronous filesystem I/O (<100ms); no GPU work, no blocking |

## Generated Artifacts

| Artifact | Path | Description |
|----------|------|-------------|
| Research | [research.md](research.md) | 5 research topics resolved: shipped detection, bin visibility, ImGui context menus, filename disambiguation, bin tab UX |
| Data Model | [data-model.md](data-model.md) | Entities, relationships, filesystem layout, state transitions, validation rules |
| MCP Contracts | [contracts/mcp-bin-tools.json](contracts/mcp-bin-tools.json) | JSON schemas for all 5 MCP tools (modified `delete_library_entry` + 4 new bin tools) |
| Quickstart | [quickstart.md](quickstart.md) | Usage guide for UI and MCP tool workflows |

## Next Step

Run `/speckit.tasks` to generate the task breakdown from this plan.
