<!--
=== SYNC IMPACT REPORT ===
Version change: 1.1.0 → 1.2.0
Modified principles: None
Added sections: Principle VI (UI Responsiveness)
Removed sections: None
Templates requiring updates:
  - plan-template.md: ✅ Compatible (Constitution Check section exists)
  - spec-template.md: ✅ Compatible
  - tasks-template.md: ✅ Compatible
Follow-up TODOs: None
===========================
-->

# Gladius Constitution

## Project Overview

Gladius is a **C++20 development tool and library** for processing **implicit geometries** using the
3MF Volumetric Extension. It serves as a reference implementation for the 3MF Consortium's
volumetric specification.

For comprehensive technical details, see:
[Gladius & 3MF Volumetric Extension Technical Overview](gladius-3mf-volumetric-overview.md)

### Key Domain Concepts

**Implicit Surfaces / Signed Distance Fields (SDF)**:
- Geometry is defined by mathematical functions evaluated at any 3D point
- Negative values = inside, zero = surface, positive = outside
- Boolean operations: union = `min()`, intersection = `max()`, subtraction = `max(a, -b)`

**3MF Volumetric Extension**:
- Extends 3MF with volumetric properties (spatially varying color, materials)
- `<levelset>` defines object shape via implicit function (iso-surface at 0)
- `<functionfromimage3d>` samples from voxel data (PNG image stacks)

**3MF Implicit Extension**:
- Node-based mathematical function graphs within 3MF files
- `<implicitfunction>` contains computation nodes connected via references
- Supports ~50 native node types (arithmetic, trigonometric, geometry, etc.)

## Core Principles

### I. Modern C++ Standards

All code MUST adhere to C++20 standards and modern C++ idioms.

- Use STL containers and algorithms (`std::algorithm`) over manual implementations
- Smart pointers (`std::unique_ptr`, `std::shared_ptr`) MUST be used for memory management
- Use `constexpr` for compile-time constants; avoid macros
- Apply move semantics for performance optimization
- Use `const` wherever possible with east-side const style (`int const*` not `const int*`)
- Exceptions MUST be used for error handling; error codes are prohibited
- Use `assert` for assertions and invariant checking

**Rationale**: Modern C++ ensures type safety, memory safety, and maintainability while enabling
compiler optimizations and reducing common bugs.

### II. Test-First Development

Every feature MUST have corresponding unit tests using GTest/GMock.

- Tests SHOULD be written before or alongside implementation (Red-Green-Refactor)
- Test naming convention: `[UnitOfWork_StateUnderTest_ExpectedBehavior]`
  - Example: `RenderProgram_WithNullBuffer_ThrowsException`
- Tests MUST follow the Arrange-Act-Assert pattern
- All tests MUST be placed in `namespace::tests`
- GPU-dependent tests MUST be gated by `GLADIUS_RUN_GPU_TESTS=1` environment variable
- Use `GTEST_SKIP()` with clear messages when prerequisites are missing

**Rationale**: Comprehensive test coverage prevents regressions, documents expected behavior,
and enables safe refactoring of compute-intensive OpenCL/GPU code.

### III. Simplicity First (KISS, DRY, YAGNI)

Code MUST be simple, non-repetitive, and minimal.

- KISS: Prefer straightforward solutions over clever abstractions
- DRY: Extract common patterns into reusable functions/classes
- YAGNI: Do not implement features until they are explicitly needed
- Keep files small (target <400 lines) for readability and maintainability
- Long functions MUST be decomposed into smaller, testable units
- Avoid unnecessary complexity in class hierarchies

**Rationale**: Simple code is easier to understand, test, debug, and maintain—critical for a
compute-heavy codebase with GPU/OpenCL kernels.

### IV. Consistent Code Style

All code MUST follow the established formatting and naming conventions.

**Formatting**:
- Allman brace style (opening brace on new line)
- 4-space indentation, no tabs
- Maximum 160 characters per line
- `#pragma once` for include guards
- Include order: precompiled headers → project headers → external headers

**Naming**:
- `camelCase` for variables and functions (start functions with a verb)
- `PascalCase` for classes, structs, enums, and type aliases
- `UPPER_SNAKE_CASE` for constants and macros
- `m_` prefix for private members, `s_` for static, `g_` for global
- Boolean functions MUST use `is/has/are` prefix
- Namespaces MUST use `lower_snake_case` (max two levels)

**Rationale**: Consistent style reduces cognitive load during code review and maintenance,
enabling developers to focus on logic rather than formatting.

### V. Documentation and Comments

Public APIs MUST be documented with Doxygen-style comments.

