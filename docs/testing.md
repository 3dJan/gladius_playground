# Running Gladius Tests

**Last Updated**: January 3, 2026

## Overview

Gladius tests are organized into three categories:

| Category | Purpose | Typical Duration | GPU Required |
|----------|---------|------------------|--------------|
| **Unit** | Fast feedback during development | ~5 seconds | No |
| **Integration** | Comprehensive validation with GPU | ~10 minutes | Yes |
| **API** | External interface validation | ~2 minutes | No |

## Quick Commands

### Run Unit Tests (Default for Development)

**Fast Method** (recommended for local development):
```bash
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test
```
This runs all unit tests directly via GTest in **~5 seconds**.

**CTest Method** (with label filtering, but slower):
```bash
cd gladius
ctest --preset UnitTests
```
This takes ~3-4 minutes due to CTest spawning a process per test.

Or use VS Code task: **Run Unit Tests (Fast)**

> **Performance Note**: CTest's `gtest_discover_tests()` creates a separate process for each test case, adding ~0.3s overhead per test. For rapid feedback during development, run `./gladius_test` directly. Use CTest when you need label filtering or CI integration.

### Run Integration Tests (With GPU)

```bash
cd gladius
ctest --preset IntegrationTests
```

Or use VS Code task: **Run Integration Tests**

### Run API Tests

```bash
cd gladius
ctest --preset ApiTests
```

Or use VS Code task: **Run API Tests**

### Run All Tests

```bash
cd gladius
ctest --preset AllTests
```

Or use VS Code task: **Run All Tests**

## Filtering by Component

Run tests for a specific component using CTest regex filters:

```bash
# Run all mesh export tests
ctest --preset AllTests -R "MeshWriter3mf|Writer3mf|Exporter|CliWriter"

# Run dual contouring tests  
ctest --preset AllTests -R "DualContouring|HierarchicalDC|ManifoldDualContouring"

# Run MCP protocol tests
ctest --preset AllTests -R "MCP|ApplicationMCPAdapter|JSONRPC"

# Run everything EXCEPT GPU tests
ctest --preset UnitTests
```

### Component Filter Patterns

| Component | Regex Pattern |
|-----------|---------------|
| `mesh-export` | `MeshWriter3mf\|Writer3mf\|Exporter\|CliWriter` |
| `dual-contouring` | `DualContouring\|HierarchicalDC\|ManifoldDualContouring` |
| `mcp` | `MCP\|ApplicationMCPAdapter\|JSONRPC` |
| `io` | `CliReader\|Writer\|ImageExtractor\|Reader` |
| `graph` | `GraphFlattener\|ExpressionToGraph\|NodeView\|NodeGraph` |
| `mesh` | `MeshBVH\|MeshSimplification\|MeshVoxelGrid\|Mesh_Test` |
| `sdf` | `MeshSDF\|SignDetermination\|Spatial\|SDF` |
| `beamlattice` | `BeamLattice` |
| `parser` | `ExpressionParser\|Function\|ImplicitModeling` |
| `ui` | `Dialog\|MainWindow\|Export` |

**Tip**: Combine `-R` (include regex) with preset filters:
```bash
# Run only unit tests matching mesh-related patterns
ctest --preset UnitTests -R "MeshWriter3mf|Mesh_Test"
```

## Environment Variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `GLADIUS_RUN_GPU_TESTS` | Enable GPU-heavy tests in integration suite | `0` |
| `GLADIUS_DEBUG_MDC_CONFIG` | Enable MDC config debug logging | `0` |

**Note**: The `IntegrationTests` and `AllTests` presets automatically set `GLADIUS_RUN_GPU_TESTS=1`.

## Writing New Tests

### Where to Place Tests

| Your test... | Place it in... |
|--------------|----------------|
| Tests pure algorithms, no GPU | `tests/unittests/` |
| Requires OpenCL context | `tests/integrationtests/` |
| Requires external tools (admesh) | `tests/integrationtests/` |
| Tests GladiusLib API or MCP protocol | `tests/apitests/` |
| Takes >5 seconds to run | `tests/integrationtests/` |

### Naming Convention

Follow the constitution test naming: `[UnitOfWork_StateUnderTest_ExpectedBehavior]`

```cpp
TEST(MeshWriter3mf_Test, WriteTriangles_WithValidMesh_CreatesValidOutput)
TEST(DualContouring_Test, ExtractMesh_WithEmptySDF_ReturnsEmptyMesh)
```

### Prerequisite Checks

For integration tests that may not have prerequisites:

```cpp
TEST_F(MyIntegrationTest, Feature_Works)
{
    if (!hasOpenCLContext())
    {
        GTEST_SKIP() << "OpenCL context not available";
    }
    // Test code...
}
```

## Troubleshooting

### "OpenCL context not available" Skips

This is expected when running integration tests without GPU. Tests skip gracefully rather than fail.

### Tests Taking Too Long

If unit tests take >60 seconds, some tests may have been miscategorized. Check for:
- Tests with `GTEST_SKIP()` for OpenCL (should be in integration)
- Tests loading large files (should be in integration)  
- Performance benchmark tests (should be in integration)

### GPU Tests Not Running

Ensure `GLADIUS_RUN_GPU_TESTS=1` is set. The `IntegrationTests` preset sets this automatically.

## Test Counts (as of January 3, 2026)

| Category | Test Count |
|----------|------------|
| Unit Tests | 520 |
| Integration Tests | 127 |
| API Tests | 90 |
| **Total** | **737** |
