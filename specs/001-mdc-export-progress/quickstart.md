# Quickstart: MDC Export Progress

## What This Feature Does

When exporting a mesh using Manifold Dual Contouring, the progress bar now updates smoothly throughout the export process instead of appearing frozen.

## For Users

No action required. The progress bar will automatically show real-time progress during mesh export.

## For Developers

### Testing the Feature

1. Load a complex model (e.g., gyroid structure with fine features)
2. Open the mesh export dialog
3. Select "Manifold Dual Contouring" method
4. Set quality to "Ultra Fine" for longer export time
5. Click Export
6. Observe the progress bar updating smoothly throughout

### Using the Progress Callback API

```cpp
#include "compute/ManifoldDualContouringGpu.h"

compute::ManifoldDualContouringGpu pipeline(core);
pipeline.setConfig(config);

// Optional: Set progress callback
pipeline.setProgressCallback([](float progress, std::string_view phase) {
    std::cout << phase << ": " << (progress * 100.0f) << "%" << std::endl;
});

pipeline.generateMesh();
```

### Running Tests

```bash
# Run progress callback unit tests
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=ManifoldDualContouringProgress*
```

## Key Files

| File | Purpose |
|------|---------|
| `compute/ManifoldDualContouringGpu.h` | New `setProgressCallback()` API |
| `compute/ManifoldDualContouringGpu.cpp` | Progress reporting implementation |
| `io/ManifoldDualContouringStlExporter.cpp` | Wires callback to atomic progress |
| `tests/unittests/ManifoldDualContouringProgress_tests.cpp` | Unit tests |

## Troubleshooting

**Progress bar still jumps**: Ensure you're using Manifold Dual Contouring method, not other extraction methods.

**Progress appears stuck at 25%**: This indicates octree construction is taking time. Normal for complex models.

**Progress appears stuck at 65-80%**: Post-processing (sharp features, simplification) is running. Normal when these options are enabled.
