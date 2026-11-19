# Developer Guide

## Welcome

This guide will help you get started with developing Gladius, whether you're fixing bugs, adding features, or integrating Gladius into your own applications.

## Prerequisites

### Required Tools

- **C++ Compiler**: MSVC 2019+ (Windows), GCC 9+ or Clang 10+ (Linux)
- **CMake**: Version 3.20 or higher
- **vcpkg**: For dependency management
- **Git**: For version control
- **OpenCL SDK**: From your GPU vendor (NVIDIA, AMD, or Intel)

### Optional Tools

- **Visual Studio Code**: Recommended IDE with C++ extensions
- **Visual Studio 2022**: Full IDE with integrated debugging
- **CLion**: JetBrains C++ IDE
- **Ninja**: Fast build system

## Getting Started

### 1. Clone the Repository

```bash
git clone https://github.com/3MFConsortium/gladius.git
cd gladius
git submodule update --init --recursive
```

### 2. Set Up vcpkg

#### Windows
```powershell
# Set environment variables
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows"

# Install vcpkg if not already installed
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

#### Linux
```bash
# Set environment variables
export VCPKG_ROOT=/path/to/vcpkg

# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
```

### 3. Build the Project

#### Using CMake Presets (Recommended)

**Windows:**
```bash
cd gladius
cmake --preset x64-debug
cmake --build --preset x64-debug
```

**Linux:**
```bash
cd gladius
cmake --preset linux-debug
cmake --build --preset linux-debug
```

#### Manual CMake Configuration

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

### 4. Run Tests

```bash
# Run all tests
ctest --output-on-failure

# Run specific test suite
./gladius_tests
```

## Project Structure

```
gladius/
├── gladius/                    # Main source directory
│   ├── src/                    # Source code
│   │   ├── api/               # API definitions (ACT)
│   │   ├── compute/           # Compute pipeline
│   │   ├── contour/           # Contour extraction
│   │   ├── io/                # Import/Export
│   │   ├── kernel/            # OpenCL kernels
│   │   ├── nodes/             # Node system
│   │   └── ui/                # User interface
│   ├── tests/                 # Test suites
│   │   ├── unittests/        # Unit tests
│   │   └── integrationtests/ # Integration tests
│   ├── library/               # Reusable libraries
│   ├── examples/              # Example code
│   └── documentation/         # Documentation (this!)
├── scripts/                   # Build and utility scripts
└── CMakeLists.txt            # Root CMake configuration
```

## Development Workflow

### Using Visual Studio Code

1. **Install Extensions:**
   - C/C++ (Microsoft)
   - CMake Tools
   - GitLens

2. **Open Project:**
   ```bash
   code .
   ```

3. **Configure CMake:**
   - Press `Ctrl+Shift+P`
   - Type "CMake: Configure"
   - Select appropriate preset

4. **Build:**
   - Press `F7` or use CMake Tools panel

5. **Debug:**
   - Set breakpoints
   - Press `F5` to start debugging

### Using Visual Studio

1. **Open Project:**
   - File → Open → CMake
   - Select `CMakeLists.txt`

2. **Select Configuration:**
   - Use configuration dropdown
   - Choose Debug or Release

3. **Build:**
   - Build → Build All (`Ctrl+Shift+B`)

4. **Run:**
   - Select gladius.exe as startup item
   - Press `F5` to debug

### Command Line Development

```bash
# Build in debug mode
cmake --preset x64-debug
cmake --build --preset x64-debug

# Run application
./out/build/x64-debug/gladius

# Run tests
cd out/build/x64-debug
ctest --output-on-failure

# Build in release mode
cmake --preset x64-release
cmake --build --preset x64-release
```

## Coding Guidelines

### C++ Style

Follow the project's C++ coding guidelines (see project README):

```cpp
// Class names: PascalCase
class MyClassName {
public:
    // Method names: camelCase
    void myMethodName();
    
    // Boolean methods: is/has/are prefix
    bool isValid() const;
    bool hasConnection() const;
    
private:
    // Member variables: m_ prefix, camelCase
    int m_memberVariable;
    
    // Static members: s_ prefix
    static int s_staticMember;
};

// Constants: UPPER_SNAKE_CASE
const int MAX_ITERATIONS = 100;

// Namespaces: lower_snake_case
namespace gladius::nodes {
    // ...
}
```

### Formatting

Use `.clang-format` for consistent formatting:

```bash
# Format all changed files
git diff --name-only | grep -E '\.(cpp|h)$' | xargs clang-format -i

# Format specific file
clang-format -i src/MyFile.cpp
```

### Documentation

Use Doxygen-style comments for public APIs:

```cpp
/**
 * @brief Evaluates the signed distance field at a given point
 * 
 * @param point The 3D point in world space
 * @param context The evaluation context with parameters
 * @return The signed distance value (negative inside, positive outside)
 * 
 * @throws ComputeException if evaluation fails
 */