- Use `///` for single-line and `/** */` for multi-line documentation
- Comments MUST add value beyond what the code already expresses
- Use `// TODO: description` for planned improvements
- Use `// FIXME: description` for known issues requiring attention
- Avoid stating the obvious in comments

**Rationale**: Good documentation accelerates onboarding and reduces support burden while
avoiding the maintenance cost of redundant or stale comments.

### VI. UI Responsiveness

The user interface MUST remain responsive at all times. Expensive operations MUST NOT block the main thread.

- Long-running tasks (file I/O, mesh generation, OpenCL operations) MUST execute asynchronously
- Use `std::async`, `std::future`, or dedicated worker threads for background operations
- Progress feedback MUST be provided for operations exceeding ~100ms
- Cancellation support SHOULD be implemented for long-running operations
- Shared state between threads MUST be protected with appropriate synchronization primitives
- Prefer message-passing and immutable data over shared mutable state
- Avoid blocking calls (`wait()`, `get()`) on the main/UI thread without timeout or progress handling
- Race conditions MUST be prevented through careful synchronization design, not added as fixes
- Use RAII for lock management (`std::lock_guard`, `std::unique_lock`)

**Rationale**: A responsive UI is critical for user experience. Blocking the main thread leads to
application freezes and poor perceived performance. Async operations introduce concurrency hazards;
disciplined synchronization prevents race conditions and deadlocks that are notoriously difficult to debug.

## Technology Stack

Gladius maintains a specific technology stack for consistency and performance.

**Build System**:
- CMake with presets (`CMakePresets.json`)
- vcpkg for dependency management (manifest mode)
- Ninja for fast builds
- Build via VS Code tasks (prefer "Build ALL (linux-releaseWithDebug)")
- NEVER run cmake or ninja manually in terminal

**Runtime Dependencies**:
- OpenCL 1.2+ for GPU compute
- OpenGL for rendering
- ImGui for user interface
- lib3mf for 3MF file handling

**Compiler Requirements**:
- Clang (Linux default: `/usr/bin/clang`, `/usr/bin/clang++`)
- MSVC (Windows)

**File Organization**:
- `.h` for declarations, `.cpp` for definitions
- Headers use `""` for relative paths, `<>` for system/external headers

**Key Directories**:
- `gladius/src/nodes/` — Node/graph system for implicit functions
- `gladius/src/io/3mf/` — 3MF import/export with volumetric support
- `gladius/src/compute/` — OpenCL program management and GPU kernels
- `gladius/src/kernel/` — OpenCL kernel source files

## Development Workflow

### Building

1. Use VS Code task "Build ALL (linux-releaseWithDebug)"
2. Never invoke cmake or ninja directly in terminal
3. Use presets defined in `gladius/CMakePresets.json`

### Testing

1. Use VS Code task "Run Gladius Tests (linux-releaseWithDebug)"
2. Use `test_failure` tool to analyze test results
3. For GPU tests, set `GLADIUS_RUN_GPU_TESTS=1`
4. Debug flags: `GLADIUS_DEBUG_MDC_CONFIG=1` for MDC config logging

### Code Review Checklist

- [ ] Follows Modern C++ Standards (Principle I)
- [ ] Has unit tests with proper naming (Principle II)
- [ ] No unnecessary complexity (Principle III)
- [ ] Follows code style conventions (Principle IV)
- [ ] Public APIs are documented (Principle V)
- [ ] UI remains responsive; async operations are thread-safe (Principle VI)
- [ ] Files remain under 400 lines
- [ ] No static variables in functions
- [ ] No global variables (use singletons or namespaces)

## Governance

This constitution supersedes all other development practices in Gladius.

**Amendment Process**:
1. Propose changes with rationale in a pull request
2. Amendments require maintainer approval
3. Major changes (removing/redefining principles) require migration plan
4. Version updates follow semantic versioning:
   - MAJOR: Backward-incompatible governance changes
   - MINOR: New principles or materially expanded guidance
   - PATCH: Clarifications, wording, typo fixes

**Compliance**:
- All PRs MUST be verified against this constitution
- Complexity violations MUST be justified in PR description
- Use `docs/developer_onboarding.md` for runtime development guidance

**Version**: 1.2.0 | **Ratified**: 2025-12-29 | **Last Amended**: 2026-01-26

## Reference Documents

For detailed technical information, see these companion documents:

| Document | Description |
|----------|-------------|
| [Gladius & 3MF Volumetric Overview](gladius-3mf-volumetric-overview.md) | Comprehensive guide to 3MF volumetric/implicit extensions and Gladius implementation |
| [Developer Onboarding](../../docs/developer_onboarding.md) | Practical guide for setting up development environment |
| [Graph to OpenCL Architecture](../../docs/architecture/graph_to_opencl.md) | How function graphs compile to GPU kernels |
