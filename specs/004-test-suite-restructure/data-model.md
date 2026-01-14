# Data Model: Test Suite Restructuring

**Feature**: 004-test-suite-restructure  
**Date**: January 3, 2026

## Entities

### Test Category

Classification of tests by execution characteristics and dependencies.

| Attribute | Type | Description |
|-----------|------|-------------|
| name | string | Category identifier: `unit`, `integration`, `api` |
| label | string | CTest label applied to tests |
| executable | string | Test binary name |
| max_duration | duration | Maximum expected runtime for full category |
| requires_gpu | bool | Whether GPU/OpenCL is required |
| requires_network | bool | Whether network access is required |
| requires_tools | list | External tools required (e.g., `admesh`) |

**Instances**:

| name | label | executable | max_duration | requires_gpu | requires_network | requires_tools |
|------|-------|------------|--------------|--------------|------------------|----------------|
| unit | `unit` | `gladius_test` | 60s | false | false | [] |
| integration | `integration` | `gladius_integrationtest` | 10min | true | false | [admesh] |
| api | `api` | `gladius_apitest` | 2min | false | false | [] |

---

### Test Scope (Component Label)

Feature area or subsystem that a test covers. Tests may have multiple scopes.

| Attribute | Type | Description |
|-----------|------|-------------|
| name | string | Scope identifier (CTest label) |
| description | string | Human-readable description |
| file_patterns | list | Glob patterns matching test files |

**Instances**:

| name | description | file_patterns |
|------|-------------|---------------|
| `mesh-export` | Mesh writing and export | `*Writer3mf*`, `*Exporter*`, `CliWriter*` |
| `dual-contouring` | Dual contouring algorithms | `DualContouring*`, `HierarchicalDC*`, `ManifoldDualContouring*` |
| `mcp` | Model Context Protocol | `MCP_*`, `ApplicationMCPAdapter*`, `JSONRPC*` |
| `opencl` | OpenCL compute | (tests requiring OpenCL context) |
| `io` | File I/O operations | `CliReader*`, `*Writer*`, `ImageExtractor*` |
| `graph` | Node graph operations | `GraphFlattener*`, `ExpressionToGraph*`, `NodeView*`, `Node*` |
| `mesh` | General mesh operations | `MeshBVH*`, `MeshSimplification*`, `MeshVoxelGrid*`, `Mesh*` |
| `sdf` | Signed distance field | `MeshSDF*`, `SignDetermination*`, `Spatial*` |
| `beamlattice` | Beam lattice operations | `BeamLattice*` |
| `parser` | Expression/function parsing | `ExpressionParser*`, `Function*` |
| `ui` | User interface | `*Dialog*`, `MainWindow*` |

---

### Test Preset

Pre-configured CTest execution profile.

| Attribute | Type | Description |
|-----------|------|-------------|
| name | string | Preset identifier |
| configurePreset | string | Build configuration to use |
| filter | object | CTest filter (label include/exclude) |
| environment | object | Environment variables to set |
| output | object | Output configuration |

**Instances**:

| name | configurePreset | filter.include.label | environment |
|------|-----------------|----------------------|-------------|
| `UnitTests` | `linux-releaseWithDebug` | `unit` | {} |
| `IntegrationTests` | `linux-releaseWithDebug` | `integration` | `GLADIUS_RUN_GPU_TESTS=1` |
| `ApiTests` | `linux-releaseWithDebug` | `api` | {} |
| `AllTests` | `linux-releaseWithDebug` | (none) | `GLADIUS_RUN_GPU_TESTS=1` |

---

### Test File Migration

Mapping of test files to their target category.

| Current Location | Target Category | Reason |
|-----------------|-----------------|--------|
| `unittests/ColorExport_Integration_tests.cpp` | integration | OpenCL required |
| `unittests/DualContouringOctree_tests.cpp` | integration | OpenCL required |
| `unittests/DualContouringStlExporter_tests.cpp` | integration | OpenCL required |
| `unittests/GlobalMortonOctree_tests.cpp` | integration | OpenCL required |
| `unittests/HierarchicalDC_CompilationDebug_tests.cpp` | integration | OpenCL required |
| `unittests/HierarchicalDC_ExtractionStep_tests.cpp` | integration | OpenCL required |
| `unittests/HierarchicalDC_STLExport_tests.cpp` | integration | OpenCL + admesh |
| `unittests/HierarchicalDualContouring_tests.cpp` | integration | OpenCL required |
| `unittests/ManifoldDualContouring_tests.cpp` | integration | GPU tests |
| `unittests/MeshBaseline_tests.cpp` | integration | OpenCL required |
| `unittests/MeshSdfPerformance_tests.cpp` | integration | GPU tests |
| `unittests/NodeLayoutEngine_tests.cpp` | integration | OpenCL required |
| `unittests/PaletteExtractor_tests.cpp` | integration | OpenCL required |
| `unittests/ShellGenerator_tests.cpp` | integration | OpenCL required |
| `unittests/MCP_tests.cpp` | api | External protocol |
| `unittests/ApplicationMCPAdapter_tests.cpp` | api | External protocol |
| `unittests/ApplicationMCPAdapter_Rollback_tests.cpp` | api | External protocol |
| `integrationtests/GladiusLib_tests.cpp` | api | External API |
| `integrationtests/MCP_tests.cpp` | api | External protocol |

## Relationships

```
┌─────────────────┐
│  Test Category  │
│  (unit/int/api) │
└────────┬────────┘
         │ 1:N
         ▼
┌─────────────────┐       N:M       ┌─────────────────┐
│   Test File     │◄───────────────►│   Test Scope    │
│  (*_tests.cpp)  │                 │   (component)   │
└────────┬────────┘                 └─────────────────┘
         │ 1:N
         ▼
┌─────────────────┐
│   Test Case     │
│ (TEST/TEST_F)   │
└─────────────────┘
```

## State Transitions

N/A - Test organization is static after configuration.

## Validation Rules

1. Every test file MUST belong to exactly one category (unit, integration, api)
2. Every test file SHOULD have at least one component scope label
3. Unit tests MUST NOT contain `GTEST_SKIP() << "OpenCL context"` patterns
4. Unit tests MUST NOT reference `GLADIUS_RUN_GPU_TESTS` environment variable
5. API tests MUST test external interface boundaries, not internal implementation
