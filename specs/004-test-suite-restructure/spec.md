# Feature Specification: Test Suite Restructuring

**Feature Branch**: `004-test-suite-restructure`  
**Created**: January 3, 2026  
**Status**: Draft  
**Input**: User description: "Running the tests takes ages. Some tests are currently failing. Some tests require a GPU. There are many long running tests that should be integration tests but are part of the unit test suite. Consolidate the tests and restructure them, so that we have just fast running unit tests and longer running integration tests, and API tests. It should also be easy to run parts of the test suite that cover a specific scope of the software."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Run Fast Unit Tests Quickly (Priority: P1)

As a developer making code changes, I want to run only the fast unit tests so that I get rapid feedback on whether my changes break existing functionality without waiting for slow tests to complete.

**Why this priority**: Rapid feedback during development is essential for productivity. Developers need confidence that their changes work without waiting 10+ minutes for a full test suite.

**Independent Test**: Can be fully tested by running `ctest --preset UnitTests` and verifying all tests complete in under 60 seconds on a standard development machine without GPU.

**Acceptance Scenarios**:

1. **Given** a developer has made a code change, **When** they run the unit test suite, **Then** all unit tests complete in under 60 seconds on a machine without GPU.
2. **Given** the unit test suite, **When** executed without GPU or OpenCL available, **Then** no tests fail due to missing GPU/OpenCL (tests that require these are not in this suite).
3. **Given** the unit test suite, **When** executed, **Then** tests do not require external tools like `admesh` or network access.

---

### User Story 2 - Run Tests by Scope/Component (Priority: P2)

As a developer working on a specific component (e.g., mesh export, dual contouring, MCP), I want to run only the tests relevant to that component so that I can focus my testing on the area I'm modifying.

**Why this priority**: Component-focused testing reduces test time and helps developers understand which tests cover their changes. This is critical for large codebases with many test files.

**Independent Test**: Can be tested by running tests for a specific component (e.g., `ctest -L mesh-export`) and verifying only relevant tests execute.

**Acceptance Scenarios**:

1. **Given** a developer is working on mesh export functionality, **When** they run tests filtered by the "mesh-export" scope, **Then** only mesh-related tests execute.
2. **Given** a developer wants to see available test scopes, **When** they query the test system, **Then** they receive a list of all defined scopes (e.g., mesh-export, dual-contouring, mcp, opencl, io).
3. **Given** a test belongs to multiple scopes, **When** any of those scopes is requested, **Then** the test is included in the execution.

---

### User Story 3 - Run GPU/Integration Tests Separately (Priority: P2)

As a developer or CI system, I want to run GPU-dependent and long-running integration tests separately from fast unit tests so that I can choose when to run comprehensive validation without blocking rapid iteration.

**Why this priority**: GPU tests require specific hardware and take longer. Separating them allows developers to run fast tests locally while CI handles comprehensive testing.

**Independent Test**: Can be tested by running `ctest --preset IntegrationTests` and verifying it includes GPU-dependent tests while the unit test preset excludes them.

**Acceptance Scenarios**:

