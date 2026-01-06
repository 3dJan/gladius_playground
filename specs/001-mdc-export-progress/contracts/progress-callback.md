# Progress Callback API Contract

**Date**: 2026-01-06  
**Component**: ManifoldDualContouringGpu

## Callback Type Definition

```cpp
namespace gladius::compute
{
    /// Progress callback for mesh generation operations.
    /// @param progress Normalized progress value in range [0.0, 1.0]
    /// @param phaseName Human-readable name of current phase (optional)
    using MeshGenerationProgressCallback = std::function<void(float progress, std::string_view phaseName)>;
}
```

## API Methods

### ManifoldDualContouringGpu::setProgressCallback

```cpp
/// Set callback for progress updates during mesh generation.
/// @param callback Function to call with progress updates.
///                 May be nullptr to disable progress reporting.
/// @note Callback is invoked from the calling thread (not a separate thread).
/// @note Callback should be lightweight; heavy operations will slow mesh generation.
void setProgressCallback(MeshGenerationProgressCallback callback);
```

### Progress Value Semantics

| Value | Meaning |
|-------|---------|
| 0.0 | Not started |
| (0.0, 1.0) | In progress |
| 1.0 | Complete |

### Phase Names

The following phase names will be reported:

- `"Initializing"` - Setup and kernel loading
- `"Building octree"` - Octree construction
- `"Generating vertices"` - Vertex generation
- `"Generating indices"` - Index/face generation
- `"Processing sharp features"` - Sharp feature post-processing
- `"Simplifying mesh"` - Mesh simplification pass
- `"Improving quality"` - Quality improvement pass

## Usage Example

```cpp
ManifoldDualContouringGpu pipeline(core);
pipeline.setConfig(config);

std::atomic<double> progress{0.0};
pipeline.setProgressCallback([&progress](float p, std::string_view /*phase*/) {
    progress.store(static_cast<double>(p));
});

pipeline.generateMesh();  // Progress updates during execution
```

## Thread Safety

- Callback is invoked synchronously from `generateMesh()`
- Callback must be thread-safe if progress is read from another thread
- Recommended: use `std::atomic` for progress storage

## Error Handling

- Exceptions from callback are NOT caught - will propagate to caller
- Progress is NOT reset on error - remains at last reported value
- Caller should check for errors separately via return value or exception

## Backward Compatibility

- Default behavior (no callback set): identical to current behavior
- Existing code continues to work unchanged
- Progress reporting is opt-in