float evaluate(const Vector3f& point, const Context& context);
```

## Adding New Features

### Adding a New Node Type

1. **Define the Node Class:**

```cpp
// In src/nodes/DerivedNodes.h
class MyCustomNode : public ClonableNode<MyCustomNode> {
public:
    MyCustomNode() 
        : ClonableNode("MyCustomNode", {}, Category::Function) 
    {
        // Define inputs
        addInput("input", PortType::Scalar);
        
        // Define outputs
        addOutput("output", PortType::Scalar);
        
        // Define parameters
        addParameter("factor", 2.0f);
    }
    
    std::string getDescription() const override {
        return "Multiplies input by factor";
    }
};
```

2. **Implement OpenCL Code Generation:**

```cpp
// In ToOCLVisitor.cpp
void ToOCLVisitor::visit(MyCustomNode& node) {
    float factor = node.getParameter("factor").get<float>();
    
    std::string code = fmt::format(
        "float {} = {} * {};",
        outputVar,
        inputVar,
        factor
    );
    
    m_code += code;
}
```

3. **Register the Node:**

```cpp
// In nodes factory
NodeBase* createNodeFromName(const std::string& name, Model& model) {
    if (name == "MyCustomNode") {
        return new MyCustomNode();
    }
    // ... other nodes
}
```

4. **Add Tests:**

```cpp
// In tests/unittests/MyCustomNode_tests.cpp
TEST(MyCustomNode_Tests, BasicMultiplication) {
    nodes::Model model;
    auto node = std::make_unique<MyCustomNode>();
    
    node->setParameter("factor", 3.0f);
    
    // Test evaluation
    // ...
    
    ASSERT_FLOAT_EQ(result, expected);
}
```

### Adding a New Export Format

1. **Create Exporter Class:**

```cpp
// In src/io/MyFormatExporter.h
class MyFormatExporter : public IExporter {
public:
    MyFormatExporter(const std::filesystem::path& filename);
    
    void exportModel(const nodes::Assembly& assembly) override;
    
private:
    void writeHeader();
    void writeGeometry();
    void writeFooter();
    
    std::filesystem::path m_filename;
};
```

2. **Implement Export Logic:**

```cpp
// In src/io/MyFormatExporter.cpp
void MyFormatExporter::exportModel(const nodes::Assembly& assembly) {
    std::ofstream file(m_filename);
    
    writeHeader();
    
    // Convert model to format
    auto& model = assembly.getModel();
    // ... export logic
    
    writeFooter();
}
```

3. **Register in Export Dialog:**

```cpp
// In ui/ExportDialog.cpp
void ExportDialog::show() {
    if (ImGui::MenuItem("My Format (.myf)")) {
        exportToMyFormat();
    }
}
```

### Adding an OpenCL Kernel

1. **Create Kernel File:**

```c
// In src/kernel/my_kernel.cl
__kernel void my_kernel(
    __global float* input,
    __global float* output,
    const int count
) {
    int gid = get_global_id(0);
    if (gid < count) {
        output[gid] = input[gid] * 2.0f;
    }
}
```

2. **Create Program Wrapper:**

```cpp
// In src/MyProgram.h
class MyProgram : public CLProgram {
public:
    MyProgram(SharedComputeContext context);
    
    void execute(cl::Buffer& input, cl::Buffer& output, int count);
    
private:
    cl::Kernel m_kernel;
};
```

3. **Implement Execution:**

```cpp
// In src/MyProgram.cpp
void MyProgram::execute(cl::Buffer& input, cl::Buffer& output, int count) {
    m_kernel.setArg(0, input);
    m_kernel.setArg(1, output);
    m_kernel.setArg(2, count);
    
    auto& queue = m_context->getQueue();
    queue.enqueueNDRangeKernel(
        m_kernel,
        cl::NullRange,
        cl::NDRange(count),
        cl::NullRange
    );
}
```

## Debugging

### Debugging C++ Code

**Visual Studio / VS Code:**
1. Set breakpoints by clicking in the margin
2. Press F5 to start debugging
3. Use Debug Console for expressions

**GDB (Linux):**
```bash
gdb ./gladius
(gdb) break Application.cpp:42
(gdb) run input.3mf
(gdb) print variableName
(gdb) continue
```

### Debugging OpenCL Kernels

1. **Use CPU Device:**
   ```cpp
   // Force CPU execution for debugging
   ComputeContext context;
   context.selectDevice(DeviceType::CPU);
   ```

2. **Add Printf Debugging:**
   ```c
   __kernel void debug_kernel(...) {
       int gid = get_global_id(0);
       printf("Thread %d: value = %f\n", gid, someValue);
   }
   ```

3. **Validate Results on CPU:**
   ```cpp
   // Compare GPU results with CPU reference
   std::vector<float> gpuResult = /* ... */;
   std::vector<float> cpuResult = computeReferenceCPU();
   
   for (size_t i = 0; i < gpuResult.size(); i++) {
       ASSERT_NEAR(gpuResult[i], cpuResult[i], 1e-5);
   }
   ```

### Memory Debugging

**Valgrind (Linux):**
```bash
valgrind --leak-check=full ./gladius
```

**Visual Studio Memory Profiler:**
1. Debug → Performance Profiler
2. Select "Memory Usage"
3. Start profiling

## Testing

### Writing Unit Tests

Use GTest framework:

```cpp
#include <gtest/gtest.h>

