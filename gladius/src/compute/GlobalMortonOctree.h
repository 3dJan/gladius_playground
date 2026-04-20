#pragma once

/**
 * @file GlobalMortonOctree.h
 * @brief EXPERIMENTAL - Path-based Morton octree for manifold dual contouring.
 * 
 * @warning This implementation is DISABLED by default (enableHierarchicalOctree=false
 * in ManifoldDualContouringGpu.h).
 * 
 * Known limitation: Edge-to-cells mapping fails when neighbor cells don't intersect
 * the surface, which is common at mesh boundaries. This causes non-manifold meshes.
 * 
 * The GPU chunked approach (enabled by default) works correctly and produces
 * watertight manifold meshes suitable for 3D printing.
 * 
 * For development/debugging, define GLOBALMORTON_DEBUG_OUTPUT before including
 * GlobalMortonOctree.cpp to enable verbose debug output.
 */

#include "GlobalVertexRegistry.h"
#include "../types.h"
#include "../kernel/types.h"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class Primitives;
}

namespace gladius::compute
{
    class ManifoldDualContouringProgram;

    /**
     * @brief Edge corner lookup table: which corners each of 12 edges connects.
     */
    constexpr std::array<std::array<std::uint8_t, 2>, 12> EDGE_CORNERS = {{
        {{0U, 1U}}, {{2U, 3U}}, {{4U, 5U}}, {{6U, 7U}},  // X-aligned edges
        {{0U, 2U}}, {{1U, 3U}}, {{4U, 6U}}, {{5U, 7U}},  // Y-aligned edges
        {{0U, 4U}}, {{1U, 5U}}, {{2U, 6U}}, {{3U, 7U}}   // Z-aligned edges
    }};

    /**
     * @brief Edge axis: which axis each edge is aligned with (0=X, 1=Y, 2=Z).
     */
    constexpr std::array<std::uint8_t, 12> EDGE_AXIS = {
        0U, 0U, 0U, 0U,  // X-aligned
        1U, 1U, 1U, 1U,  // Y-aligned
        2U, 2U, 2U, 2U   // Z-aligned
    };

    /**
     * @brief Configuration for global Morton octree construction.
     */
    struct GlobalMortonOctreeConfig
    {
        std::size_t initialDepth{5U};       ///< Initial octree depth (coarse construction)
        std::size_t maxDepth{7U};           ///< Maximum subdivision depth
        float isoValue{0.0F};               ///< ISO surface value
        float minFeatureSize{0.0F};         ///< Minimum feature size to preserve
        bool enableAdaptiveRefinement{true}; ///< Enable curvature-based refinement
        float curvatureThreshold{0.3F};     ///< Gradient variance threshold for subdivision
        std::size_t refinementPasses{2U};   ///< Number of adaptive refinement passes

        /// Optional override for the octree/SDF domain bounding box.
        ///
        /// If not set, the octree will use ComputeCore::getBoundingBox().
        /// When set, the caller must ensure the precomputed SDF buffer corresponds
        /// to the same bounding box, otherwise sampling will be incorrect.
        std::optional<BoundingBox> boundingBoxOverride{std::nullopt};

        // ---- Thickness Field Support ----
        /// Enable shell SDF mode using thickness fields
        bool useThicknessField{false};
        /// 3D outer thickness field buffer (flattened, size = resolution^3)
        std::vector<float> outerThicknessField;
        /// 3D inner thickness field buffer (empty for innermost layer)
        std::vector<float> innerThicknessField;
        /// Resolution of the thickness field grid
        int thicknessFieldResolution{128};
        /// World-to-grid transformation matrix for thickness field sampling
        Eigen::Matrix4f worldToThicknessField = Eigen::Matrix4f::Identity();
        /// True if this is the innermost layer (no inner boundary)
        bool isInnermostLayer{false};
    };

    /**
     * @brief Hermite sample at an edge crossing point.
     */
    struct HermiteSample
    {
        Eigen::Vector3f position;   ///< Position on surface
        Eigen::Vector3f gradient;   ///< Surface gradient (unnormalized normal)
        float value{0.0F};          ///< SDF value (should be ~0)
        std::uint8_t edgeIndex{0U}; ///< Which edge (0-11)
    };

