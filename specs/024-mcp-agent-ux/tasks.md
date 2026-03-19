# Tasks: MCP Agent UX Improvements

**Feature**: `024-mcp-agent-ux`  
**Input**: [spec.md](spec.md), [plan.md](plan.md), [contracts/](contracts/)  
**Generated**: 2026-03-17

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (touches different files, no dependencies on incomplete tasks in this phase)
- **[Story]**: User story label [US2]–[US8]
- US1 (Function Round-Trip) and US5 (Library Entry from Snippet) are **already implemented** per plan.md — no implementation tasks needed

## Status Summary

| Story | Priority | Status | Notes |
|-------|----------|--------|-------|
| US1 — Function Round-Trip | P1 | ✅ Done | `get_function_snippet` already returns `arguments` + `output_type` |
| US2 — Function Evaluation | P1 | ✅ Done | `FunctionEvaluatorTool` + OpenCL wiring |
| US3 — Rich Document Inspection | P1 | ✅ Done | `get_3mf_structure` enriched per function |
| US4 — Library Discovery | P2 | ✅ Done | Keyword filter + tags + snippet in library tools |
| US5 — Library Entry from Snippet | P2 | ✅ Done | FR-014b failure path + atomic write |
| US6 — Change Notifications | P2 | ✅ Done | `get_changes_since` tool + change log |
| US7 — API Surface Cleanup | P3 | ✅ Done | Centralized usage_example enrichment |
| US8 — Skills Update | P3 | ✅ Done | SKILL.md v3.0 with full tool inventory |

---

## Phase 1: Foundational (Library Metadata — blocks US4)

**Purpose**: Extend the `LibraryMetadata` data model with tags support. Required before US4 library work can begin.

**⚠️ CRITICAL**: Complete before starting US4 tasks.

- [X] T001 Add `libraryTags` field and `LIBRARY_TAGS_KEY` constant to `LibraryMetadata` struct in `gladius/src/io/3mf/LibraryMetadata.h`
- [X] T002 Update `readLibraryMetadata` and `writeLibraryMetadata` to read/write the `gladius:library-tags` metadata key (comma-separated) in `gladius/src/io/3mf/LibraryMetadata.cpp`

**Checkpoint**: Tags can be persisted to and read from `.3mf` library files.

---

## Phase 2: US2 — Numerical Function Evaluation (Priority: P1) 🎯 MVP

**Goal**: Agents can evaluate a function at arbitrary 3D sample points and receive numeric output values via OpenCL, enabling SDF verification without rendering.

**Independent Test**: Create a sphere SDF (`length(pos) - 1`) → call `evaluate_function` at `[0,0,0]`, `[1,0,0]`, `[2,0,0]` → expect results `[-1.0, 0.0, 1.0]`. No render required.

- [X] T003 [P] [US2] Create `FunctionEvaluatorTool.h` in `gladius/src/mcp/tools/` — declare `SamplePoint` type alias, `FunctionEvaluatorTool` class (subclass of `MCPToolBase`), and `evaluateFunction(uint32_t functionId, nlohmann::json const& samples)` public method
- [X] T004 [P] [US2] Add `virtual nlohmann::json evaluateFunction(uint32_t functionId, nlohmann::json const& samples) = 0` to `MCPApplicationInterface.h`
- [X] T005 [US2] Implement `FunctionEvaluatorTool.cpp` — parse sample points from JSON, compile the function via the existing OpenCL compute infrastructure (reuse `ToOCLVisitor` / `ComputeContext` path), submit a 1D kernel over sample points, read back float results; return structured error `{"success": false, "error": "..."}` for compile failures, unsupported node types, or argument mismatches; support `float` and `vec3` output types (FR-004, FR-005, FR-006, FR-007)
- [X] T006 [US2] Add `evaluateFunction` delegation method to `ApplicationMCPAdapter.h/.cpp` forwarding to `m_functionEvaluatorTool` (FR-004)
- [X] T007 [US2] Register `evaluate_function` tool in `MCPServer.cpp` using the schema from `contracts/evaluate_function.json`; instantiate and hold `FunctionEvaluatorTool` in `MCPServer`

