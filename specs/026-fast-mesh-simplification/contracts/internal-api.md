# Internal API Contracts: Fast Mesh Simplification

**Feature**: 026-fast-mesh-simplification  
**Date**: 2026-03-26

This feature has no external APIs (REST, GraphQL, etc.). All contracts are internal C++ interfaces.

## Contract 1: FastQemSimplifier Public Interface

```cpp
namespace gladius::compute
{
    /// Configuration for fast geometric QEM simplification (CPU-only)
    struct FastQemConfig
    {
        /// Termination mode
        SimplificationTerminationMode terminationMode{SimplificationTerminationMode::TargetReductionPercent};

        /// Target triangle count (used when terminationMode == TargetTriangleCount)
        std::size_t targetTriangleCount{0U};

        /// Target reduction percentage 0-100 (used when terminationMode == TargetReductionPercent)
        float targetReductionPercent{50.0F};

        /// Maximum error for edge collapse (used when terminationMode == ErrorBounded,
        /// also as a hard cap in other modes). In squared world units.
        float maxError{std::numeric_limits<float>::max()};

        /// Threshold for flip detection: minimum dot product between old and new normals
        float flipThreshold{0.2F};

        /// How often to check for cancellation (every N collapses)
        std::uint32_t cancelCheckPeriod{16U};
    };

    /// Simplify an indexed triangle set using fast greedy QEM.
    ///
    /// @param positions  IN/OUT vertex positions
    /// @param indices    IN/OUT triangle indices (3 per triangle)
    /// @param config     Simplification parameters
    /// @param throwOnCancel  Called periodically; throw to cancel
    /// @param progressFn     Called with progress 0-100
    /// @return Number of edges collapsed
    std::size_t fastQemSimplify(
        std::vector<Eigen::Vector3f> & positions,
        std::vector<std::uint32_t> & indices,
        FastQemConfig const & config,
        std::function<void()> throwOnCancel = nullptr,
        std::function<void(int)> progressFn = nullptr);
}
```

**Preconditions**:
- `positions` is non-empty
- `indices.size()` is a multiple of 3
- Input mesh should be manifold for manifoldness-preserving guarantees

**Postconditions**:
- `positions` and `indices` are compacted (no unused vertices, no degenerate triangles)
- If input was manifold and watertight, output is manifold and watertight
- Triangle count is ≤ target (or as close as quality constraints allow)
- The function is exception-safe: if `throwOnCancel` throws, positions/indices may be in a partially-simplified state but still represent a valid mesh

## Contract 2: Extended SimplificationMethod Enum (io namespace)

```cpp
namespace gladius::io
{
    enum class SimplificationMethod
    {
        None,           ///< No simplification
        QemFast,        ///< Fast geometric QEM, CPU-only (NEW)
        QemSdfAware     ///< QEM with GPU SDF error evaluation
    };

    enum class SimplificationTerminationMode
    {
        TargetTriangleCount,    ///< Stop at specified triangle count
        TargetReductionPercent, ///< Stop at specified reduction %
        ErrorBounded            ///< Stop when all remaining collapses exceed error threshold
    };
}
```

## Contract 3: Extended SimplificationMethod Enum (compute namespace)

```cpp
namespace gladius::compute
{
    enum class SimplificationMethod
    {
        None,           ///< No simplification
        QemFast,        ///< Fast geometric QEM (NEW)
        QemSdfAware     ///< QEM with GPU SDF evaluation
    };

    enum class SimplificationTerminationMode
    {
        TargetTriangleCount,
        TargetReductionPercent,
        ErrorBounded
    };
}
```

## Contract 4: Extended SurfaceExtractionOptions

New/modified fields in `SurfaceExtractionOptions`:

```cpp
// Existing fields (unchanged):
SimplificationMethod simplificationMethod{SimplificationMethod::None};
std::optional<std::size_t> simplificationTargetTriangles{std::nullopt};
std::optional<float> simplificationTargetReduction{std::nullopt};

// New fields:
SimplificationTerminationMode simplificationTerminationMode{
    SimplificationTerminationMode::TargetReductionPercent};
float simplificationMaxError{std::numeric_limits<float>::max()};  ///< For error-bounded mode
```

## Contract 5: MutablePriorityQueue

```cpp
namespace gladius::compute
{
    /// Mutable min-heap with O(log n) push/pop/update/remove.
    /// Elements must be trivially copyable.
    /// IndexSetter is called with (element, heap_index) to maintain external index mapping.
    template<typename T, typename IndexSetter, typename LessPredicate>
    class MutablePriorityQueue
    {
    public:
        MutablePriorityQueue(IndexSetter indexSetter, LessPredicate less);

        void push(T const & item);
        void pop();
        T & top();
        void remove(std::size_t heapIndex);
        void update(std::size_t heapIndex);

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] bool empty() const;
        T & operator[](std::size_t heapIndex);

        void reserve(std::size_t count);
        void clear();
    };
}
```

## Contract 6: Color Resampling Interface

No new contract needed. Color resampling uses the existing color evaluation pipeline:

```cpp
// Already exists in the export pipeline — positions are evaluated for color
// after mesh extraction. The same call is made after simplification:
//   colorFunction.evaluate(newVertexPositions) → perVertexColors

// The simplification function does NOT handle colors.
// The exporter calls simplification first, then color evaluation on the result.
```

## Integration Sequence

```
Export request (from UI)
  → SurfaceExtractionOptions populated from dialog
  → Mesh extraction (LMC / DC / MDC) → indexed_triangle_set
  → IF simplificationMethod != None:
      → IF QemFast: call fastQemSimplify(positions, indices, config, cancel, progress)
      → IF QemSdfAware: call existing QemMeshSimplifier::simplify(...)
  → IF hasColors:
      → Evaluate color function at final vertex positions
  → Write to file (STL / 3MF)
```