    /// Intermediate QEF solution stored per-node for deferred vertex registration.
    struct ComputedVertex
    {
        Eigen::Vector3f position;
        Eigen::Vector3f normal;
        std::uint8_t component{0U};
    };

    /**
     * @brief Octree node with global Morton indexing.
     * 
     * All nodes use Morton codes derived from the global bounding box,
     * ensuring consistent cell boundaries across the entire domain.
     */
    struct GlobalOctreeNode
    {
        std::uint64_t mortonCode{0U};           ///< Global Morton code
        std::uint8_t depth{0U};                 ///< Depth in octree (0 = root)
        std::uint16_t edgeMask{0U};             ///< Which of 12 edges have zero-crossings
        std::uint8_t internalMask{0U};          ///< Which of 8 corners are inside surface
        bool isLeaf{true};                      ///< True if no children
        bool isIntersecting{false};             ///< True if surface crosses this node
        bool needsRefinement{false};            ///< True if should be subdivided
        float curvatureMetric{0.0F};            ///< Estimated curvature

        std::array<float, 8> cornerValues{};    ///< SDF values at 8 corners
        std::array<std::size_t, 8> childIndices{}; ///< Child node indices (if !isLeaf)

        /// Vertices for this cell (up to 4 for complex manifold configurations)
        std::vector<std::uint32_t> vertexIndices;

        /// Edge→component mapping for multi-vertex cells (used by quad emission).
        /// For each of the 12 local edges, stores which vertex component to use.
        /// Defaults to component 0.
        std::array<std::uint8_t, 12> edgeComponents{};

        /// Edge→component mapping for multi-vertex cells (used by quad emission).
        /// Defaults to component 0.
        std::uint8_t edge3Component{0U};
        std::uint8_t edge7Component{0U};
        std::uint8_t edge11Component{0U};

        /// Hermite samples for QEF solving
        std::vector<HermiteSample> hermiteSamples;

        /// QEF-solved vertex data awaiting registration (used during parallel vertex generation).
        std::vector<ComputedVertex> computedVertices;

        /**
         * @brief Compute bounding box from Morton code and depth.
         */
        [[nodiscard]] BoundingBox computeBounds(Eigen::Vector3f const& globalBboxMin,
                                                 Eigen::Vector3f const& globalBboxSize,
                                                 std::uint32_t maxDepth) const;
    };

    /**
     * @brief Level in the octree (for breadth-first traversal).
     */
    struct OctreeLevel
    {
        std::vector<std::size_t> nodeIndices; ///< Indices into global node array
        std::uint8_t depth{0U};               ///< Depth of this level
    };

    /**
     * @brief Statistics for octree construction.
     */
    struct OctreeStats
    {
        std::size_t totalNodes{0U};
        std::size_t leafNodes{0U};
        std::size_t intersectingLeaves{0U};
        std::size_t vertexCount{0U};
        std::size_t triangleCount{0U};
        std::size_t boundaryEdges{0U};          ///< Should be 0 for watertight mesh
        std::size_t nonManifoldEdges{0U};       ///< Should be 0 for manifold mesh
        double constructionTimeMs{0.0};
        double vertexGenerationTimeMs{0.0};
        double meshExtractionTimeMs{0.0};
    };

    struct MortonNodeKey
    {
        std::uint64_t morton{0U};
        std::uint8_t depth{0U};

        friend bool operator==(MortonNodeKey const& a, MortonNodeKey const& b)
        {
            return a.morton == b.morton && a.depth == b.depth;
        }
    };

