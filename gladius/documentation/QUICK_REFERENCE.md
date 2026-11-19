# Quick Reference Guide

A handy reference for common tasks, APIs, and patterns in Gladius.

## 📌 Build Commands

### Windows
```bash
# Configure
cmake --preset x64-debug
cmake --preset x64-release

# Build
cmake --build --preset x64-debug
cmake --build --preset x64-release

# Test
cd out/build/x64-debug
ctest --output-on-failure
```

### Linux
```bash
# Configure
cmake --preset linux-debug
cmake --preset linux-release

# Build
cmake --build out/build/linux-debug
cmake --build out/build/linux-release

# Test
cd out/build/linux-debug
ctest --output-on-failure
```

## 🔧 Common Development Tasks

### Running the Application
```bash
# Debug build
./out/build/x64-debug/gladius

# With a file
./out/build/x64-debug/gladius model.3mf

# Release build
./out/build/x64-release/gladius
```

### Formatting Code
```bash
# Format all changed files
git diff --name-only | grep -E '\.(cpp|h)$' | xargs clang-format -i

# Format specific file
clang-format -i src/MyFile.cpp
```

### Running Specific Tests
```bash
# Run all tests
./gladius_tests

# Run specific test suite
./gladius_tests --gtest_filter=NodeSystem_Tests.*

# Run single test
./gladius_tests --gtest_filter=NodeSystem_Tests.CreateBasicGraph
```

## 🎯 Common Coding Patterns

### Creating a Node Graph
```cpp
#include <nodes/Model.h>
#include <nodes/DerivedNodes.h>

nodes::Model model;
model.createBeginEnd();

// Add a sphere
auto sphereId = model.addNode(std::make_unique<nodes::Sphere>());
auto sphereNode = model.getNode(sphereId);
sphereNode->setParameter("radius", 1.0f);

// Add a box
auto boxId = model.addNode(std::make_unique<nodes::Box>());

// Add union operation
auto unionId = model.addNode(std::make_unique<nodes::Union>());

// Connect nodes
auto beginId = model.getBeginNode();
auto endId = model.getEndNode();

model.connect(beginId, "cs", sphereId, "cs");
model.connect(sphereId, "distance", unionId, "a");
model.connect(boxId, "distance", unionId, "b");
model.connect(unionId, "result", endId, "input");
```

### Loading and Rendering a Model
```cpp
#include <io/3mf/Importer3mf.h>
#include <compute/ComputeCore.h>

// Load model
Importer3mf importer("model.3mf");
auto assembly = importer.import();

// Set up compute
auto computeContext = std::make_shared<ComputeContext>();
auto resourceContext = std::make_shared<ResourceContext>(computeContext);
ComputeCore computeCore(computeContext, resourceContext);

// Update with model
computeCore.updateModel(assembly->getModel());

// Render
Camera camera;
ImageBuffer output(1920, 1080);
computeCore.render(camera, output);
```

### Generating Slices
```cpp
#include <compute/ComputeCore.h>
#include <BitmapLayer.h>

// Generate single slice
float zHeight = 5.0f;
int resolution = 2048;
BitmapLayer slice;
computeCore.generateSlice(zHeight, resolution, slice);

// Extract contours
ContourExtractor extractor;
auto contours = extractor.extract(slice);

// Process contours
for (const auto& contour : contours) {
    for (const auto& point : contour.points) {
        // Process point
    }
}
```

### Exporting to Different Formats
```cpp
#include <io/Writer3mf.h>
#include <io/MeshExporter.h>
#include <SvgWriter.h>

// Export as 3MF
Writer3mf writer3mf("output.3mf");
writer3mf.write(*assembly);

// Export as STL mesh
MeshExporter meshExporter("output.stl");
meshExporter.exportMesh(*assembly);

// Export contours as SVG
SvgWriter svgWriter("output.svg");
for (float z = 0; z < 10.0f; z += 0.1f) {
    auto slice = computeCore.generateSlice(z, 2048);
    svgWriter.addSlice(slice, z);
}
svgWriter.write();
```