1. **Given** the integration test suite, **When** executed with GPU available, **Then** all GPU-dependent tests run and report results.
2. **Given** the integration test suite, **When** executed without GPU available, **Then** GPU-dependent tests are skipped with clear messages, not failed.
3. **Given** a test that requires OpenCL context, **When** the unit test suite runs, **Then** this test is not included (it's in integration tests).

---

### User Story 4 - Run API Tests for External Interface Validation (Priority: P3)

As a developer or QA engineer, I want to run API-level tests that validate the external interface (GladiusLib API, MCP protocol) so that I can ensure backwards compatibility and correct behavior from a consumer's perspective.

**Why this priority**: API tests catch integration issues and ensure external consumers aren't broken by internal changes. Lower priority as they're typically run in CI rather than during active development.

**Independent Test**: Can be tested by running `ctest --preset ApiTests` and verifying tests exercise external-facing APIs.

**Acceptance Scenarios**:

1. **Given** the API test suite, **When** executed, **Then** tests validate the GladiusLib component API and MCP protocol interfaces.
2. **Given** an API change that breaks compatibility, **When** API tests run, **Then** the breaking change is detected and reported.

---

### User Story 5 - CI/CD Runs Complete Test Suite Efficiently (Priority: P3)

As a CI/CD pipeline, I want to run all test categories in an optimal order so that fast-failing tests provide early feedback while comprehensive tests run in parallel where possible.

**Why this priority**: Efficient CI reduces feedback time and resource costs. Early failure detection saves compute resources.

**Independent Test**: Can be tested by running the full CI test workflow and measuring total time vs. running all tests sequentially.

**Acceptance Scenarios**:

1. **Given** the CI pipeline, **When** tests are executed, **Then** unit tests run first and fail-fast before integration tests begin.
2. **Given** multiple test categories, **When** running full test suite, **Then** independent test categories can run in parallel on separate CI nodes.

---

### Edge Cases

- What happens when a test file contains both fast unit tests and slow integration tests?
  - Tests should be split into separate files, or individual tests should be tagged appropriately.
- What happens when GPU becomes available mid-test-run?
  - Tests determine GPU availability at test startup; mid-run changes don't affect already-skipped tests.
- What happens when a test requires external tools (like admesh) that may not be installed?
  - Such tests belong in integration tests and skip gracefully with clear messages when tools are unavailable.
- What happens when developers want to run "everything except GPU tests"?
  - Support negative filtering (e.g., `ctest -L unit -LE gpu` to run unit tests excluding gpu-tagged tests).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support three test categories: `unit`, `integration`, and `api`.
- **FR-002**: System MUST provide CTest presets for each test category (`UnitTests`, `IntegrationTests`, `ApiTests`, `AllTests`).
- **FR-003**: Unit tests MUST complete in under 60 seconds total on a standard development machine without GPU.
- **FR-004**: Unit tests MUST NOT require GPU, OpenCL, network access, or external CLI tools.
- **FR-005**: Integration tests MUST include all GPU-dependent tests currently gated by `GLADIUS_RUN_GPU_TESTS`.
- **FR-006**: Integration tests MUST include all OpenCL-context-dependent tests.
- **FR-007**: Integration tests MUST include all tests requiring external tools (admesh, etc.).
- **FR-008**: API tests MUST cover GladiusLib component API and MCP protocol interfaces.
- **FR-009**: System MUST support component-based test scopes via CTest labels (e.g., `mesh-export`, `dual-contouring`, `mcp`, `io`, `opencl`).
- **FR-010**: Tests MUST be labeled with their category (`unit`, `integration`, `api`) and relevant component scopes.
- **FR-011**: CTest label filtering MUST work with both positive (`-L`) and negative (`-LE`) filters.
- **FR-012**: Tests that cannot run due to missing prerequisites MUST skip gracefully with descriptive messages, not fail.
- **FR-013**: Current integration test directory structure (`tests/integrationtests/`) MUST be preserved and enhanced.
- **FR-014**: Build system MUST support building and running each test category independently.
- **FR-015**: Documentation MUST be updated to describe the new test organization and how to run each category.

### Key Entities

- **Test Category**: Classification of tests by execution characteristics (unit, integration, api).
- **Test Scope/Label**: Component or feature area a test covers (mesh-export, dual-contouring, mcp, opencl, io).
- **Test Preset**: Pre-configured CTest execution profile combining filters, environment, and options.
- **Test Executable**: Compiled test binary (gladius_test, gladius_integrationtest, gladius_apitest).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Unit tests complete in under 60 seconds on a developer machine without GPU (currently tests take several minutes).
- **SC-002**: Developers can run component-specific tests with a single command targeting their area of work.
- **SC-003**: GPU-dependent tests are isolated such that running unit tests requires no GPU and produces no GPU-related skips or failures.
- **SC-004**: 100% of existing tests are categorized and labeled appropriately (no uncategorized tests remain).
- **SC-005**: CI pipeline can run unit tests and integration tests as separate stages, enabling fail-fast behavior.
- **SC-006**: Test category boundaries are documented such that developers know where to place new tests.
- **SC-007**: No test failures occur due to missing prerequisites when running the appropriate category (tests skip gracefully instead).

## Assumptions

- GTest/GMock framework continues to be used for all tests.
- CTest preset functionality (CMake 3.21+) is available and appropriate for organizing test execution.
- Current `GLADIUS_RUN_GPU_TESTS` environment variable pattern is acceptable and will continue to be used for GPU test gating within the integration test category.
- Existing test helper infrastructure (testhelper.cpp/h, opencl_test_helper.h) can be shared across test categories.
- The workspace tasks (`.vscode/tasks.json`) will be updated to reflect the new test organization.
