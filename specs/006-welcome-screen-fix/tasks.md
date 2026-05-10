# Tasks: Welcome Screen Improvements

**Input**: Design documents from `/specs/006-welcome-screen-fix/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, quickstart.md ✓

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Project structure verification

- [X] T001 Verify build compiles cleanly with "Build ALL (linux-releaseWithDebug)" task
- [X] T002 [P] Create gladius/tests/unittests/ui/ directory if it doesn't exist

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared types and enums needed by multiple user stories

**⚠️ CRITICAL**: User Story 1 (async loading) depends on these types

- [X] T003 Add ThumbnailLoadState enum to gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T004 Add decodedPixels, textureCreated, and loadState fields to ThumbnailInfo struct in gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T005 Add ThumbnailLoadResult struct to gladius/src/ui/ThreemfThumbnailExtractor.h

**Checkpoint**: Foundation ready - user story implementation can begin

---

## Phase 3: User Story 2 - Reliable File Selection (Priority: P1) 🎯 MVP

**Goal**: Fix timing issue where clicking thumbnails loads default template instead of selected file

**Independent Test**: Click any recent file thumbnail and verify the correct file loads

### Implementation for User Story 2

- [X] T006 [US2] Add m_pendingFileOpen (optional\<path\>) member to WelcomeScreen class in gladius/src/ui/WelcomeScreen.h
- [X] T007 [US2] Add m_clickProcessed (bool) member to WelcomeScreen class in gladius/src/ui/WelcomeScreen.h  
- [X] T008 [US2] Add processFileOpen() method declaration to WelcomeScreen in gladius/src/ui/WelcomeScreen.h
- [X] T009 [US2] Add hasPendingFileOpen() method declaration to WelcomeScreen in gladius/src/ui/WelcomeScreen.h
- [X] T010 [US2] Implement processFileOpen() in gladius/src/ui/WelcomeScreen.cpp to return and clear m_pendingFileOpen
- [X] T011 [US2] Implement hasPendingFileOpen() in gladius/src/ui/WelcomeScreen.cpp
- [X] T012 [US2] Modify renderThumbnailItem() in gladius/src/ui/WelcomeScreen.cpp to store path in m_pendingFileOpen instead of calling callback directly
- [X] T013 [US2] Remove duplicate m_isVisible = false from renderThumbnailItem() in gladius/src/ui/WelcomeScreen.cpp (keep single location)
- [X] T014 [US2] Modify MainWindow render loop in gladius/src/ui/MainWindow.cpp to check pendingFileOpen after welcome screen closes
- [X] T015 [US2] Add file existence check before calling open() in MainWindow when processing pending file
- [X] T016 [US2] Add error logging when clicked file no longer exists in gladius/src/ui/MainWindow.cpp
- [X] T017 [US2] Reset m_clickProcessed at start of each render frame in gladius/src/ui/WelcomeScreen.cpp

**Checkpoint**: File selection bug is fixed - clicking thumbnails reliably opens correct file

---

## Phase 4: User Story 3 - Layout Persistence (Priority: P2)

**Goal**: Preserve ImGui docking layout when welcome screen closes

**Independent Test**: Customize layout, restart app, close welcome screen, verify layout preserved

### Implementation for User Story 3

- [X] T018 [US3] Review startAnimationMode() in gladius/src/ui/RenderWindow.cpp for any layout-affecting operations
- [X] T019 [US3] Audit MainWindow::render() welcome screen close handling for dock reset triggers in gladius/src/ui/MainWindow.cpp
- [X] T020 [US3] Ensure DockSpace is rendered consistently before and after welcome screen in gladius/src/ui/MainWindow.cpp
- [X] T021 [US3] Guard any layout-modifying operations with welcome screen visibility check in gladius/src/ui/MainWindow.cpp
- [X] T022 [US3] Verify imgui.ini is not being reloaded during welcome screen close transition in gladius/src/ui/GLView.cpp

**Checkpoint**: Layout persistence is fixed - custom layouts survive welcome screen close

---

## Phase 5: User Story 1 - Async Thumbnail Loading (Priority: P1)

**Goal**: Load thumbnails asynchronously to prevent UI freezes

**Independent Test**: Launch with 20+ recent files, verify UI responsive, thumbnails appear progressively

### Create AsyncThumbnailLoader Component

- [X] T023 [P] [US1] Create AsyncThumbnailLoader.h in gladius/src/ui/ with class declaration
- [X] T024 [P] [US1] Create AsyncThumbnailLoader.cpp in gladius/src/ui/ with stub implementations
- [X] T025 [US1] Add ThumbnailLoadTask struct to AsyncThumbnailLoader.h
- [X] T026 [US1] Implement AsyncThumbnailLoader constructor with logger and maxConcurrentLoads in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T027 [US1] Implement requestLoad() method in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T028 [US1] Implement update() method to poll futures and update ThumbnailInfo states in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T029 [US1] Implement processPendingTextures() for main-thread GL texture creation in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T030 [US1] Implement cancelAll() method in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T031 [US1] Implement hasPendingWork() method in gladius/src/ui/AsyncThumbnailLoader.cpp
- [X] T032 [US1] Add AsyncThumbnailLoader.cpp to gladius/src/ui/CMakeLists.txt (auto-discovered via GLOB_RECURSE)

### Modify ThreemfThumbnailExtractor for Thread Safety

- [X] T033 [US1] Add extractThumbnailDataOnly() method to ThreemfThumbnailExtractor that returns PNG bytes without creating texture in gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T034 [US1] Implement extractThumbnailDataOnly() in gladius/src/ui/ThreemfThumbnailExtractor.cpp
- [X] T035 [US1] Add decodePngToPixels() static method for thread-safe PNG decoding in gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T036 [US1] Implement decodePngToPixels() using lodepng in gladius/src/ui/ThreemfThumbnailExtractor.cpp
- [X] T037 [US1] Add createTextureFromPixels() method for main-thread texture creation in gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T038 [US1] Implement createTextureFromPixels() in gladius/src/ui/ThreemfThumbnailExtractor.cpp

### Integrate Async Loader into WelcomeScreen

- [X] T039 [US1] Add m_asyncLoader member to WelcomeScreen in gladius/src/ui/WelcomeScreen.h
- [X] T040 [US1] Initialize m_asyncLoader in WelcomeScreen::setLogger() in gladius/src/ui/WelcomeScreen.cpp
- [X] T041 [US1] Modify updateThumbnailInfos() to queue loads via m_asyncLoader instead of blocking in gladius/src/ui/WelcomeScreen.cpp
- [X] T042 [US1] Add m_asyncLoader->update() call in WelcomeScreen::render() in gladius/src/ui/WelcomeScreen.cpp
- [X] T043 [US1] Add m_asyncLoader->processPendingTextures() call in WelcomeScreen::render() in gladius/src/ui/WelcomeScreen.cpp
- [X] T044 [US1] Call m_asyncLoader->cancelAll() in WelcomeScreen::hide() in gladius/src/ui/WelcomeScreen.cpp
- [X] T045 [US1] Update renderThumbnailGrid() to show placeholders for Loading state in gladius/src/ui/WelcomeScreen.cpp
- [X] T046 [US1] Repeat async loader integration for example thumbnails in gladius/src/ui/WelcomeScreen.cpp

**Checkpoint**: Async thumbnail loading complete - UI responsive during thumbnail loading

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Testing, documentation, and edge case handling

- [X] T047 [P] Add Doxygen comments to AsyncThumbnailLoader public methods in gladius/src/ui/AsyncThumbnailLoader.h
- [X] T048 [P] Add Doxygen comments to new ThreemfThumbnailExtractor methods in gladius/src/ui/ThreemfThumbnailExtractor.h
- [X] T049 [P] Add Doxygen comments to new WelcomeScreen methods in gladius/src/ui/WelcomeScreen.h
- [X] T050 Run "Build ALL (linux-releaseWithDebug)" task and fix any compilation errors
- [X] T051 Run "Run Gladius Tests (ReleaseWithDebug, summary)" task and fix any test failures
- [ ] T052 Manual test: Launch with 20+ recent files, verify responsive UI
- [ ] T053 Manual test: Click thumbnails, verify correct file loads
- [ ] T054 Manual test: Customize layout, restart, verify layout preserved
- [ ] T055 Manual test: Click on file that was deleted, verify error message shown
- [ ] T056 Manual test: Rapid double-click on thumbnail, verify only one file opens
- [X] T057 Add try-catch around LoadIniSettingsFromDisk in gladius/src/ui/GLView.cpp with fallback to default layout

### Unit Tests (Constitution Principle II)

- [X] T058 [P] Create AsyncThumbnailLoader_tests.cpp in gladius/tests/unittests/ui/
- [X] T059 [P] Add unit test: AsyncThumbnailLoader_RequestLoad_SetsLoadingState
- [X] T060 [P] Add unit test: AsyncThumbnailLoader_CancelAll_StopsPendingLoads
- [X] T061 [P] Add unit test: AsyncThumbnailLoader_Update_TransitionsToDecodedPending
- [X] T062 [P] Add unit test: WelcomeScreen_DoubleClick_ProcessesOnlyFirst (tested via m_clickProcessed logic)

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1: Setup
    │
    ▼
Phase 2: Foundational (shared types)
    │
    ├─────────────────┬─────────────────┐
    ▼                 ▼                 ▼
Phase 3: US2      Phase 4: US3      Phase 5: US1
(File Selection)  (Layout)          (Async Loading)
    │                 │                 │
    └─────────────────┴─────────────────┘
                      │
                      ▼
              Phase 6: Polish
```