### Using the API (C++)
```cpp
#include <GladiusLib.hpp>

// Create Gladius instance
auto gladius = GladiusLib::CreateGladius();

// Load model
auto model = gladius->LoadModel("input.3mf");

// Generate slice
auto slice = model->GenerateSlice(5.0f, 2048);

// Process contours
for (uint32_t i = 0; i < slice->GetContourCount(); i++) {
    auto contour = slice->GetContour(i);
    for (uint32_t j = 0; j < contour->GetPointCount(); j++) {
        auto point = contour->GetPoint(j);
        // Process point
    }
}

// Export
model->ExportMesh("output.stl");
```

## 🔍 Useful Debugging Commands

### GDB (Linux)
```bash
# Start debugging
gdb ./gladius

# Set breakpoint
(gdb) break Application.cpp:42
(gdb) break ComputeCore::render

# Run with arguments
(gdb) run model.3mf

# Print variable
(gdb) print variableName
(gdb) print *this

# Continue execution
(gdb) continue
(gdb) next
(gdb) step

# Backtrace
(gdb) backtrace
(gdb) frame 2
```

### LLDB (macOS/Linux)
```bash
# Start debugging
lldb ./gladius

# Set breakpoint
(lldb) breakpoint set --file Application.cpp --line 42
(lldb) breakpoint set --name ComputeCore::render

# Run with arguments
(lldb) run model.3mf

# Print variable
(lldb) print variableName
(lldb) expr this

# Continue execution
(lldb) continue
(lldb) next
(lldb) step
```

### Visual Studio Debugger
```
F9          - Toggle breakpoint
F5          - Start debugging
F10         - Step over
F11         - Step into
Shift+F11   - Step out
F5          - Continue
Shift+F5    - Stop debugging
```

## 📊 Common OpenCL Patterns

### Creating a Kernel
```cpp
#include <CLProgram.h>

class MyProgram : public CLProgram {
public:
    MyProgram(SharedComputeContext context) 
        : CLProgram(context) {
        // Load kernel source
        std::string source = loadKernelSource("my_kernel.cl");
        
        // Compile
        compile(source);
        
        // Get kernel
        m_kernel = cl::Kernel(m_program, "my_kernel");
    }
    
    void execute(cl::Buffer& input, cl::Buffer& output, int count) {
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
    
private:
    cl::Kernel m_kernel;
};
```

### Buffer Operations
```cpp
// Create buffer
cl::Buffer buffer = context->createBuffer(
    size * sizeof(float),
    CL_MEM_READ_WRITE
);

// Write to buffer
std::vector<float> data = {1.0f, 2.0f, 3.0f};
queue.enqueueWriteBuffer(buffer, CL_TRUE, 0, 
                        data.size() * sizeof(float), 
                        data.data());

// Read from buffer
std::vector<float> result(size);
queue.enqueueReadBuffer(buffer, CL_TRUE, 0,
                       result.size() * sizeof(float),
                       result.data());

// Copy buffer
cl::Buffer buffer2 = context->createBuffer(size * sizeof(float));
queue.enqueueCopyBuffer(buffer, buffer2, 0, 0, size * sizeof(float));
```

## 🧪 Testing Patterns

### Basic Unit Test
```cpp
#include <gtest/gtest.h>

TEST(ComponentName_Tests, MethodName_Condition_ExpectedBehavior) {
    // Arrange
    ComponentName component;
    component.setup();
    
    // Act
    auto result = component.methodName();
    
    // Assert
    ASSERT_EQ(result, expectedValue);
}
```

### Testing with Fixtures
```cpp
class MyTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code
        m_model.createBeginEnd();
    }
    
    void TearDown() override {
        // Cleanup code
    }
    
    nodes::Model m_model;
};

TEST_F(MyTestFixture, TestSomething) {
    // Test uses m_model from fixture
    ASSERT_TRUE(m_model.isValid());
}
```

### Testing Exceptions
```cpp
TEST(MyTest, ThrowsException) {
    MyClass obj;
    ASSERT_THROW(obj.methodThatThrows(), std::runtime_error);
}

TEST(MyTest, NoThrow) {
    MyClass obj;
    ASSERT_NO_THROW(obj.methodThatDoesntThrow());
}
```

## 📝 Node Types Quick Reference