**Checkpoint**: An agent can call `evaluate_function` against a loaded function and receive one float value per sample point.

---

## Phase 3: US3 — Rich Document Inspection (Priority: P1)

**Goal**: A single `get_3mf_structure` call returns function signatures, argument lists, output types, snippet previews, and constant parameter values — enough to understand the model without further queries.

**Independent Test**: Open any example 3MF → call `get_3mf_structure` → each function resource contains `arguments`, `output_type`, `snippet_preview` (≥ 3 lines), and constant node values.

- [X] T008 [US3] Enrich `get_3mf_structure` in `ApplicationMCPAdapter.cpp`: for each function resource call `getFunctionSnippet(resourceId)` and add `arguments`, `output_type`, and `snippet_preview` (first 3 lines of the snippet, or full snippet if shorter) to the function's JSON entry (FR-008)
- [X] T009 [US3] Add constant node parameter values to each function's node list in `get_3mf_structure` — iterate the function's graph and include `{name, value}` for each constant/parameter node (FR-009)

**Checkpoint**: `get_3mf_structure` is self-contained; no follow-up `get_function_snippet` calls needed to understand function signatures.

---

## Phase 4: US4 — Library Discovery and Metadata Search (Priority: P2)

**Goal**: Agents can find relevant library entries by keyword and get full entry details (signature, snippet, tags) without iterating the whole catalog.

**Independent Test**: Call `list_library` with `query: "sphere"` → only sphere entries returned. Call `get_library_entry_info` → response includes `snippet` and `tags` fields.

*Depends on Phase 1 (T001, T002) for tags persistence.*

- [X] T010 [US4] Add optional `query` string parameter to `LibraryTool::listLibrary` in `gladius/src/mcp/tools/LibraryTool.cpp` — filter entries by case-insensitive substring match against `name`, `description`, and `tags`; return empty list (not error) when no matches (FR-010)
- [X] T011 [US4] Extend `LibraryTool::getLibraryEntryInfo` in `gladius/src/mcp/tools/LibraryTool.cpp` to return `snippet` (full snippet body of the tagged function) and `tags` (decoded from `libraryTags` comma-separated string) in the response (FR-011)
- [X] T012 [US4] Extend `LibraryTool::setLibraryMetadata` in `gladius/src/mcp/tools/LibraryTool.cpp` to accept and persist a `tags` array (encode as comma-separated string into `libraryTags` field) (FR-012)
- [X] T013 [P] [US4] Update `ApplicationMCPAdapter::listLibrary` signature in `ApplicationMCPAdapter.h/.cpp` to accept and pass through the `query` parameter to `LibraryTool::listLibrary`
- [X] T014 [P] [US4] Update `list_library` and `set_library_metadata` tool schemas in `MCPServer.cpp`: add optional `query` string to `list_library`; add optional `tags` array to `set_library_metadata` (per `contracts/list_library_extended.json`)

**Checkpoint**: Agents can search the library by keyword and retrieve full entry details including code snippets and tags.

---

## Phase 5: US5 — Library Entry Creation Failure Handling (Priority: P2, Minimal)

**Goal**: `create_library_entry` fails gracefully and atomically when thumbnail rendering or bounding box computation fails — no partial files are written.

**Independent Test**: Call `create_library_entry` with a function that produces an empty/degenerate surface → response returns `success: false`, `reason: "thumbnail_render_failed"` or `reason: "invalid_bounding_box"`, and no `.3mf` file is written to the library directory.

*Note: The `snippet` parameter and happy path are already implemented. Only the failure-path handling is added here.*

