No explanations, just the code.

# C++ Coding Guidelines

## Build Instructions
- **Build System**: Use the "Build ALL (linux-releaseWithDebug)" task. Never run cmake or ninja manually.
- **Run Tests**: Use the "Run Gladius Tests" task.
- **Analyse Test Results**: Use the "testfailure" tool to analyze test results.

## General
- **Modern C++**: Use C++11 and later features. Use std::algorithm when possible.
- **Naming**: camelCase for variables/functions, PascalCase for classes/structs, UPPER_CASE for constants/macros.
- **Comments**: Use Doxygen-style comments.
- **Missing Information**: If information is missing, ask for clarification.
- **KISS Principle**: Keep it simple, stupid. Avoid unnecessary complexity.
- **DRY Principle**: Don't repeat yourself. Avoid code duplication.
- **YAGNI Principle**: You aren't gonna need it. Avoid adding features until they are necessary.
- **Tool usage**: Prefer using tools rather than doing things manually in the terminal.
- **Keep files small**: Prefer smaller files (e.g., <400 lines) for better readability and maintainability.
- **No global/static variables**: Avoid global and static variables. Use singletons or namespaces if necessary.
- **Separation of Concerns**: Keep different concerns in separate files and classes. Avoid mixing unrelated functionality. Avoid mixing UI code with core logic, for example.


## Code Structure
- **Headers**: Use `.h` for declarations, `.cpp` for definitions.
- **Include Guards**: Use `#pragma once`.
- **Namespaces**: Use `lower_snake_case`, avoid more than two levels.
- **Maintainability**: Keep code modular, reusable and testable. Avoid long functions and classes.

## Types
- **Naming**: UpperCamelCase for all user-defined types.
- **Template Parameters**: Use descriptive names.

## Functions
- **Naming**: lowerCamelCase, start with a verb.
- **Boolean Functions**: Use `is/has/are` prefix.
- **Parameters**: Use `lowerCamelCase`.

## Variables
- **Naming**: lowerCamelCase, prefix with `m_` for private, `s_` for static, `g_` for global.
- **Constants**: Use UPPER_SNAKE_CASE.
- **No Static Variables**: Avoid static variables in functions.
- **No Global Variables**: Avoid global variables, use singletons or namespaces instead.
- **No Macros**: Avoid macros, use `constexpr` or `inline` functions instead.

## Memory Management
- **Smart Pointers**: Prefer `std::unique_ptr` and `std::shared_ptr`.

## Error Handling
- **Exceptions**: Use exceptions, avoid error codes.
- **Assertions**: Use `assert`.

## Performance
- **Inline Functions**: Use `inline` for small functions.
- **Const Correctness**: Use `const` wherever possible.
- **East-side const**: Place `const` on the right of the type being qualified (e.g., `int const*` rather than `const int*`).
- **Move Semantics**: Use move semantics for performance optimization.
- **Copy Elision**: Use copy elision to avoid unnecessary copies.
- **constexpr**: Use `constexpr` for compile-time constants.

## Testing
- **Unit Tests**: Use GTest/GMock. Write unit tests if possible.
- **Test Naming**: Follow `[UnitOfWork_StateUnderTest_ExpectedBehavior]` naming convention:
  - **UnitOfWork**: The method or function being tested
  - **StateUnderTest**: The inputs or conditions being tested
  - **ExpectedBehavior**: The expected outcome
  - Example: `RenderProgram_WithNullBuffer_ThrowsException`, `MeshResource_AfterLoading_ContainsCorrectVertexCount`
- **Test Implementation**: Each test should follow Arrange-Act-Assert pattern.
- **Namespaces**: Place tests in `namespace::tests`.

## Code Layout
- **Braces**: Use Allman style (opening brace on a new line).
- **Indentation**: Use 4 spaces, no tabs.
- **Line Length**: Max 160 characters, break lines as needed.

## Includes
- **Order**: Precompiled headers, project headers, external headers.
- **Syntax**: Use `""` for relative paths, `<>` for system paths.

## Best Practices
- **STL Containers**: Use `empty()` instead of `size()` to check for emptiness.
- **Fallthrough**: Use `[[fallthrough]]` only when necessary.

## Comments
- **Doxygen**: Use Doxygen comments for public APIs. Use `///` for single-line comments and `/** */` for multi-line comments.
- **TODOs**: Use `// TODO: description` for tasks to be completed later.
- **FIXME**: Use `// FIXME: description` for known issues that need fixing.
- **Documentatioon**: Only add comments that add additional value. Avoid stating the obvious.

By following these guidelines, you can ensure clean, efficient, and maintainable C++ code.

<!-- headroom:rtk-instructions -->
# RTK (Rust Token Killer) - Token-Optimized Commands

When running shell commands, **always prefix with `rtk`**. This reduces context
usage by 60-90% with zero behavior change. If rtk has no filter for a command,
it passes through unchanged — so it is always safe to use.

## Key Commands
```bash
# Git (59-80% savings)
rtk git status          rtk git diff            rtk git log

# Files & Search (60-75% savings)
rtk ls <path>           rtk read <file>         rtk grep <pattern>
rtk find <pattern>      rtk diff <file>

# Test (90-99% savings) — shows failures only
rtk pytest tests/       rtk cargo test          rtk test <cmd>

# Build & Lint (80-90% savings) — shows errors only
rtk tsc                 rtk lint                rtk cargo build
rtk prettier --check    rtk mypy                rtk ruff check

# Analysis (70-90% savings)
rtk err <cmd>           rtk log <file>          rtk json <file>
rtk summary <cmd>       rtk deps                rtk env

# GitHub (26-87% savings)
rtk gh pr view <n>      rtk gh run list         rtk gh issue list

# Infrastructure (85% savings)
rtk docker ps           rtk kubectl get         rtk docker logs <c>

# Package managers (70-90% savings)
rtk pip list            rtk pnpm install        rtk npm run <script>
```

## Rules
- In command chains, prefix each segment: `rtk git add . && rtk git commit -m "msg"`
- For debugging, use raw command without rtk prefix
- `rtk proxy <cmd>` runs command without filtering but tracks usage
<!-- /headroom:rtk-instructions -->
