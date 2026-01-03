# Code Review: Test Suite Restructuring

**Branch**: `004-test-suite-restructure`  
**Diff base**: `develop`  
**Reviewed**: January 3, 2026

## Summary of Changes

This PR restructures the test suite to separate tests into three categories:

1. **Unit tests** (`gladius_test`) - Fast, no GPU required, ~5 seconds
2. **Integration tests** (`gladius_integrationtest`) - GPU/OpenCL tests, longer running
3. **API tests** (`gladius_apitest`) - MCP and GladiusLib external interface tests

### Files Changed

| Change Type | Count | Description |
|-------------|-------|-------------|
| Renamed | 19 | Test files moved between directories |
| Added | 5 | New apitests infrastructure files |
| Modified | 4 | CMakeLists.txt files and CMakePresets.json |

### File Migrations

- **14 files**: `unittests/` → `integrationtests/` (GPU-dependent tests)
- **5 files**: `unittests/` + `integrationtests/` → `apitests/` (API boundary tests)

---

## Review Findings

### 1. Missing Newline at End of File

**Location**: [gladius/tests/apitests/testdata.h](gladius/tests/apitests/testdata.h#L13)

**Issue**: File does not end with a newline character. POSIX standard requires text files to end with newline, and some tools/compilers may warn.

**Suggestion**:
```cpp
        static auto constexpr Implicit3mf = "testdata/RadialRadiator.3mf";
    };
}
```
Add a newline after the closing brace.

**Rationale**: Compliance with POSIX text file standard; avoids compiler warnings (e.g., `no newline at end of file`).

---

### 2. Inconsistent Whitespace in testhelper.h

**Location**: [gladius/tests/apitests/testhelper.h](gladius/tests/apitests/testhelper.h#L11-L14)

**Issue**: Extra leading space before `void saveBitmapLayer` declaration and inconsistent alignment.

**Current code**:
```cpp
    [[nodiscard]] std::optional<std::filesystem::path> findGladiusSharedLib();
    
     void saveBitmapLayer(std::filesystem::path const & filename,
                         std::vector<float> & data,
```

**Suggestion**:
```cpp
    [[nodiscard]] std::optional<std::filesystem::path> findGladiusSharedLib();

    void saveBitmapLayer(std::filesystem::path const & filename,
                         std::vector<float> & data,
```

**Rationale**: Consistent indentation improves readability; the extra space is likely a copy-paste artifact.

---

### 3. Inconsistent Whitespace in testhelper.cpp

**Location**: [gladius/tests/apitests/testhelper.cpp](gladius/tests/apitests/testhelper.cpp#L55)

**Issue**: Extra leading space before `std::cout` statement.

**Current code**:
```cpp
        image.resize(data.size());

         std::cout << "exporting bitmap to " << filename << "\n";
```

**Suggestion**:
```cpp
        image.resize(data.size());

        std::cout << "exporting bitmap to " << filename << "\n";
```

**Rationale**: Consistent indentation; appears to be a copy-paste artifact from the original file.

---

### 4. Duplicate Code: testhelper.cpp/.h Files

**Location**: 
- [gladius/tests/apitests/testhelper.cpp](gladius/tests/apitests/testhelper.cpp)
- [gladius/tests/apitests/testhelper.h](gladius/tests/apitests/testhelper.h)
- [gladius/tests/integrationtests/testhelper.cpp](gladius/tests/integrationtests/testhelper.cpp)
- [gladius/tests/integrationtests/testhelper.h](gladius/tests/integrationtests/testhelper.h)

**Issue**: The `testhelper.cpp` and `testhelper.h` files are now duplicated between `apitests/` and `integrationtests/`. This violates DRY principle.

**Suggestion**: Create a shared test utilities library:
```cmake
# gladius/tests/CMakeLists.txt
add_library(gladius_testhelper STATIC
    common/testhelper.cpp
    common/testhelper.h
    common/testdata.h
)
target_include_directories(gladius_testhelper PUBLIC common)
target_link_libraries(gladius_testhelper PUBLIC lodepng fmt::fmt)

add_subdirectory(unittests)
add_subdirectory(integrationtests)
add_subdirectory(apitests)
```

Then link against it:
```cmake
# In apitests/CMakeLists.txt and integrationtests/CMakeLists.txt
target_link_libraries(${BINARY} PRIVATE gladius_testhelper ...)
```

**Rationale**: 
- Eliminates code duplication (DRY)
- Ensures consistent behavior across test suites
- Single point of maintenance for test utilities

**Priority**: Medium - Not blocking, but should be addressed in a follow-up PR.

---

### 5. Potential Memory Issue: Non-const Reference for Read-Only Data

**Location**: [gladius/tests/apitests/testhelper.h](gladius/tests/apitests/testhelper.h#L12)

**Issue**: `saveBitmapLayer` takes `std::vector<float> & data` as non-const reference, but the function only reads from it (transforms data into image buffer).

**Current code**:
```cpp
void saveBitmapLayer(std::filesystem::path const & filename,
                     std::vector<float> & data,
                     unsigned int width_px,
                     unsigned int height_px);
```

**Suggestion**:
```cpp
void saveBitmapLayer(std::filesystem::path const & filename,
                     std::vector<float> const & data,
                     unsigned int width_px,
                     unsigned int height_px);
```

**Rationale**: 
- Const-correctness signals intent and prevents accidental modification
- Allows calling with const vectors or temporaries
- Note: This would need corresponding changes in the integrationtests copy as well

---

### 6. CMake Version Inconsistency

**Location**: 
- [gladius/tests/unittests/CMakeLists.txt](gladius/tests/unittests/CMakeLists.txt#L1): `cmake_minimum_required(VERSION 3.12)`
- [gladius/tests/integrationtests/CMakeLists.txt](gladius/tests/integrationtests/CMakeLists.txt#L2): `cmake_minimum_required(VERSION 3.21)`
- [gladius/tests/apitests/CMakeLists.txt](gladius/tests/apitests/CMakeLists.txt#L1): `cmake_minimum_required(VERSION 3.21)`

**Issue**: Inconsistent CMake minimum version requirements across test subdirectories.

**Suggestion**: Unify to `VERSION 3.21` across all test CMakeLists.txt files (matches CMakePresets.json requirement).

```cmake
cmake_minimum_required(VERSION 3.21)
```

**Rationale**: 
- Consistency across the codebase
- CMakePresets.json requires version 3.21+ anyway
- Avoids confusion about actual minimum requirements

---

### 7. Missing DISCOVERY_MODE in integrationtests and apitests

**Location**: 
- [gladius/tests/integrationtests/CMakeLists.txt](gladius/tests/integrationtests/CMakeLists.txt#L68-L70)
- [gladius/tests/apitests/CMakeLists.txt](gladius/tests/apitests/CMakeLists.txt#L66-L68)

**Issue**: `gtest_discover_tests()` in integrationtests and apitests does not use `DISCOVERY_MODE POST_BUILD`, while unittests does.

**Current code** (integrationtests):
```cmake
gtest_discover_tests(${CMAKE_PROJECT_NAME}_integrationtest
    PROPERTIES LABELS "integration"
)
```

**Suggestion**:
```cmake
gtest_discover_tests(${CMAKE_PROJECT_NAME}_integrationtest
    PROPERTIES LABELS "integration"
    DISCOVERY_MODE POST_BUILD
)
```

**Rationale**: 
- Consistency with unittests CMakeLists.txt
- POST_BUILD discovery is more robust for incremental builds
- Allows `set_tests_properties` to work after discovery

---

### 8. Redundant Link to gladius_lib

**Location**: [gladius/tests/apitests/CMakeLists.txt](gladius/tests/apitests/CMakeLists.txt#L42-L43)

**Issue**: `gladius_lib` is added to DEPENDENCIES list and then linked again separately.

**Current code**:
```cmake
set(DEPENDENCIES ${CMAKE_PROJECT_NAME}_lib unofficial::imgui-node-editor::imgui-node-editor ...)
...
target_link_libraries(${BINARY} PRIVATE ${CMAKE_PROJECT_NAME}_lib ${DEPENDENCIES})
```

**Suggestion**:
```cmake
set(DEPENDENCIES unofficial::imgui-node-editor::imgui-node-editor ...)
...
target_link_libraries(${BINARY} PRIVATE ${CMAKE_PROJECT_NAME}_lib ${DEPENDENCIES})
```

Or:
```cmake
set(DEPENDENCIES ${CMAKE_PROJECT_NAME}_lib unofficial::imgui-node-editor::imgui-node-editor ...)
...
target_link_libraries(${BINARY} PRIVATE ${DEPENDENCIES})
```

**Rationale**: Removes redundant dependency specification; cleaner CMake code.

---

### 9. Namespace Mismatch for API Tests

**Location**: [gladius/tests/apitests/testdata.h](gladius/tests/apitests/testdata.h#L5)

**Issue**: The namespace `gladius_integration_tests` is used in apitests, but these are now API tests.

**Suggestion**: Either rename to `gladius_api_tests` or use a shared namespace like `gladius::tests`:

```cpp
namespace gladius::tests
{
    struct FileNames
    {
        // ...
    };
}
```

**Rationale**: 
- Semantic clarity: API tests are not integration tests
- A shared `gladius::tests` namespace would work for all test types

**Priority**: Low - cosmetic improvement.

---

## Summary

| Severity | Count | Description |
|----------|-------|-------------|
| 🔴 High | 0 | No critical issues |
| 🟡 Medium | 1 | #4 (duplicate testhelper) |
| 🟢 Low | 1 | #9 (namespace mismatch - cosmetic) |

### Fixed Issues ✅

- ✅ #1: Missing newline at EOF in testdata.h
- ✅ #2: Inconsistent whitespace in testhelper.h
- ✅ #3: Inconsistent whitespace in testhelper.cpp
- ✅ #5: Non-const reference for read-only data
- ✅ #6: CMake version inconsistency (unified to 3.21)
- ✅ #7: Missing DISCOVERY_MODE in integrationtests and apitests
- ✅ #8: Redundant link to gladius_lib

### Remaining Actions

1. **Follow-up PR**: Extract shared test utilities library (#4)

### Positive Observations

✅ Clean separation of test categories  
✅ Proper use of CTest labels for filtering  
✅ Well-structured CMakePresets.json with descriptive test presets  
✅ Correct static library linking order (imgui-node-editor before imgui)  
✅ Consistent use of `#pragma once`  
✅ Good use of modern CMake practices (target-based, CONFIGURE_DEPENDS)

---

*Review by GitHub Copilot*

