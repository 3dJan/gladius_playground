#pragma once

#include "FastQemSimplification.h"

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
