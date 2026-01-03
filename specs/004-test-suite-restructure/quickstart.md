# Quickstart: Running Gladius Tests

**Feature**: 004-test-suite-restructure  
**Date**: January 3, 2026

## Overview

Gladius tests are organized into three categories:

| Category | Purpose | Typical Duration | GPU Required |
|----------|---------|------------------|--------------|
| **Unit** | Fast feedback during development | <60 seconds | No |
| **Integration** | Comprehensive validation with GPU | ~10 minutes | Yes |
| **API** | External interface validation | ~2 minutes | No |

## Quick Commands

### Run Unit Tests (Default for Development)

```bash
cd gladius
ctest --preset UnitTests
```

Or use VS Code task: **Run Unit Tests**

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

Run tests for a specific component using CTest label filters:

```bash
# Run all mesh export tests (unit + integration)
ctest --preset AllTests -L mesh-export

# Run only unit tests for dual contouring
ctest --preset UnitTests -L dual-contouring

# Run MCP protocol tests
ctest --preset ApiTests -L mcp

# Run everything EXCEPT GPU tests
ctest --preset AllTests -LE opencl
```

### Available Component Labels

| Label | Description |
|-------|-------------|
| `mesh-export` | Mesh writing/export (3MF, STL) |
| `dual-contouring` | Dual contouring algorithms |
| `mcp` | Model Context Protocol |
| `opencl` | Tests requiring OpenCL context |
| `io` | File I/O operations |
| `graph` | Node graph operations |
| `mesh` | General mesh operations |
| `sdf` | Signed distance field |
| `beamlattice` | Beam lattice operations |
| `parser` | Expression/function parsing |
| `ui` | User interface tests |

## Environment Variables

| Variable | Purpose | Default |
|----------|---------|---------|
| `GLADIUS_RUN_GPU_TESTS` | Enable GPU-heavy tests in integration suite | `0` |
| `GLADIUS_DEBUG_MDC_CONFIG` | Enable MDC config debug logging | `0` |

**Note**: The `IntegrationTests` and `AllTests` presets automatically set `GLADIUS_RUN_GPU_TESTS=1`.

## Test Output

Test results are logged to:
- `out/build/linux-releaseWithDebug/ctest-*.log`

Failed test output is displayed automatically with `--output-on-failure` (enabled in all presets).

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