namespace gladius::tests {

TEST(ComponentName_Tests, MethodName_Condition_ExpectedBehavior) {
    // Arrange
    ComponentName component;
    component.setup();
    
    // Act
    auto result = component.methodName();
    
    // Assert
    ASSERT_EQ(result, expectedValue);
}

TEST(ComponentName_Tests, MethodName_WithNullInput_ThrowsException) {
    ComponentName component;
    
    ASSERT_THROW(component.methodName(nullptr), std::invalid_argument);
}

} // namespace gladius::tests
```

### Running Tests

```bash
# Run all tests
ctest

# Run with verbose output
ctest --output-on-failure

# Run specific test
./gladius_tests --gtest_filter=NodeSystem_Tests.*

# Run with debugging
gdb ./gladius_tests
```

### Integration Tests

```cpp
TEST(Integration_Tests, LoadAndExport_CompleteWorkflow) {
    // Load model
    Importer3mf importer("test_data/model.3mf");
    auto assembly = importer.import();
    
    // Process
    auto& model = assembly->getModel();
    ASSERT_TRUE(model.isValid());
    
    // Export
    Writer3mf writer("output.3mf");
    writer.write(*assembly);
    
    // Verify
    ASSERT_TRUE(std::filesystem::exists("output.3mf"));
}
```

## Performance Profiling

### Using Built-in Profiling

```cpp
#include "Profiling.h"

void myFunction() {
    PROFILE_SCOPE("MyFunction");
    
    // Code to profile
    
    PROFILE_SCOPE("SubSection");
    // More code
}
```

### External Profilers

**Visual Studio Profiler:**
1. Debug → Performance Profiler
2. Select "CPU Usage"
3. Start profiling

**perf (Linux):**
```bash
perf record ./gladius
perf report
```

**Intel VTune:**
```bash
vtune -collect hotspots ./gladius
vtune -report hotspots
```

## Common Issues

### Build Issues

**Issue:** vcpkg dependencies fail to build
**Solution:** 
```bash
# Clear vcpkg cache
rm -rf $VCPKG_ROOT/buildtrees
rm -rf $VCPKG_ROOT/packages

# Rebuild
vcpkg install
```

**Issue:** OpenCL not found
**Solution:** Install OpenCL SDK for your platform and set environment variables

**Issue:** CMake cache issues
**Solution:**
```bash
rm -rf build/
rm CMakeCache.txt
cmake --preset x64-debug
```

### Runtime Issues

**Issue:** "Device not found" error
**Solution:** 
- Install GPU drivers with OpenCL support
- Install CPU OpenCL runtime as fallback

**Issue:** Kernel compilation fails
**Solution:**
- Check kernel syntax
- Enable build log output
- Test on CPU device first

**Issue:** Out of memory errors
**Solution:**
- Reduce image resolution
- Use smaller models
- Monitor GPU memory usage

## Contributing

### Pull Request Process

1. **Create a Branch:**
   ```bash
   git checkout -b feature/my-new-feature
   ```

2. **Make Changes:**
   - Follow coding guidelines
   - Add tests
   - Update documentation

3. **Test:**
   ```bash
   cmake --build . --target all
   ctest --output-on-failure
   ```

4. **Commit:**
   ```bash
   git add .
   git commit -m "Add my new feature"
   ```

5. **Push:**
   ```bash
   git push origin feature/my-new-feature
   ```

6. **Create Pull Request:**
   - Go to GitHub
   - Create PR with clear description
   - Link related issues

### Code Review

- Address all review comments
- Keep commits focused and atomic
- Squash commits if requested
- Ensure CI passes

## Resources

### Documentation
- [Architecture Overview](ARCHITECTURE.md)
- [Node System](NODE_SYSTEM.md)
- [Compute Pipeline](COMPUTE_PIPELINE.md)
- [API Reference](API_REFERENCE.md)

### External Resources
- [OpenCL Documentation](https://www.khronos.org/opencl/)
- [3MF Specification](https://3mf.io/specification/)
- [ImGUI Documentation](https://github.com/ocornut/imgui)
- [CMake Documentation](https://cmake.org/documentation/)

### Community
- GitHub Issues for bug reports
- GitHub Discussions for questions
- Pull Requests for contributions

## Next Steps

1. Build the project successfully
2. Run the test suite
3. Explore the example code
4. Try adding a simple feature
5. Read the architecture documentation
6. Join the community discussions

Happy coding! 🚀
