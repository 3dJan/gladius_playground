# Implementation Plan: Test Suite Restructuring

**Branch**: `004-test-suite-restructure` | **Date**: January 3, 2026 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/004-test-suite-restructure/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/commands/plan.md` for the execution workflow.

## Summary

Restructure the Gladius test suite from a monolithic unit test executable into three distinct categories (unit, integration, api) with CTest labels for component-based filtering. Unit tests must complete in <60 seconds without GPU dependencies. Integration tests contain all GPU/OpenCL-dependent and long-running tests. API tests validate external interfaces (GladiusLib, MCP).

## Technical Context

**Language/Version**: C++20 (as per constitution)  
**Primary Dependencies**: GTest/GMock, CMake 3.21+ (for test presets), CTest  
**Storage**: N/A  
**Testing**: GTest/GMock with CTest for test organization and execution  
**Target Platform**: Linux (primary), Windows (secondary)  
**Project Type**: Single project with multiple test executables  
**Performance Goals**: Unit tests complete in <60 seconds  
**Constraints**: No GPU/OpenCL required for unit tests, graceful skipping in integration tests  
**Scale/Scope**: ~70 test files, ~14 requiring GPU/OpenCL migration to integration tests

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Modern C++ Standards | ✅ PASS | No code changes to C++ source, only CMake/build config |
| II. Test-First Development | ✅ PASS | Reorganizing tests, not changing test behavior |
| III. Simplicity First (KISS, DRY, YAGNI) | ✅ PASS | Simplifying test organization, reducing complexity |
| IV. Consistent Code Style | ✅ PASS | Using existing naming conventions for test categories |
| V. Documentation and Comments | ✅ PASS | Documentation updates required (FR-015) |

**Technology Stack Compliance**:
- ✅ Using CMake presets (existing pattern in CMakePresets.json)
- ✅ Using GTest/GMock (existing)
- ✅ Build via VS Code tasks (tasks.json updates planned)
- ✅ No manual cmake/ninja invocation required

**Post-Phase 1 Re-check** (January 3, 2026):
- ✅ Design maintains simplicity: CTest labels vs. complex filtering logic
- ✅ No new dependencies introduced
- ✅ File migrations are straightforward (move files, update CMakeLists.txt)
- ✅ quickstart.md provides developer documentation

## Project Structure

### Documentation (this feature)

```text
specs/004-test-suite-restructure/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (test categorization model)
├── quickstart.md        # Phase 1 output (how to run tests)
├── contracts/           # Phase 1 output (N/A - no external APIs)
└── tasks.md             # Phase 2 output
```

### Source Code (repository root)

```text
gladius/
├── CMakePresets.json           # Add test presets: UnitTests, IntegrationTests, ApiTests
├── tests/
│   ├── CMakeLists.txt          # Top-level test configuration
│   ├── unittests/              # Fast CPU-only tests (<60s total)
│   │   ├── CMakeLists.txt      # Label tests with 'unit' + component scopes
│   │   └── *_tests.cpp         # ~56 files after migration
│   ├── integrationtests/       # GPU/OpenCL/slow tests
│   │   ├── CMakeLists.txt      # Label tests with 'integration' + component scopes
│   │   └── *_tests.cpp         # Existing 5 + ~14 migrated from unittests
│   └── apitests/               # NEW: API boundary tests
│       ├── CMakeLists.txt      # Label tests with 'api'
│       └── *_tests.cpp         # GladiusLib + MCP protocol tests
└── src/
    └── ...                     # No source changes required

docs/
└── developer_onboarding.md     # Update with new test organization
```

**Structure Decision**: Preserve existing two-tier structure (unittests/, integrationtests/) and add apitests/. Move GPU-dependent tests from unittests/ to integrationtests/. Use CTest labels rather than separate executables per component for flexibility.

## Complexity Tracking

> No constitution violations requiring justification.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| N/A | N/A | N/A |