- [X] T015 [US5] Implement FR-014b failure path in `LibraryTool::createLibraryEntry` in `gladius/src/mcp/tools/LibraryTool.cpp`: auto-render thumbnail and compute bounding box before writing the entry; if thumbnail render fails return `{"success": false, "reason": "thumbnail_render_failed", "message": "..."}` without writing the file; if bounding box is degenerate/NaN/zero-volume return `{"success": false, "reason": "invalid_bounding_box", "message": "..."}` without writing the file; ensure write is atomic (write to temp path, rename on success)

**Checkpoint**: `create_library_entry` is fully atomic — no partial state is left on disk on failure.

---

## Phase 6: US6 — Collaborative Editing with Change Notifications (Priority: P2)

**Goal**: Agents can query what changed in the open document since a given timestamp, enabling safe collaboration with a human working in the UI simultaneously.

**Independent Test**: Record timestamp → call `set_function_snippet` to modify a function → call `get_changes_since` with the recorded timestamp → response lists one "modified" entry for the changed function. Test headless mode: returns empty `changes` list when no changes occurred.

- [X] T016 [P] [US6] Add `ChangeEntry` struct, `m_changeLog` deque (max 1000 entries), `recordChange()`, and `getChangesSince()` declarations to `ApplicationMCPAdapter.h` (FR-015, FR-016, FR-017)
- [X] T017 [P] [US6] Add `virtual nlohmann::json getChangesSince(std::string const& isoTimestamp) const = 0` to `MCPApplicationInterface.h`
- [X] T018 [US6] Implement `getChangesSince(isoTimestamp)` in `ApplicationMCPAdapter.cpp`: parse the ISO-8601 UTC timestamp using `std::chrono`, filter `m_changeLog` to entries after that timestamp, return structured JSON with `success: true` and `changes` array (FR-015, FR-016, FR-017, FR-018)
- [X] T019 [US6] Instrument all write-path methods in `ApplicationMCPAdapter.cpp` to call `recordChange()` after successful mutation: `setFunctionSnippet`, `setProgramSnippet`, `setParameter`, `createFunctionFromSnippet`, `createFunctionFromExpression`, `createLevelSet`, `openDocument`, `createDocument` (FR-015)
- [X] T020 [US6] Install a `Document` modified callback in `ApplicationMCPAdapter` constructor to call `recordChange("modified", "document", 0, "")` for UI-driven changes (e.g. user edits in the node editor) (FR-015, FR-016)
- [X] T021 [P] [US6] Add `server_time` field (ISO-8601 UTC via `std::chrono::system_clock`) to `get_status` response in `MCPServer.cpp` (FR-019)
- [X] T022 [US6] Register `get_changes_since` tool in `MCPServer.cpp` using the schema from `contracts/get_changes_since.json`; wire to `m_application->getChangesSince(since)`

**Checkpoint**: Agent can detect all user-driven document changes with a single `get_changes_since` call; returns empty list in headless mode when no changes occurred.

---

## Phase 7: US7 — API Surface Cleanup (Priority: P3)

**Goal**: Clean, minimal MCP tool set — no debug tools, all responses carry `success` field, all error responses carry `usage_example`.

**Independent Test**: After cleanup, `tools/list` contains no debug-only tools (`ping`, `test_computation`, `list_tools`). All inline tool lambdas in `MCPServer.cpp` that bypass `createToolError` include `success: true/false` and `usage_example` in errors.

- [X] T023 [US7] Verify and remove any remaining `ping`, `test_computation`, and `list_tools` tool registrations from `MCPServer.cpp` and all tool files in `gladius/src/mcp/tools/` (FR-020)
- [X] T024 [US7] Audit all inline tool handler lambdas in `MCPServer.cpp` that construct response JSON directly: add `"success", true` to success responses and `"usage_example"` JSON fragment to error responses that are missing them (FR-021, FR-022) — focus on `get_status`, `open_document`, `save_document`, `create_document`, and any other handlers not delegating to `createToolError`

