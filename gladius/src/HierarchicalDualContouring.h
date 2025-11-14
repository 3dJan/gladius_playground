#pragma once

#include "kernel/types.h"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;

    namespace events
    {
        class Logger;
        using SharedLogger = std::shared_ptr<Logger>;
    }
}

namespace gladius::dual_contouring
{
    class QuadraticErrorFunction;
}

namespace gladius::hierarchical_dc
{
    /**
     * @brief Configuration options for hierarchical dual contouring.
     */
    struct HierarchicalConfig
    {
        std::size_t initialDepth{5U};            ///< Coarse octree depth (fast construction)
        std::size_t maxDepth{8U};                ///< Maximum refinement depth
        std::size_t refinementIterations{2U};    ///< Number of adaptive refinement passes
        float curvatureThreshold{0.3F};          ///< Gradient variance threshold for subdivision
        float isoValue{0.0F};                    ///< ISO surface value
        float gradientEpsilon{0.001F};           ///< Finite difference step for gradients (adaptive, scaled by cell size)
        float zeroCrossingTolerance{1e-5F};      ///< Bisection convergence tolerance
        std::size_t maxBisectionIterations{10U}; ///< Max iterations for zero-crossing refinement
        bool enableGpuAcceleration{true};        ///< Use GPU for parallel evaluation
        bool enableCornerCaching{true};          ///< Cache corner values across nodes
        bool enableProgressiveRefinement{true};  ///< Multi-pass adaptive refinement
        bool projectVerticesToSurface{true};     ///< Project QEF vertices onto surface (post-processing)
        std::size_t cpuFallbackResolution{96U};  ///< Resolution of CPU SDF grid used as fallback
        float minFeatureSize{0.0F};              ///< Minimum feature size to preserve (world units); 0 = disabled
        bool enableCoarsening{false};            ///< Enable bottom-up coarsening pass after fine sampling
        float coarseningErrorFactor{0.25F};      ///< Relative error tolerance for merging (fraction of cell size)
        float minWallThicknessFactor{2.0F};      ///< Multiplier for minFeatureSize when protecting thin walls
        float maxNormalDeviationDegrees{25.0F};  ///< Max allowed normal deviation between merged cells
        std::size_t maxNodes{10000000U};         ///< Safety limit on total nodes to prevent OOM (0 = unlimited)
    };

    /// Quality presets for hierarchical dual contouring
    enum class HierarchicalQuality
    {
        Draft,      ///< Fast preview: depth 5, no refinement
        Balanced,   ///< Good quality: depth 7, 1 refinement pass
        Fine,       ///< High detail: depth 8, 2 refinement passes
        UltraFine,  ///< Maximum quality: depth 9, 3 refinement passes
        Custom      ///< User-specified parameters
    };

    /// Apply quality preset to configuration
    void applyQualityPreset(HierarchicalConfig & config, HierarchicalQuality quality);

    /// Edge crossing information for Hermite data
    struct EdgeCrossing
    {
        Eigen::Vector3f startPos;
        Eigen::Vector3f endPos;
        float startValue{0.0F};
        float endValue{0.0F};
        std::size_t nodeIndex{0U};  ///< Which leaf node owns this edge
        std::uint8_t edgeIndex{0U}; ///< Which of 12 edges (0-11)
    };

    /// Hermite sample with position and gradient
    struct HermiteSample
    {
        Eigen::Vector3f position;
        Eigen::Vector3f gradient;
        float value{0.0F};
    };

    /// Octree node in hierarchical structure
    struct OctreeNode
    {
        BoundingBox bounds;                        ///< Spatial extent of this node
        std::array<float, 8> cornerValues{};       ///< SDF values at 8 corners
        std::uint8_t cornerSignMask{0U};           ///< Bitmask of positive corners (>0)
        std::uint8_t cornerZeroMask{0U};           ///< Bitmask of zero-value corners
        std::uint8_t depth{0U};                    ///< Depth in octree (0 = root)
        bool isLeaf{true};                         ///< True if no children
        bool isIntersecting{false};                ///< True if surface crosses this node
        bool needsRefinement{false};               ///< True if should be subdivided
        float curvatureMetric{0.0F};               ///< Estimated curvature (for adaptive refinement)
        std::array<std::size_t, 8> childIndices{}; ///< Indices into node array (if !isLeaf)
        std::optional<Eigen::Vector3f> vertexPosition; ///< QEF-solved vertex (for leaves)
        std::vector<HermiteSample> hermiteSamples; ///< Hermite data collected for this leaf
        Eigen::Vector3f vertexNormal{Eigen::Vector3f::Zero()}; ///< Averaged vertex normal
        float vertexResidual{0.0F};                ///< Residual error from QEF solve
        bool hasVertex{false};                     ///< True when a vertex was solved
    };

