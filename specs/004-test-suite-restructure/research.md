# Research: Test Suite Restructuring

**Feature**: 004-test-suite-restructure  
**Date**: January 3, 2026

## Research Tasks

### 1. CTest Labels with gtest_discover_tests

**Decision**: Use `gtest_discover_tests()` with `PROPERTIES LABELS` parameter to assign labels to discovered tests.

**Rationale**: 
- CMake 3.21+ supports `PROPERTIES` parameter in `gtest_discover_tests()`
- Labels can be assigned at discovery time, allowing CTest filtering without modifying test source code
- Multiple labels can be applied per test (e.g., "unit;mesh-export")

**Example**:
```cmake
gtest_discover_tests(gladius_test
    PROPERTIES LABELS "unit"
    TEST_PREFIX "unit."
)
```

**Alternatives considered**:
- Manual `add_test()` + `set_tests_properties()`: Too verbose for 70+ test files
- Separate executables per component: Increases build time, complicates maintenance

### 2. Test Categorization Criteria

**Decision**: Categorize tests based on their dependencies and execution time:

| Category | Criteria | Examples |
|----------|----------|----------|
| **unit** | <1s execution, no GPU/OpenCL, no external tools, no network | Parser tests, math tests, pure algorithm tests |
| **integration** | Requires GPU/OpenCL, external tools (admesh), or >5s execution | DualContouring GPU tests, mesh export validation |
| **api** | Tests external API boundaries from consumer perspective | GladiusLib component API, MCP protocol |

**Rationale**: Clear boundaries prevent ambiguity about where new tests belong.

### 3. Files Requiring Migration to Integration Tests

**Decision**: Move the following 14 files from `unittests/` to `integrationtests/`:

| File | Reason |
|------|--------|
| `ColorExport_Integration_tests.cpp` | OpenCL context required |
| `DualContouringOctree_tests.cpp` | OpenCL context required |
| `DualContouringStlExporter_tests.cpp` | OpenCL context required |
| `GlobalMortonOctree_tests.cpp` | OpenCL context required |
| `HierarchicalDC_CompilationDebug_tests.cpp` | OpenCL context required |
| `HierarchicalDC_ExtractionStep_tests.cpp` | OpenCL context required |
| `HierarchicalDC_STLExport_tests.cpp` | OpenCL + admesh required |
| `HierarchicalDualContouring_tests.cpp` | OpenCL context required |
| `ManifoldDualContouring_tests.cpp` | GPU tests gated by GLADIUS_RUN_GPU_TESTS |
| `MeshBaseline_tests.cpp` | OpenCL context required |
| `MeshSdfPerformance_tests.cpp` | GPU tests gated by GLADIUS_RUN_GPU_TESTS |
| `NodeLayoutEngine_tests.cpp` | Some tests require OpenCL |
| `PaletteExtractor_tests.cpp` | OpenCL context required |
| `ShellGenerator_tests.cpp` | OpenCL context required |

**Rationale**: These files contain `GTEST_SKIP() << "OpenCL context not available"` or `GLADIUS_RUN_GPU_TESTS` checks, indicating GPU dependency.

### 4. API Test Structure

**Decision**: Create new `apitests/` directory with tests for:
- GladiusLib component API (existing `integrationtests/GladiusLib_tests.cpp`)
- MCP protocol tests (existing `unittests/MCP_tests.cpp`, `ApplicationMCPAdapter_tests.cpp`)

**Rationale**: 
- API tests validate external contracts from a consumer perspective
- Separating them enables focused CI stages for backwards compatibility validation
- MCP tests currently in unittests are already testing external protocol behavior

### 5. Component Labels/Scopes

**Decision**: Define the following component labels:

| Label | Description | Example Files |
|-------|-------------|---------------|
| `mesh-export` | Mesh writing/export functionality | `MeshWriter3mf*`, `Writer3mf*`, `*Exporter*` |
| `dual-contouring` | Dual contouring algorithms | `DualContouring*`, `HierarchicalDC*`, `ManifoldDualContouring*` |
| `mcp` | Model Context Protocol | `MCP_tests`, `ApplicationMCPAdapter*`, `JSONRPC*` |
| `opencl` | OpenCL compute tests | All files requiring OpenCL context |
| `io` | File I/O operations | `CliReader*`, `Writer3mf*`, `ImageExtractor*` |
| `graph` | Node graph operations | `GraphFlattener*`, `ExpressionToGraph*`, `NodeView*` |
| `mesh` | General mesh operations | `MeshBVH*`, `MeshSimplification*`, `MeshVoxelGrid*` |
| `sdf` | Signed distance field | `MeshSDF*`, `SignDetermination*` |

**Rationale**: Labels mirror the logical architecture of Gladius, enabling developers to test specific subsystems.

### 6. CTest Preset Configuration

**Decision**: Add the following test presets to `CMakePresets.json`:

```json
{
  "name": "UnitTests",
  "configurePreset": "linux-releaseWithDebug",
  "filter": { "include": { "label": "unit" } },
  "output": { "outputOnFailure": true }
},
{
  "name": "IntegrationTests", 
  "configurePreset": "linux-releaseWithDebug",
  "filter": { "include": { "label": "integration" } },
  "environment": { "GLADIUS_RUN_GPU_TESTS": "1" }
},
{
  "name": "ApiTests",
  "configurePreset": "linux-releaseWithDebug", 
  "filter": { "include": { "label": "api" } }
}
```

**Rationale**: Presets provide single-command test execution without remembering filter syntax.

### 7. VS Code Task Updates

**Decision**: Update `.vscode/tasks.json` to include:
- "Run Unit Tests" - fast feedback for developers
- "Run Integration Tests" - comprehensive GPU testing
- "Run API Tests" - external interface validation
- "Run All Tests" - full suite

**Rationale**: Tasks provide IDE integration matching the command-line experience.

## Summary

All NEEDS CLARIFICATION items have been resolved. The implementation approach is:

1. **Migrate files**: Move 14 GPU-dependent test files to `integrationtests/`
2. **Create apitests/**: New directory for API boundary tests
3. **Add CMake labels**: Apply category + component labels via `gtest_discover_tests()`
4. **Add CTest presets**: UnitTests, IntegrationTests, ApiTests in CMakePresets.json
5. **Update tasks.json**: VS Code task integration
6. **Update documentation**: Developer onboarding with new test organization