    struct MortonNodeKeyHash
    {
        std::size_t operator()(MortonNodeKey const& k) const noexcept
        {
            std::size_t const h0 = std::hash<std::uint64_t>{}(k.morton);
            std::size_t const h1 = std::hash<std::uint8_t>{}(k.depth);
            return h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6U) + (h0 >> 2U));
        }
    };

    /**
     * @brief Global Morton-indexed octree for watertight mesh generation.
     * 
     * This class builds a single octree covering the entire bounding box
     * with consistent cell boundaries. All cells use global Morton codes,
     * ensuring that vertices at identical positions are shared correctly.
     * 
     * Key features:
     * - Level-by-level breadth-first construction
     * - GPU-accelerated corner evaluation and intersection detection
     * - Adaptive refinement based on curvature
     * - Global vertex registry for shared vertices
     * - Watertight mesh extraction with correct winding
     */
    class GlobalMortonOctree
    {
      public:
        explicit GlobalMortonOctree(ComputeCore& core);
        ~GlobalMortonOctree();

        /**
         * @brief Build octree for the current model.
         * 
         * @param config Construction configuration
         */
        void build(GlobalMortonOctreeConfig const& config);

        /**
         * @brief Extract watertight triangle mesh from octree.
         * 
         * @param[out] positions Vertex positions
         * @param[out] normals Vertex normals
         * @param[out] indices Triangle indices
         */
        void extractMesh(std::vector<Eigen::Vector3f>& positions,
                         std::vector<Eigen::Vector3f>& normals,
                         std::vector<std::uint32_t>& indices);

        /**
         * @brief Get octree statistics.
         */
        [[nodiscard]] OctreeStats const& getStats() const
        {
            return m_stats;
        }

        /**
         * @brief Get all octree nodes (for debugging/visualization).
         */
        [[nodiscard]] std::vector<GlobalOctreeNode> const& getNodes() const
        {
            return m_nodes;
        }

        /**
         * @brief Get global vertex registry.
         */
        [[nodiscard]] GlobalVertexRegistry const& getVertexRegistry() const
        {
            return m_vertexRegistry;
        }

        /// Cancellation check callback
        /// @return true if the operation should be cancelled
        using CancellationCheckCallback = std::function<bool()>;

        /**
         * @brief Set cancellation check callback.
         * @param callback Function that returns true if the operation should be cancelled
         */
        void setCancellationCheckCallback(CancellationCheckCallback callback);

        /**
         * @brief Check if the operation was cancelled.
         * @return true if cancelled
         */
        [[nodiscard]] bool wasCancelled() const { return m_wasCancelled; }

      private:
        ComputeCore& m_core;
        ManifoldDualContouringProgram* m_program{nullptr};
        GlobalMortonOctreeConfig m_config;
        OctreeStats m_stats;

        // Global bounding box
        Eigen::Vector3f m_globalBboxMin = Eigen::Vector3f::Zero();
        Eigen::Vector3f m_globalBboxMax = Eigen::Vector3f::Zero();
        Eigen::Vector3f m_globalBboxSize = Eigen::Vector3f::Zero();

        // Octree structure
        std::vector<GlobalOctreeNode> m_nodes;
        std::vector<OctreeLevel> m_levels;
        GlobalVertexRegistry m_vertexRegistry;

        // Morton code → node index lookup
        std::unordered_map<MortonNodeKey, std::size_t, MortonNodeKeyHash> m_mortonToIndex;

        // Phase 1: Initial octree construction
        void buildInitialOctree();
        void processLevel(std::size_t levelIndex, bool forceSubdivision = false);
        void evaluateCornersGpu(std::vector<std::size_t> const& nodeIndices);
        void evaluateCornersCpu(std::vector<std::size_t> const& nodeIndices);
        void detectIntersections(std::vector<std::size_t> const& nodeIndices);
        void createChildNodes(std::size_t parentLevelIndex, bool forceSubdivision = false);

        // Phase 1b: Octree balancing for watertight mesh
        void balanceOctree();
        void ensureNeighborsExist(std::size_t nodeIndex);
        [[nodiscard]] std::size_t createNodeAtCoordinates(std::uint32_t x, std::uint32_t y,
                                                           std::uint32_t z, std::uint8_t depth);
        /// Allocate a node without evaluating its corners (for deferred batch evaluation).
        [[nodiscard]] std::size_t allocateNodeAtCoordinates(std::uint32_t x, std::uint32_t y,
                                                             std::uint32_t z, std::uint8_t depth);
        /// Batch-evaluate corners and classify (intersecting/edge mask) for a set of nodes.
        void evaluateAndClassifyNodes(std::vector<std::size_t> const& nodeIndices);

        // Phase 3b: Halo vertex generation (neighbors that must exist for quad closure)
        void generateHaloVerticesForWatertightness();
        void ensureProjectedVertex(GlobalOctreeNode& node);
        /// Batch Newton-project halo vertices onto the surface using GPU analytical SDF.
        void projectVerticesBatchGpu(std::vector<std::size_t> const& nodeIndices);

        // Phase 2: Adaptive refinement
        void refineAdaptively();
        void estimateCurvatureGpu(std::vector<std::size_t> const& leafIndices);
        void subdivideMarkedLeaves();

        // Phase 3: Vertex generation
        void generateVertices();
        void solveQefForNode(GlobalOctreeNode& node);
        void gatherHermiteSamples(GlobalOctreeNode& node);
        void refineZeroCrossing(Eigen::Vector3f const& start,
                                 Eigen::Vector3f const& end,
                                 float startValue,
                                 float endValue,
                                 Eigen::Vector3f& outPosition);

        // Phase 4: Mesh extraction
        void generateQuads(std::vector<std::uint32_t>& indices);
        void splitNonManifoldEdges(std::vector<Eigen::Vector3f>& positions,
                                   std::vector<Eigen::Vector3f>& normals,
                                   std::vector<std::uint32_t>& indices);
        void fillBoundaryHoles(std::vector<std::uint32_t>& indices,
                               std::vector<Eigen::Vector3f> const& positions);
        void fixTriangleOrientation(std::vector<std::uint32_t>& indices);
        void findSharedEdges();

        // Helper methods
        [[nodiscard]] std::size_t allocateNode();
        [[nodiscard]] Eigen::Vector3f cornerPosition(std::uint8_t cornerIndex,
                                                      BoundingBox const& bounds) const;
        [[nodiscard]] bool hasSignChange(GlobalOctreeNode const& node) const;
        [[nodiscard]] std::uint64_t computeChildMorton(std::uint64_t parentMorton,
                                                        std::uint8_t childIndex,
                                                        std::uint8_t parentDepth) const;
        [[nodiscard]] std::size_t findNeighborNode(std::uint64_t mortonCode,
                                                    int dx, int dy, int dz,
                                                    std::uint8_t depth) const;

        // Morton coordinate helpers
        void decodePathMorton(std::uint64_t mortonCode, std::uint8_t depth,
                              std::uint32_t& x, std::uint32_t& y, std::uint32_t& z) const;
        [[nodiscard]] std::uint64_t encodePathMorton(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t z, std::uint8_t depth) const;

        // SDF evaluation
        [[nodiscard]] float sampleSdf(Eigen::Vector3f const& position) const;
        [[nodiscard]] Eigen::Vector3f sampleGradient(Eigen::Vector3f const& position,
                                                      float epsilon) const;

        // Analytical GPU SDF evaluation (bypasses precomputed voxel grid)
        [[nodiscard]] float sampleSdfAnalytical(Eigen::Vector3f const& position) const;
        [[nodiscard]] Eigen::Vector3f sampleGradientAnalytical(Eigen::Vector3f const& position,
                                                                float epsilon) const;

        /// Batch-evaluate analytical SDF at all corner positions for a set of nodes.
        void evaluateCornersGpuBatch(std::vector<std::size_t> const& nodeIndices);

        /// Batch gather Hermite samples for all intersecting leaves using GPU SDF.
        void gatherHermiteSamplesBatchGpu();


        // Thickness field support
        [[nodiscard]] float sampleThicknessField(std::vector<float> const& field,
                                                  Eigen::Vector3f const& position) const;
        [[nodiscard]] float sampleShellSdf(Eigen::Vector3f const& position) const;
        [[nodiscard]] Eigen::Vector3f sampleShellGradient(Eigen::Vector3f const& position,
                                                           float epsilon) const;

        /// Sample the effective SDF (shell SDF if thickness field enabled, base SDF otherwise)
        [[nodiscard]] float sampleEffectiveSdf(Eigen::Vector3f const& position) const;
        /// Sample the effective gradient (shell gradient if thickness field enabled, base gradient otherwise)
        [[nodiscard]] Eigen::Vector3f sampleEffectiveGradient(Eigen::Vector3f const& position,
                                                               float epsilon) const;
        
        // Cancellation support
        CancellationCheckCallback m_cancellationCheckCallback{nullptr};
        bool m_wasCancelled{false};
        
        /// Check if the operation should be cancelled
        [[nodiscard]] bool isCancelled();
    };
}