| Category | Node Types |
|----------|-----------|
| **Primitives** | Sphere, Box, Cylinder, Cone, Torus, Capsule, Plane |
| **Boolean Ops** | Union, Intersection, Difference, SmoothUnion, SmoothIntersection |
| **Math Functions** | Add, Subtract, Multiply, Divide, Power, Sqrt, Abs, Min, Max |
| **Trigonometry** | Sin, Cos, Tan, Asin, Acos, Atan, Atan2 |
| **Vector Ops** | Dot, Cross, Normalize, Length, Distance |
| **Interpolation** | Mix, Smoothstep, Step, Clamp |
| **Resources** | MeshResource, ImageStackResource, VdbResource |
| **Transforms** | Translate, Rotate, Scale, Matrix |

## 🎨 ImGUI Patterns

### Basic Window
```cpp
void MyWindow::render() {
    if (ImGui::Begin("My Window")) {
        ImGui::Text("Hello, World!");
        
        if (ImGui::Button("Click Me")) {
            // Handle click
        }
        
        ImGui::End();
    }
}
```

### Input Widgets
```cpp
// Float input
float value = 1.0f;
ImGui::InputFloat("Value", &value);

// Slider
ImGui::SliderFloat("Radius", &radius, 0.0f, 10.0f);

// Color picker
ImVec4 color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
ImGui::ColorEdit4("Color", (float*)&color);

// Checkbox
bool enabled = true;
ImGui::Checkbox("Enabled", &enabled);

// Combo box
const char* items[] = {"Item 1", "Item 2", "Item 3"};
static int currentItem = 0;
ImGui::Combo("Select", &currentItem, items, IM_ARRAYSIZE(items));
```

## 🔐 Error Handling Patterns

### Exception Handling
```cpp
try {
    auto model = importer.import();
    // Use model
} catch (const ImportException& e) {
    EventLogger::instance().error("Import failed: {}", e.what());
} catch (const std::exception& e) {
    EventLogger::instance().error("Unexpected error: {}", e.what());
}
```

### OpenCL Error Checking
```cpp
#include "ComputeContext.h"

cl_int err;
cl::Buffer buffer(context, CL_MEM_READ_WRITE, size, nullptr, &err);
CL_ERROR(err);  // Throws if error
```

### Validation
```cpp
bool validate(const Model& model) {
    if (!model.hasBeginNode()) {
        EventLogger::instance().error("Model missing begin node");
        return false;
    }
    
    if (!model.hasEndNode()) {
        EventLogger::instance().error("Model missing end node");
        return false;
    }
    
    if (model.hasCycle()) {
        EventLogger::instance().error("Model contains cycle");
        return false;
    }
    
    return true;
}
```

## 📚 Common Includes

```cpp
// Core
#include <nodes/Model.h>
#include <nodes/Assembly.h>
#include <nodes/DerivedNodes.h>

// Compute
#include <ComputeContext.h>
#include <compute/ComputeCore.h>
#include <compute/ProgramManager.h>

// I/O
#include <io/3mf/Importer3mf.h>
#include <io/3mf/Writer3mf.h>
#include <io/MeshExporter.h>

// Resources
#include <ResourceManager.h>
#include <MeshResource.h>
#include <ImageStackResource.h>

// Utilities
#include <EventLogger.h>
#include <Profiling.h>
#include <types.h>
```

## 🌐 File Format Extensions

| Extension | Format | Use Case |
|-----------|--------|----------|
| `.3mf` | 3MF with volumetric | Import/Export models |
| `.stl` | STL mesh | Export meshes |
| `.obj` | Wavefront OBJ | Export meshes |
| `.svg` | SVG vector | Export contours |
| `.cli` | CLI format | Export contours for manufacturing |
| `.vdb` | OpenVDB | Import/Export volumetric data |
| `.raw` | Raw binary | Image stack data |

## 🎯 Performance Tips

### DO
- ✅ Cache compiled kernels
- ✅ Use local memory in kernels
- ✅ Batch GPU operations
- ✅ Use OpenGL interop for rendering
- ✅ Profile before optimizing

### DON'T
- ❌ Copy between CPU/GPU unnecessarily
- ❌ Recompile kernels on every frame
- ❌ Use printf in production kernels
- ❌ Allocate in hot loops
- ❌ Use global state

## 🔗 Useful Links

- [Main Documentation](README.md)
- [Developer Guide](DEVELOPER_GUIDE.md)
- [Architecture Overview](ARCHITECTURE.md)
- [API Reference](API_REFERENCE.md)
- [GitHub Repository](https://github.com/3MFConsortium/gladius)

---

**Tip**: Bookmark this page for quick reference during development!