**Checkpoint**: `tools/list` is clean; every tool response has a top-level `success` field; every error response has a `usage_example`.

---

## Phase 8: US8 — Updated MCP Skills Documentation (Priority: P3)

**Goal**: The `creating-library-items` skill accurately documents all new and changed tools so agents loaded with the skill can operate correctly without consulting additional sources.

**Independent Test**: An agent loaded with only the updated skill file can complete the creating-library-items workflow, call `evaluate_function`, use `get_changes_since`, and search the library — all without additional documentation.

- [X] T025 [US8] Update `.github/skills/creating-library-items/SKILL.md`: (1) document the one-step `create_library_entry` snippet workflow as the primary path (FR-023); (2) add `evaluate_function` usage section with sample request/response; (3) add `get_changes_since` collaboration workflow (record timestamp → act → query → inspect); (4) add complete tool inventory table with one-line descriptions for all current MCP tools (FR-024); (5) remove or mark deprecated any workflow steps that relied on the old multi-step expression-only path

**Checkpoint**: Skill file is self-contained and reflects the full feature-024 API surface.

---

## Dependencies

```
T001 → T002                                (Foundational: tags data model)
T001, T002 → T010, T011, T012             (US4 library tools need tags)

T003, T004 → T005 → T006 → T007          (US2 eval: header → impl → adapter → server)

T008 → T009                               (US3: snippet_preview before constants)

T010, T011, T012 → T013 → T014           (US4: LibraryTool changes before adapter/server)

T016, T017 → T018 → T019 → T020 → T022  (US6: struct → impl → instrument → register)
T021 independent (US6: server_time in get_status)

Phase 2 (US2) and Phase 3 (US3) are independent of Phase 1 and each other.
Phase 4 (US4) depends on Phase 1.
Phase 5 (US5) is independent.
Phases 6, 7, 8 are independent of all prior phases.
```

### Parallel Execution Opportunities

**After Phase 1 complete**, these phases can run in parallel across team members:
- Phase 2 (US2: evaluate_function) ← most complex, start first
- Phase 3 (US3: get_3mf_structure) ← touches ApplicationMCPAdapter.cpp
- Phase 5 (US5: create_library_entry failure path) ← touches LibraryTool.cpp only

**After Phase 1 + Phase 5 complete**:
- Phase 4 (US4: library search + tags) ← touches LibraryTool.cpp + MCPServer.cpp

**Any time**:
- Phase 8 (US8: skills documentation) ← purely docs, no source conflicts

**Within Phase 6** (US6): T016 + T017 + T021 can be parallelised (different files).

---

## Implementation Strategy

**MVP scope**: Phase 2 (US2) + Phase 3 (US3) deliver the highest-value P1 stories and are independently testable without UI or library.

**Recommended order**:
1. Phase 1 (Foundational) — unblocks US4
2. Phase 2 (US2) + Phase 3 (US3) in parallel — P1 value
3. Phase 4 (US4) + Phase 5 (US5) + Phase 6 (US6) in parallel — P2 value
4. Phase 7 (US7) + Phase 8 (US8) — P3 polish

---

## Task Count Summary

| Phase | Story | Tasks | Parallel |
|-------|-------|-------|----------|
| Phase 1: Foundational | — | 2 | 0 |
| Phase 2: US2 eval | P1 | 5 | 2 (T003, T004) |
| Phase 3: US3 inspection | P1 | 2 | 0 |
| Phase 4: US4 library | P2 | 5 | 2 (T013, T014) |
| Phase 5: US5 failure path | P2 | 1 | 0 |
| Phase 6: US6 change log | P2 | 7 | 3 (T016, T017, T021) |
| Phase 7: US7 cleanup | P3 | 2 | 0 |
| Phase 8: US8 skills | P3 | 1 | 1 |
| **Total** | | **25** | **8** |
