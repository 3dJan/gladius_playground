#pragma once

/**
 * @file MortonQuadtree.h
 * @brief Morton-encoded quadtree for adaptive slice extraction with thin wall protection.
 * 
 * This implementation adapts the 3D Morton octree approach from surface extraction
 * to 2D slice extraction, enabling reliable detection of 200 μm thin walls while
 * optimizing memory usage through sparse allocation.
 */

#include "../types.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gladius::slicer
{
    /**
     * @brief 2D Morton code for spatial indexing.
     * 
     * Morton codes interleave x and y bits to preserve spatial locality,
     * enabling efficient quadtree traversal and neighbor finding.
     */
    using MortonCode2D = std::uint64_t;

    /**
     * @brief Edge crossing information for contour extraction.
     */
    struct EdgeCrossing2D
    {
        Eigen::Vector2f position;     ///< Position on the surface
        Eigen::Vector2f gradient;     ///< Surface gradient (unnormalized normal)
        float value{0.0F};            ///< SDF value (should be ~0)
        std::uint8_t edgeIndex{0U};   ///< Which edge (0-3: bottom, right, top, left)
    };

    /**
     * @brief Key for node lookup by Morton code and depth.
     * 
     * Spatial Morton codes can collide across depths (e.g., root at depth 0
     * and its first child at depth 1 both have code 0), so we need both
     * morton code and depth for unique identification.
     */
    struct MortonNodeKey
    {
        MortonCode2D morton{0U};
        std::uint8_t depth{0U};

        friend bool operator==(MortonNodeKey const& a, MortonNodeKey const& b)
        {
            return a.morton == b.morton && a.depth == b.depth;
        }
    };

    /**
     * @brief Hash function for MortonNodeKey.
     */
    struct MortonNodeKeyHash
    {
        std::size_t operator()(MortonNodeKey const& k) const noexcept
        {
            std::size_t const h0 = std::hash<MortonCode2D>{}(k.morton);
            std::size_t const h1 = std::hash<std::uint8_t>{}(k.depth);
            return h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6U) + (h0 >> 2U));
        }
    };

    /**
     * @brief Quadtree node with Morton indexing.
     * 
     * All nodes use Morton codes derived from the global bounding box,
     * ensuring consistent cell boundaries across the entire slice domain.
     */
    struct QuadNode
    {
        MortonCode2D mortonCode{0U};          ///< Global Morton code
        std::uint8_t depth{0U};               ///< Depth in quadtree (0 = root)
        std::uint8_t edgeMask{0U};            ///< Which of 4 edges have zero-crossings
        std::uint8_t cornerSignMask{0U};      ///< Bitmask of positive corners (>0)
        bool isLeaf{true};                    ///< True if no children
        bool isIntersecting{false};           ///< True if surface crosses this node
        bool needsRefinement{false};          ///< True if should be subdivided

        /// SDF values at 4 corners (order: bottom-left, bottom-right, top-right, top-left)
        std::array<float, 4> cornerValues{};

        /// Child node indices (if !isLeaf), order: bottom-left, bottom-right, top-right, top-left
        std::array<std::size_t, 4> childIndices{};

        /// Edge crossings for contour extraction
        std::vector<EdgeCrossing2D> edgeCrossings;

        /**
         * @brief Calculate the cell size at this depth.
         * @param rootSize Size of the root cell
         * @return Cell size at this depth
         */
        [[nodiscard]] float calculateCellSize(float rootSize) const
        {
            return rootSize / static_cast<float>(1 << depth);
        }
    };

    /**
     * @brief 2D bounding box for slice domain.
     */
    struct BoundingBox2D
    {
        Eigen::Vector2f min{Eigen::Vector2f::Zero()};
        Eigen::Vector2f max{Eigen::Vector2f::Ones()};

        [[nodiscard]] float getWidth() const { return max.x() - min.x(); }
        [[nodiscard]] float getHeight() const { return max.y() - min.y(); }
        [[nodiscard]] float getMaxExtent() const { return std::max(getWidth(), getHeight()); }
        [[nodiscard]] Eigen::Vector2f getCenter() const { return (min + max) * 0.5F; }

        /**
         * @brief Get bounds of a cell at given grid coordinates and depth.
         * @param x Grid X coordinate
         * @param y Grid Y coordinate
         * @param depth Depth level (determines cell size)
         * @param rootSize Size of the root cell
         * @return Bounding box of the cell
         */
        [[nodiscard]] BoundingBox2D getCellBounds(std::uint32_t x, std::uint32_t y,
                                                   std::uint8_t depth, float rootSize) const
        {
            float const cellSize = rootSize / static_cast<float>(1 << depth);
            return {
                {min.x() + static_cast<float>(x) * cellSize, min.y() + static_cast<float>(y) * cellSize},
                {min.x() + static_cast<float>(x + 1) * cellSize, min.y() + static_cast<float>(y + 1) * cellSize}
            };
        }
    };

    /**
     * @brief Configuration for Morton quadtree construction.
     */
    struct MortonQuadtreeConfig
    {
        std::size_t initialDepth{5U};         ///< Initial quadtree depth (coarse construction)
        std::size_t maxDepth{9U};             ///< Maximum subdivision depth
        float isoValue{0.0F};                 ///< ISO surface value
        float minFeatureSize{0.2F};           ///< Minimum feature size to preserve (mm)
        float minWallThicknessFactor{2.0F};   ///< Multiplier for thin wall protection
        bool enableAdaptiveRefinement{true};  ///< Enable curvature-based refinement
        float curvatureThreshold{0.3F};       ///< Gradient variance threshold for subdivision
        std::size_t refinementPasses{2U};     ///< Number of adaptive refinement passes
        std::size_t maxNodes{1000000U};       ///< Safety limit on total nodes (0 = unlimited)
    };

    /**
     * @brief Morton-encoded quadtree for adaptive slice extraction.
     * 
     * This class implements a 2D quadtree with Morton code indexing,
     * adapted from the 3D Morton octree used in surface extraction.
     * Key features:
     * - Explicit thin wall protection via minFeatureSize constraint
     * - Adaptive refinement based on curvature and feature size
     * - Sparse allocation (only where surface exists)
     * - Watertight contour extraction via consistent cell boundaries
     */
    class MortonQuadtree
    {
    public:
        /**
         * @brief Construct an empty quadtree.
         */
        explicit MortonQuadtree(BoundingBox2D const& bounds);

        /**
         * @brief Build the quadtree with given configuration.
         * @param config Configuration parameters
         */
        void build(MortonQuadtreeConfig const& config);

        /**
         * @brief Perform adaptive refinement based on curvature and feature size.
         * @param config Configuration parameters
         */
        void refineAdaptively(MortonQuadtreeConfig const& config);

        /**
         * @brief Get all leaf nodes that intersect the surface.
         * @return Vector of leaf node indices
         */
        [[nodiscard]] std::vector<std::size_t> getIntersectingLeaves() const;

        /**
         * @brief Get all nodes in the tree.
         * @return Const reference to node vector
         */
        [[nodiscard]] std::vector<QuadNode> const& getNodes() const { return m_nodes; }

        /**
         * @brief Get all nodes in the tree (mutable).
         * 
         * Use for populating corner values from an SDF or other external data.
         * @return Mutable reference to node vector
         */
        [[nodiscard]] std::vector<QuadNode>& getNodesMutable() { return m_nodes; }

        /**
         * @brief Get node by Morton code and depth.
         * @param mortonCode Morton code of the node
         * @param depth Depth of the node
         * @return Pointer to node, or nullptr if not found
         */
        [[nodiscard]] QuadNode const* getNodeByMorton(MortonCode2D mortonCode, std::uint8_t depth) const;

        /**
         * @brief Get the bounding box of the quadtree.
         * @return 2D bounding box
         */
        [[nodiscard]] BoundingBox2D const& getBounds() const { return m_bounds; }

        /**
         * @brief Get the total number of nodes.
         * @return Node count
         */
        [[nodiscard]] std::size_t getNodeCount() const { return m_nodes.size(); }

        /**
         * @brief Get the number of leaf nodes.
         * @return Leaf node count
         */
        [[nodiscard]] std::size_t getLeafCount() const;

        /**
         * @brief Clear all nodes and reset the tree.
         */
        void clear();

    private:
        /**
         * @brief Create the root node.
         */
        void createRootNode();

        /**
         * @brief Subdivide a node into 4 children.
         * @param nodeIndex Index of the node to subdivide
         * @return Indices of the 4 child nodes
         */
        std::array<std::size_t, 4> subdivide(std::size_t nodeIndex);

        /**
         * @brief Check if a node should be subdivided.
         * @param node The node to check
         * @param config Configuration parameters
         * @return True if subdivision is needed
         */
        [[nodiscard]] bool shouldSubdivide(QuadNode const& node,
                                           MortonQuadtreeConfig const& config) const;

        /**
         * @brief Check if node has high curvature (needs refinement).
         * @param node The node to check
         * @param threshold Curvature threshold
         * @return True if curvature is high
         */
        [[nodiscard]] bool hasHighCurvature(QuadNode const& node, float threshold) const;

        BoundingBox2D m_bounds;                                    ///< Bounding box of the tree
        std::vector<QuadNode> m_nodes;                             ///< All nodes in the tree
        std::unordered_map<MortonNodeKey, std::size_t, MortonNodeKeyHash> m_mortonToIndex; ///< (morton, depth) to node index
        MortonQuadtreeConfig m_config;                             ///< Configuration
    };

    // ============================================================================
    // Morton Encoding Functions (inline for performance)
    // ============================================================================

    /**
     * @brief Calculate Morton code from grid coordinates.
     * @param x X coordinate
     * @param y Y coordinate
     * @return Morton code
     */
    inline MortonCode2D mortonEncode(std::uint32_t x, std::uint32_t y)
    {
        auto splitBy2 = [](std::uint32_t a) -> std::uint64_t
        {
            std::uint64_t z = a;
            z = (z | z << 16) & 0x0000ffff0000ffffULL;
            z = (z | z << 8) & 0x00ff00ff00ff00ffULL;
            z = (z | z << 4) & 0x0f0f0f0f0f0f0f0fULL;
            z = (z | z << 2) & 0x3333333333333333ULL;
            z = (z | z << 1) & 0x5555555555555555ULL;
            return z;
        };

        // x bits go in even positions, y bits go in odd positions
        return splitBy2(x) | (splitBy2(y) << 1);
    }

    /**
     * @brief Decode Morton code to grid coordinates.
     * @param mortonCode Morton code
     * @return Pair of (x, y) coordinates
     */
    inline std::pair<std::uint32_t, std::uint32_t> mortonDecode(MortonCode2D mortonCode)
    {
        auto compactBy2 = [](std::uint64_t z) -> std::uint32_t
        {
            z = z & 0x5555555555555555ULL;
            z = (z | z >> 1) & 0x3333333333333333ULL;
            z = (z | z >> 2) & 0x0f0f0f0f0f0f0f0fULL;
            z = (z | z >> 4) & 0x00ff00ff00ff00ffULL;
            z = (z | z >> 8) & 0x0000ffff0000ffffULL;
            z = (z | z >> 16) & 0x00000000ffffffffULL;
            return static_cast<std::uint32_t>(z);
        };

        // x bits are in even positions, y bits are in odd positions
        return {compactBy2(mortonCode), compactBy2(mortonCode >> 1)};
    }

    /**
     * @brief Calculate child Morton codes from parent.
     * 
     * Uses spatial Morton codes where each node's code represents its position
     * in the 2^depth × 2^depth grid. Children are at 2x the parent position.
     * 
     * @param parentMorton Parent Morton code (spatial)
     * @param parentDepth Parent depth
     * @return Array of 4 child Morton codes
     */
    inline std::array<MortonCode2D, 4> calculateChildMortonCodes(
        MortonCode2D parentMorton, std::uint8_t parentDepth)
    {
        // Decode parent position in the 2^parentDepth × 2^parentDepth grid
        auto [parentX, parentY] = mortonDecode(parentMorton);
        
        // Children are at 2x the parent position in the 2^(parentDepth+1) × 2^(parentDepth+1) grid
        // Child offsets: (0,0), (1,0), (0,1), (1,1)
        return {
            mortonEncode(parentX * 2, parentY * 2),         // Bottom-left
            mortonEncode(parentX * 2 + 1, parentY * 2),     // Bottom-right
            mortonEncode(parentX * 2, parentY * 2 + 1),     // Top-left
            mortonEncode(parentX * 2 + 1, parentY * 2 + 1)  // Top-right
        };
    }

} // namespace gladius::slicer
