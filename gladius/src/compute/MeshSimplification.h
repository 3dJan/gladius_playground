#pragma once

#include "../types.h"

#include <Eigen/Core>
#include <Eigen/Dense>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace gladius::compute
{
    /// Termination criterion for fast QEM simplification
    enum class SimplificationTerminationMode
    {
        TargetTriangleCount,    ///< Stop at specified triangle count
        TargetReductionPercent, ///< Stop at specified reduction percentage
        ErrorBounded            ///< Stop when all remaining collapses exceed error threshold
    };

    /// Configuration for fast geometric QEM simplification (CPU-only, single-pass)
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

    /// Double-precision 4x4 symmetric matrix for Quadric Error Metrics.
    /// Stores 10 unique elements of the upper triangle:
    /// [m0  m1  m2  m3]
    /// [    m4  m5  m6]
    /// [        m7  m8]
    /// [            m9]
    struct SymMat
    {
        double m[10]{};

        SymMat() = default;

        /// Construct from all 10 elements
        SymMat(double m0, double m1, double m2, double m3,
               double m4, double m5, double m6,
               double m7, double m8,
               double m9)
            : m{m0, m1, m2, m3, m4, m5, m6, m7, m8, m9}
        {
        }

        /// Construct from plane equation ax + by + cz + d = 0 as outer product p*p^T
        static SymMat fromPlane(double a, double b, double c, double d)
        {
            return {a * a, a * b, a * c, a * d,
                           b * b, b * c, b * d,
                                  c * c, c * d,
                                         d * d};
        }

        SymMat & operator+=(SymMat const & rhs)
        {
            for (int i = 0; i < 10; ++i)
            {
                m[i] += rhs.m[i];
            }
            return *this;
        }

        SymMat operator+(SymMat const & rhs) const
        {
            auto result = *this;
            result += rhs;
            return result;
        }

        /// Determinant of a 3x3 sub-matrix formed by selecting rows/cols from the 4x4 matrix.
        /// Row i of the 4x4 matrix is indexed as: (r0,c0), (r0,c1), (r0,c2); etc.
        /// Indices reference the flat m[10] array via the index accessor.
        [[nodiscard]] double det(int a11, int a12, int a13,
                                 int a21, int a22, int a23,
                                 int a31, int a32, int a33) const
        {
            return m[a11] * (m[a22] * m[a33] - m[a23] * m[a32])
                 - m[a12] * (m[a21] * m[a33] - m[a23] * m[a31])
                 + m[a13] * (m[a21] * m[a32] - m[a22] * m[a31]);
        }

        /// Evaluate quadric error at point v: v^T * Q * v (homogeneous: w=1)
        [[nodiscard]] double evaluate(Eigen::Vector3f const & v) const
        {
            double const x = v.x();
            double const y = v.y();
            double const z = v.z();
            return x * x * m[0] + 2.0 * x * y * m[1] + 2.0 * x * z * m[2] + 2.0 * x * m[3]
                 + y * y * m[4] + 2.0 * y * z * m[5] + 2.0 * y * m[6]
                 + z * z * m[7] + 2.0 * z * m[8]
                 + m[9];
        }

        /// Find optimal vertex position that minimizes quadric error.
        /// Returns nullopt if the 3x3 sub-matrix is singular.
        [[nodiscard]] std::optional<Eigen::Vector3f> optimalVertex() const
        {
            // Solve: A * v = -b  where A = top-left 3x3, b = top-right 3x1
            double const d = det(0, 1, 2, 1, 4, 5, 2, 5, 7);
            if (std::abs(d) < 1e-15)
            {
                return std::nullopt;
            }
            double const invD = 1.0 / d;
            double const x = -invD * det(3, 1, 2, 6, 4, 5, 8, 5, 7);
            double const y = -invD * det(0, 3, 2, 1, 6, 5, 2, 8, 7);
            double const z = -invD * det(0, 1, 3, 1, 4, 6, 2, 5, 8);
            return Eigen::Vector3f(static_cast<float>(x),
                                   static_cast<float>(y),
                                   static_cast<float>(z));
        }
    };

    /// Mutable min-heap with O(log n) push/pop/update/remove.
    /// IndexSetter is called as indexSetter(element&, heap_index) to let elements track their position.
    /// LessPredicate compares two elements; returns true if the first is "less" (higher priority).
    template<typename T, typename IndexSetter, typename LessPredicate>
    class MutablePriorityQueue
    {
      public:
        MutablePriorityQueue(IndexSetter indexSetter, LessPredicate less)
            : m_indexSetter(std::move(indexSetter))
            , m_less(std::move(less))
        {
        }

        void reserve(std::size_t count) { m_heap.reserve(count); }
        void clear() { m_heap.clear(); }
        [[nodiscard]] std::size_t size() const { return m_heap.size(); }
        [[nodiscard]] bool empty() const { return m_heap.empty(); }
        T & top() { return m_heap.front(); }
        T const & top() const { return m_heap.front(); }
        T & operator[](std::size_t heapIndex) { return m_heap[heapIndex]; }

        void push(T const & item)
        {
            m_heap.push_back(item);
            m_indexSetter(m_heap.back(), m_heap.size() - 1U);
            bubbleUp(m_heap.size() - 1U);
        }

        void pop()
        {
            if (m_heap.size() > 1U)
            {
                swapElements(0U, m_heap.size() - 1U);
                m_heap.pop_back();
                if (!m_heap.empty())
                {
                    bubbleDown(0U);
                }
            }
            else
            {
                m_heap.pop_back();
            }
        }

        void remove(std::size_t heapIndex)
        {
            if (heapIndex >= m_heap.size())
            {
                return;
            }
            if (heapIndex == m_heap.size() - 1U)
            {
                m_heap.pop_back();
                return;
            }
            swapElements(heapIndex, m_heap.size() - 1U);
            m_heap.pop_back();
            if (heapIndex < m_heap.size())
            {
                bubbleUp(heapIndex);
                bubbleDown(heapIndex);
            }
        }

        void update(std::size_t heapIndex)
        {
            if (heapIndex >= m_heap.size())
            {
                return;
            }
            bubbleUp(heapIndex);
            bubbleDown(heapIndex);
        }

      private:
        std::vector<T> m_heap;
        IndexSetter m_indexSetter;
        LessPredicate m_less;

        void swapElements(std::size_t a, std::size_t b)
        {
            std::swap(m_heap[a], m_heap[b]);
            m_indexSetter(m_heap[a], a);
            m_indexSetter(m_heap[b], b);
        }

        void bubbleUp(std::size_t idx)
        {
            while (idx > 0U)
            {
                std::size_t const parent = (idx - 1U) / 2U;
                if (m_less(m_heap[idx], m_heap[parent]))
                {
                    swapElements(idx, parent);
                    idx = parent;
                }
                else
                {
                    break;
                }
            }
        }

        void bubbleDown(std::size_t idx)
        {
            std::size_t const sz = m_heap.size();
            for (;;)
            {
                std::size_t const left = 2U * idx + 1U;
                std::size_t const right = 2U * idx + 2U;
                std::size_t smallest = idx;
                if (left < sz && m_less(m_heap[left], m_heap[smallest]))
                {
                    smallest = left;
                }
                if (right < sz && m_less(m_heap[right], m_heap[smallest]))
                {
                    smallest = right;
                }
                if (smallest == idx)
                {
                    break;
                }
                swapElements(idx, smallest);
                idx = smallest;
            }
        }
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

    /// Configuration for QEM-based mesh simplification with GPU SDF error evaluation
    struct QemSimplificationConfig
    {
        // Error metric weights (should sum to ~1.0 for normalized comparison)
        float sdfErrorWeight{0.5F};              ///< Weight for GPU-evaluated SDF deviation error
        float qemErrorWeight{0.2F};              ///< Weight for quadric error metric
        float normalDeviationWeight{0.3F};       ///< Weight for triangle normal vs SDF gradient deviation
        
        // Error bounds
        float maxSdfError{0.01F};                ///< Maximum allowed SDF deviation (world units)
        float maxQemError{1e-4F};                ///< Maximum allowed QEM error (squared distance)
        float maxNormalDeviation{0.5F};          ///< Maximum allowed normal deviation (1 - dot product, 0.5 = ~60°)
        
        // Edge length constraint
        float maxEdgeLengthRatio{2.5F};          ///< Max ratio of new edge length to neighbor average (prevents long thin triangles)
        
        // Multi-point SDF sampling for curved surfaces
        std::size_t edgeSdfSampleCount{5U};      ///< Number of points to sample along edge for SDF error
        float edgeSdfErrorWeight{0.3F};          ///< Weight for edge SDF error (max deviation along edge)
        
        // Batch processing for GPU efficiency
        std::size_t batchSize{100000U};          ///< Number of edges to evaluate per GPU batch
        
        // Feature preservation
        float sharpEdgeAngleThreshold{0.7F};     ///< Cosine of angle threshold for sharp edges (0.7 ≈ 45°)
        float boundaryEdgeLockFactor{1e6F};      ///< Collapse cost multiplier for boundary edges (watertight preservation)
        
        // Termination criteria (priority: error bound > triangle count > percentage)
        std::optional<std::size_t> targetTriangleCount{std::nullopt};  ///< Stop when triangle count reached
        std::optional<float> targetReductionPercent{std::nullopt};     ///< Stop when reduction percentage reached
        
        // Quality settings
        bool recalculateQuadricsAfterCollapse{true};  ///< Recalculate quadrics exactly (slower but more accurate)
        std::size_t maxPasses{10U};                   ///< Maximum simplification passes
    };

    /// 4x4 symmetric matrix for Quadric Error Metrics, stored as 10 unique floats
    /// Represents the quadric Q = [A b; b^T c] where A is 3x3, b is 3x1, c is scalar
    /// Error at point v: v^T * Q * v = v^T*A*v + 2*b^T*v + c
    struct Quadric
    {
        // Upper triangle of 4x4 symmetric matrix stored in row-major order:
        // [a00 a01 a02 a03]     [0  1  2  3]
        // [    a11 a12 a13]  =  [   4  5  6]
        // [        a22 a23]     [      7  8]
        // [            a33]     [         9]
        float data[10]{0.0F};

        Quadric() = default;

        /// Create quadric from plane equation: ax + by + cz + d = 0
        /// Plane normal (a,b,c) should be normalized
        static Quadric fromPlane(Eigen::Vector3f const & normal, float d);

        /// Create quadric from triangle vertices
        static Quadric fromTriangle(Eigen::Vector3f const & v0,
                                    Eigen::Vector3f const & v1,
                                    Eigen::Vector3f const & v2);

        /// Add another quadric to this one
        Quadric & operator+=(Quadric const & other);
        
        /// Add two quadrics
        Quadric operator+(Quadric const & other) const;

        /// Evaluate error at a point: v^T * Q * v (extended to homogeneous coords)
        [[nodiscard]] float evaluate(Eigen::Vector3f const & v) const;

        /// Find optimal vertex position that minimizes error
        /// Returns nullopt if matrix is singular (uses SVD internally)
        [[nodiscard]] std::optional<Eigen::Vector3f> optimalVertex() const;

        /// Get the 3x3 A matrix (top-left block)
        [[nodiscard]] Eigen::Matrix3f getA() const;

        /// Get the b vector (top-right 3x1 block, same as left 1x3 block)
        [[nodiscard]] Eigen::Vector3f getB() const;

        /// Get the c scalar (bottom-right element)
        [[nodiscard]] float getC() const;

        /// Reset to zero quadric
        void reset();
    };

    /// Edge collapse candidate with computed error metrics
    struct CollapseCandidate
    {
        std::uint32_t vertexA{0U};           ///< First vertex of edge
        std::uint32_t vertexB{0U};           ///< Second vertex of edge  
        Eigen::Vector3f targetPosition;       ///< Optimal collapse position
        float qemError{0.0F};                 ///< Quadric error at target position
        float sdfError{0.0F};                 ///< SDF deviation at target position (filled by GPU)
        float edgeSdfError{0.0F};            ///< Maximum SDF deviation along edge (for curved surfaces)
        float normalDeviation{0.0F};          ///< Max triangle normal vs SDF gradient deviation (filled by GPU)
        float combinedError{0.0F};            ///< Weighted combination of errors
        float edgeLength{0.0F};              ///< Length of this edge
        float maxNeighborEdgeLength{0.0F};   ///< Maximum length of neighbor edges
        bool isBoundaryEdge{false};           ///< True if edge is on mesh boundary
        bool isSharpFeatureEdge{false};       ///< True if edge is on a sharp feature
        bool isValid{true};                   ///< False if candidate was invalidated by previous collapse
    };

    /// Progress callback for simplification
    using SimplificationProgressCallback = std::function<void(std::size_t currentTriangles,
                                                               std::size_t targetTriangles,
                                                               std::size_t collapsedEdges)>;

    /// GPU SDF evaluation function type
    /// Takes array of positions, returns array of SDF values
    using GpuSdfEvaluator = std::function<std::vector<float>(std::vector<Eigen::Vector3f> const & positions)>;

    /// GPU SDF gradient evaluation function type
    /// Takes array of positions, returns array of normalized gradient vectors (surface normals)
    using GpuSdfGradientEvaluator = std::function<std::vector<Eigen::Vector3f>(std::vector<Eigen::Vector3f> const & positions)>;

    /// QEM-based mesh simplifier with GPU SDF error evaluation
    class QemMeshSimplifier
    {
      public:
        QemMeshSimplifier() = default;

        /// Set configuration
        void setConfig(QemSimplificationConfig const & config);

        /// Set GPU SDF evaluation function (must be set before simplify())
        void setGpuSdfEvaluator(GpuSdfEvaluator evaluator);

        /// Set GPU SDF gradient evaluation function (must be set before simplify() for normal deviation check)
        void setGpuSdfGradientEvaluator(GpuSdfGradientEvaluator evaluator);

        /// Set progress callback (optional)
        void setProgressCallback(SimplificationProgressCallback callback);

        /// Simplify mesh in-place
        /// Returns number of edges collapsed
        std::size_t simplify(std::vector<Eigen::Vector3f> & positions,
                             std::vector<Eigen::Vector3f> & normals,
                             std::vector<std::uint32_t> & indices);

      private:
        QemSimplificationConfig m_config{};
        GpuSdfEvaluator m_gpuSdfEvaluator{nullptr};
        GpuSdfGradientEvaluator m_gpuSdfGradientEvaluator{nullptr};
        SimplificationProgressCallback m_progressCallback{nullptr};

        // Per-vertex quadrics
        std::vector<Quadric> m_vertexQuadrics;

        // Build vertex quadrics from mesh triangles
        void buildVertexQuadrics(std::vector<Eigen::Vector3f> const & positions,
                                 std::vector<std::uint32_t> const & indices);

        // Recalculate quadric for a single vertex from its incident triangles
        void recalculateVertexQuadric(std::uint32_t vertexIndex,
                                      std::vector<Eigen::Vector3f> const & positions,
                                      std::vector<std::uint32_t> const & indices,
                                      std::vector<std::vector<std::size_t>> const & vertexToTriangles);

        // Collect all edge collapse candidates
        std::vector<CollapseCandidate> collectCandidates(
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<Eigen::Vector3f> const & normals,
            std::vector<std::uint32_t> const & indices) const;

        // Evaluate SDF errors for a batch of candidates using GPU
        void evaluateSdfErrorsGpu(std::vector<CollapseCandidate> & candidates,
                                  std::vector<Eigen::Vector3f> const & positions,
                                  std::vector<std::uint32_t> const & indices,
                                  std::vector<std::vector<std::size_t>> const & vertexToTriangles);

        // Perform a single edge collapse
        bool performCollapse(CollapseCandidate const & candidate,
                             std::vector<Eigen::Vector3f> & positions,
                             std::vector<Eigen::Vector3f> & normals,
                             std::vector<std::uint32_t> & indices,
                             std::vector<std::vector<std::size_t>> & vertexToTriangles,
                             std::vector<bool> & vertexRemoved);

        // Check if collapse would create degenerate or inverted triangles
        [[nodiscard]] bool wouldCreateDegenerateTriangles(
            CollapseCandidate const & candidate,
            std::vector<Eigen::Vector3f> const & positions,
            std::vector<std::uint32_t> const & indices,
            std::vector<std::vector<std::size_t>> const & vertexToTriangles) const;

        // Compact mesh by removing unused vertices and degenerate triangles
        void compactMesh(std::vector<Eigen::Vector3f> & positions,
                         std::vector<Eigen::Vector3f> & normals,
                         std::vector<std::uint32_t> & indices,
                         std::vector<bool> const & vertexRemoved);
    };

} // namespace gladius::compute