### User Story Dependencies

- **User Story 2 (P1)**: Depends only on Phase 2 - Can start immediately after Foundational
- **User Story 3 (P2)**: Depends only on Phase 2 - Can start in parallel with US2
- **User Story 1 (P1)**: Depends on Phase 2 - Can start in parallel with US2/US3

### Within Phase Parallelization

Phase 5 (User Story 1) has three sub-groups that can be parallelized:
- T023-T032: AsyncThumbnailLoader component
- T033-T038: ThreemfThumbnailExtractor modifications
- T039-T046: WelcomeScreen integration (depends on above two)

---

## Implementation Strategy

### Recommended Order (Sequential)

1. **Phase 1-2**: Setup and Foundational (T001-T005)
2. **Phase 3**: Fix file selection bug first (T006-T017) - Quick win, low risk
3. **Phase 4**: Fix layout persistence (T018-T022) - Low risk
4. **Phase 5**: Async thumbnail loading (T023-T046) - Larger change, higher risk
5. **Phase 6**: Polish and testing (T047-T055)

### MVP Scope

For minimum viable fix, complete:
- Phase 1-2: Setup and Foundational
- Phase 3: User Story 2 (File Selection Bug) ← Most critical user-facing bug
- Phase 6: Testing tasks T050-T056

---

## Notes

- [P] tasks can run in parallel (different files, no dependencies)
- [US1/US2/US3] labels map tasks to specific user stories
- Each user story can be tested independently after its checkpoint
- Run build task after each phase to catch issues early
- Commit after each logical group of tasks