    /// Single octree level (for breadth-first traversal)
    struct OctreeLevel
    {
        std::vector<std::size_t> nodeIndices; ///< Indices into global node array
        std::uint8_t depth{0U};               ///< Depth of this level
    };

    /// Statistics for hierarchical construction
    struct ConstructionStats
    {
        std::size_t totalNodes{0U};
        std::size_t leafNodes{0U};
        std::size_t intersectingLeaves{0U};
        std::size_t totalCornerQueries{0U};
        std::size_t totalGradientQueries{0U};
        std::size_t cachedCornerHits{0U};
        std::size_t refinementPasses{0U};
        std::size_t deepestLevel{0U};
        double totalConstructionTimeMs{0.0};
        double gpuTimeMs{0.0};

        void reset()
        {
            *this = ConstructionStats{};
        }
    };

    /// Main hierarchical dual contouring builder
    class HierarchicalOctreeBuilder
    {
      public:
        HierarchicalOctreeBuilder(ComputeCore & core, HierarchicalConfig config);
        ~HierarchicalOctreeBuilder();

        /// Build octree for given bounding box
        void buildOctree(BoundingBox const & bounds);

        /// Get all nodes (for inspection/debugging)
        [[nodiscard]] std::vector<OctreeNode> const & getNodes() const
        {
            return m_nodes;
        }

        /// Get only leaf nodes
        [[nodiscard]] std::vector<std::size_t> getLeafIndices() const;

        /// Get construction statistics
        [[nodiscard]] ConstructionStats const & getStats() const
        {
            return m_stats;
        }

        /// Extract triangle mesh from octree
        void extractMesh(std::vector<Eigen::Vector3f> & outVertices,
                         std::vector<std::uint32_t> & outIndices);

      private:
        struct CpuSampler;

        ComputeCore * m_core{nullptr};
        HierarchicalConfig m_config;
        ConstructionStats m_stats;
        events::SharedLogger m_logger;

        std::vector<OctreeNode> m_nodes;   ///< All octree nodes (flat array)
        std::vector<OctreeLevel> m_levels; ///< Levels for breadth-first traversal
        std::vector<std::size_t> m_freeNodes; ///< Reusable node slots
        std::size_t m_activeNodeCount{0U};
        BoundingBox m_rootBounds;
        std::unique_ptr<CpuSampler> m_cpuSampler;
        bool m_cornerValuesReleased{false};

        // Phase 1: Coarse octree construction
        void buildInitialOctree();
        void processLevel(std::size_t levelIndex);
        void evaluateCorners(std::vector<std::size_t> const & nodeIndices);
        void detectIntersections(std::vector<std::size_t> const & nodeIndices);
        void createChildLevel(std::size_t parentLevelIndex);

        // Phase 2: Adaptive refinement
        void refineAdaptively();
        void estimateCurvature(std::vector<std::size_t> const & leafIndices);
        void subdivideMarkedLeaves();

        // Optional Phase 2b: Bottom-up coarsening after fine sampling
        void coarsenOctree();
        bool tryCoarsenParent(std::size_t parentIndex,
                      dual_contouring::QuadraticErrorFunction & qef);

        // Phase 3: High-precision finishing
        void refineZeroCrossings();
        void solveQEFVertices();
        void releaseHermiteData();
        void releaseCornerValues();
        void compactNodes();

        // GPU acceleration methods
        bool evaluateCornersGPU(std::vector<std::size_t> const & nodeIndices);
        bool estimateCurvatureGPU(std::vector<std::size_t> const & leafIndices);

        // Helper methods
        [[nodiscard]] Eigen::Vector3f cornerPosition(std::uint8_t cornerIndex,
                                                     BoundingBox const & bounds) const;
                [[nodiscard]] bool hasSignChange(OctreeNode const & node) const;
                void gatherEdgeCrossings(std::vector<std::size_t> const & leafIndices,
                                                                 std::vector<EdgeCrossing> & out) const;
                [[nodiscard]] std::vector<HermiteSample> computeHermiteSamples(
                    std::vector<EdgeCrossing> const & crossings,
                    std::vector<Eigen::Vector3f> const & refinedPositions);
                [[nodiscard]] bool cornersHaveOppositeSigns(OctreeNode const & node,
                                                                                                        std::uint8_t cornerA,
                                                                                                        std::uint8_t cornerB) const;

        bool ensureCpuSampler();
        float sampleSdfCpu(Eigen::Vector3f const & position) const;
        Eigen::Vector3f sampleGradientCpu(Eigen::Vector3f const & position,
                                          float epsilon) const;
        std::size_t allocateNode();
        void releaseNode(std::size_t index);

        void logInfo(std::string const & message) const;
        void logError(std::string const & message) const;
    };
}
