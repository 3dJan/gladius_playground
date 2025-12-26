# Gladius Project

## Project Overview

Gladius is a development tool and playground for the Volumetric Extension of the 3MF file format. It processes implicit geometries, particularly 3mf files with the volumetric extension and implicit namespace. The software can be used as both a library and a standalone application, featuring:

- A graphical programming interface for designing parts using Constructive Solid Geometry (CSG)
- A rendering engine for visualizing volumetric 3D models
- Import/export capabilities for 3mf files with volumetric extension
- API bindings for C#, C++, and Python

**Target Audience**: Developers working on 3D printing software, slicers, and implicit geometry processing applications.

## Tech Stack

### Core Technologies
- **Language**: C++11 and later
- **Build System**: CMake 3.12+
- **Package Manager**: vcpkg
- **Graphics**: OpenGL (for rendering)
- **Compute**: OpenCL 1.2+ (for GPU-accelerated computations)

### Key Dependencies
- **GUI**: ImGui (with docking-experimental, GLFW, OpenGL2 bindings)
- **3D Processing**: OpenVDB, NanoVDB, OpenMesh, lib3mf
- **Math**: Eigen3
- **Testing**: GTest/GMock
- **File Formats**: pugixml, lodepng, minizip, clipper2
- **Utilities**: fmt, nlohmann-json, tracy (profiling)

### Project Structure
```
gladius/
├── src/                  # Main application source code
├── library/              # Library API implementation
├── components/           # Third-party components and submodules
├── tests/
│   ├── unittests/       # Unit tests using GTest
│   └── integrationtests/# Integration tests
├── examples/             # Example applications and usage
├── documentation/        # Project documentation and images
├── cmake/               # CMake configuration files
└── vcpkg.json           # Dependency definitions
```

## Build and Test Instructions

### Prerequisites
1. Install OpenCL SDK for your platform (Intel/AMD/NVIDIA)
2. Install vcpkg package manager
3. Install CMake 3.12+
4. Set environment variables:
   - `VCPKG_ROOT` = Path to vcpkg installation
   - `VCPKG_DEFAULT_TRIPLET` = x64-windows or linux-x64

### Building
**Windows (Visual Studio Developer Prompt):**
```bash
cd gladius/gladius
mkdir build && cd build
cmake --preset x64-release -S ../
cmake --build .
```

**Linux:**
```bash
cd gladius/gladius
mkdir build && cd build
cmake --preset linux-release -S ../
cmake --build ../out/build/linux-release/
```

### Testing
- **Build Tests**: Use the "Build ALL" task
- **Run Tests**: Use the "Run Gladius Tests" task
- Tests are located in `tests/unittests/` and `tests/integrationtests/`
- OpenCL tests can be enabled/disabled via `ENABLE_OPENCL_TESTS` CMake option

# C++ Coding Guidelines

## General
- **Modern C++**: Use C++11 and later features. Use std::algorithm when possible.
- **Naming**: camelCase for variables/functions, PascalCase for classes/structs, UPPER_CASE for constants/macros.
- **Comments**: Use Doxygen-style comments.
- **Missing Information**: If information is missing, ask for clarification.
- **KISS Principle**: Keep it simple, stupid. Avoid unnecessary complexity.
- **DRY Principle**: Don't repeat yourself. Avoid code duplication.
- **YAGNI Principle**: You aren't gonna need it. Avoid adding features until they are necessary.

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
- **Documentation**: Only add comments that add additional value. Avoid stating the obvious.

## Code Examples

### Naming Conventions
```cpp
// Class (PascalCase)
class MeshRenderer {
private:
    int m_vertexCount;        // Private member (m_ prefix)
    static int s_instanceCount;  // Static member (s_ prefix)
    
public:
    // Function (lowerCamelCase, verb prefix)
    void renderMesh();
    bool isVisible() const;   // Boolean function (is prefix)
    
    // Getter/Setter
    int getVertexCount() const { return m_vertexCount; }
};

// Constants (UPPER_SNAKE_CASE)
constexpr int MAX_VERTICES = 10000;

// Namespace (lower_snake_case)
namespace gladius_core {
    // ...
}
```

### Memory Management
```cpp
// Prefer smart pointers over raw pointers
std::unique_ptr<Mesh> createMesh() {
    return std::make_unique<Mesh>();
}

// Use shared_ptr for shared ownership
std::shared_ptr<Texture> texture = std::make_shared<Texture>();
```

### East-side const
```cpp
// Prefer east-side const
int const* ptr;              // Pointer to const int
int* const ptr;              // Const pointer to int
int const* const ptr;        // Const pointer to const int

// Instead of west-side const
const int* ptr;              // Less preferred
```

### Error Handling
```cpp
// Use exceptions for error handling
void loadMesh(std::string const& filename) {
    if (!fileExists(filename)) {
        throw std::runtime_error("File not found: " + filename);
    }
    // Load mesh...
}
```

By following these guidelines, you can ensure clean, efficient, and maintainable C++ code.

## Additional Resources

For more information about writing effective Copilot instructions:
- [Best practices for using GitHub Copilot](https://docs.github.com/en/copilot/get-started/best-practices)
- [Best practices for Copilot coding agent](https://docs.github.com/en/copilot/tutorials/coding-agent/get-the-best-results)
- [5 tips for writing better custom instructions](https://github.blog/ai-and-ml/github-copilot/5-tips-for-writing-better-custom-instructions-for-copilot/)