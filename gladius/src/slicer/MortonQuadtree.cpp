#include "MortonQuadtree.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include <fmt/format.h>

namespace gladius::slicer
{

    MortonQuadtree::MortonQuadtree(BoundingBox2D const& bounds)
        : m_bounds(bounds)
    {
    }

    void MortonQuadtree::build(MortonQuadtreeConfig const& config)
    {
        m_config = config;
        clear();
        createRootNode();

        // Build initial tree to specified depth
        std::vector<std::size_t> nodesToProcess;
        nodesToProcess.push_back(0);  // Start with root

        for (std::size_t depth = 0; depth < config.initialDepth; ++depth)
        {
            std::vector<std::size_t> nextLevelNodes;

            for (auto nodeIndex : nodesToProcess)
            {
                if (m_nodes[nodeIndex].depth >= config.initialDepth)
                {
                    continue;
                }

                // Subdivide this node
                auto childIndices = subdivide(nodeIndex);

                // Add children to next level
                for (auto childIndex : childIndices)
                {
                    nextLevelNodes.push_back(childIndex);
                }
            }

            nodesToProcess = std::move(nextLevelNodes);

            // Safety check
            if (m_nodes.size() > config.maxNodes && config.maxNodes > 0)
            {
                throw std::runtime_error(
                    fmt::format("MortonQuadtree: exceeded maximum node count ({})", config.maxNodes));
            }
        }
    }

    void MortonQuadtree::refineAdaptively(MortonQuadtreeConfig const& config)
    {
        m_config = config;

        // Perform multiple refinement passes
        for (std::size_t pass = 0; pass < config.refinementPasses; ++pass)
        {
            std::vector<std::size_t> nodesToRefine;

            // Find all nodes that need refinement
            for (std::size_t i = 0; i < m_nodes.size(); ++i)
            {
                if (shouldSubdivide(m_nodes[i], config))
                {
                    nodesToRefine.push_back(i);
                }
            }

            // Subdivide marked nodes
            for (auto nodeIndex : nodesToRefine)
            {
                if (m_nodes[nodeIndex].depth >= config.maxDepth)
                {
                    continue;  // Already at max depth
                }

                subdivide(nodeIndex);

                // Safety check
                if (m_nodes.size() > config.maxNodes && config.maxNodes > 0)
                {
                    throw std::runtime_error(
                        fmt::format("MortonQuadtree: exceeded maximum node count ({})",
                                   config.maxNodes));
                }
            }

            // If no nodes were refined, we're done
            if (nodesToRefine.empty())
            {
                break;
            }
        }
    }

    std::vector<std::size_t> MortonQuadtree::getIntersectingLeaves() const
    {
        std::vector<std::size_t> leaves;
        leaves.reserve(m_nodes.size() / 4);  // Estimate

        for (std::size_t i = 0; i < m_nodes.size(); ++i)
        {
            if (m_nodes[i].isLeaf && m_nodes[i].isIntersecting)
            {
                leaves.push_back(i);
            }
        }

        return leaves;
    }

    QuadNode const* MortonQuadtree::getNodeByMorton(MortonCode2D mortonCode, std::uint8_t depth) const
    {
        auto it = m_mortonToIndex.find(MortonNodeKey{mortonCode, depth});
        if (it != m_mortonToIndex.end())
        {
            return &m_nodes[it->second];
        }
        return nullptr;
    }

    std::size_t MortonQuadtree::getLeafCount() const
    {
        return std::count_if(m_nodes.begin(), m_nodes.end(),
                            [](QuadNode const& node) { return node.isLeaf; });
    }

    void MortonQuadtree::clear()
    {
        m_nodes.clear();
        m_mortonToIndex.clear();
    }

    void MortonQuadtree::createRootNode()
    {
        QuadNode root;
        root.mortonCode = 0U;
        root.depth = 0U;
        root.isLeaf = true;
        root.isIntersecting = false;
        root.needsRefinement = false;

        m_nodes.push_back(root);
        m_mortonToIndex[MortonNodeKey{0U, 0U}] = 0U;
    }

    std::array<std::size_t, 4> MortonQuadtree::subdivide(std::size_t nodeIndex)
    {
        assert(nodeIndex < m_nodes.size());
        
        QuadNode& node = m_nodes[nodeIndex];
        
        if (!node.isLeaf)
        {
            // Already subdivided
            return node.childIndices;
        }

        // Calculate child Morton codes
        auto childMortonCodes = calculateChildMortonCodes(node.mortonCode, node.depth);

        // Store parent depth before modifying the vector (which may reallocate)
        auto const childDepth = static_cast<std::uint8_t>(node.depth + 1);

        // Create 4 child nodes
        std::array<std::size_t, 4> childIndices;
        for (int i = 0; i < 4; ++i)
        {
            QuadNode child;
            child.mortonCode = childMortonCodes[i];
            child.depth = childDepth;
            child.isLeaf = true;
            child.isIntersecting = false;
            child.needsRefinement = false;

            childIndices[i] = m_nodes.size();
            m_nodes.push_back(child);
            m_mortonToIndex[MortonNodeKey{child.mortonCode, child.depth}] = childIndices[i];
        }

        // Update parent node (reference may have been invalidated, so re-fetch)
        m_nodes[nodeIndex].isLeaf = false;
        m_nodes[nodeIndex].childIndices = childIndices;

        return childIndices;
    }

    bool MortonQuadtree::shouldSubdivide(QuadNode const& node,
                                        MortonQuadtreeConfig const& config) const
    {
        // Don't subdivide if already at max depth
        if (node.depth >= config.maxDepth)
        {
            return false;
        }

        // Don't subdivide if not a leaf
        if (!node.isLeaf)
        {
            return false;
        }

        // Calculate cell size
        float const rootSize = m_bounds.getMaxExtent();
        float const cellSize = node.calculateCellSize(rootSize);

        // Even if the cell is not intersecting, the surface might be entirely contained
        // within it (all corners on the same side but surface loops inside).
        // Refine if any corner's |SDF| < cell diagonal — the surface could be nearby.
        if (!node.isIntersecting)
        {
            // Skip near-surface check if corners have not been populated (all default zeros)
            bool const allCornersIdentical =
                (node.cornerValues[0] == node.cornerValues[1]) &&
                (node.cornerValues[1] == node.cornerValues[2]) &&
                (node.cornerValues[2] == node.cornerValues[3]);
            if (allCornersIdentical)
            {
                return false;
            }

            float const diagonal = cellSize * 1.414213562F; // √2
            float minAbsVal = std::numeric_limits<float>::max();
            for (auto const val : node.cornerValues)
            {
                minAbsVal = std::min(minAbsVal, std::abs(val - config.isoValue));
            }
            if (minAbsVal >= diagonal)
            {
                return false; // Surface is definitely not within this cell
            }
            // Surface might be inside — fall through to refine
        }

        // Always subdivide intersecting (or near-surface) nodes below maxDepth

        // Thin wall protection: keep subdividing until cell is small enough
        if (config.minFeatureSize > 0.0F)
        {
            float const allowedSize =
                config.minFeatureSize * std::max(1.0F, config.minWallThicknessFactor);

            if (cellSize <= allowedSize)
            {
                // Already fine enough — only continue if curvature demands it
                return config.enableAdaptiveRefinement &&
                       hasHighCurvature(node, config.curvatureThreshold);
            }
            // Cell still larger than allowedSize — must subdivide
            return true;
        }

        // No minFeatureSize constraint: subdivide all intersecting nodes
        return true;
    }

    bool MortonQuadtree::hasHighCurvature(QuadNode const& node, float threshold) const
    {
        // Check if corner values indicate high curvature
        // This is a simple heuristic: if the gradient varies significantly across the cell,
        // it likely has high curvature and needs refinement
        
        if (node.cornerSignMask == 0U || node.cornerSignMask == 0xFU)
        {
            // All corners have the same sign - no surface crossing
            return false;
        }

        // Calculate gradient variation
        // For a 2D cell, we can estimate curvature from the corner value differences
        float const d1 = std::abs(node.cornerValues[1] - node.cornerValues[0]);  // Bottom edge
        float const d2 = std::abs(node.cornerValues[2] - node.cornerValues[1]);  // Right edge
        float const d3 = std::abs(node.cornerValues[3] - node.cornerValues[2]);  // Top edge
        float const d4 = std::abs(node.cornerValues[0] - node.cornerValues[3]);  // Left edge

        float const maxGradient = std::max({d1, d2, d3, d4});
        float const minGradient = std::min({d1, d2, d3, d4});

        // High curvature if gradient varies significantly
        float const gradientVariation = maxGradient - minGradient;
        
        return gradientVariation > threshold;
    }

    // ============================================================================
    // Neighbor finding
    // ============================================================================

    std::size_t MortonQuadtree::findLeafIndexContaining(
        std::uint32_t gx, std::uint32_t gy, std::uint8_t queryDepth) const
    {
        // Walk from queryDepth up to root, looking for the first leaf that covers (gx, gy).
        for (int d = static_cast<int>(queryDepth); d >= 0; --d)
        {
            auto const shift = static_cast<std::uint32_t>(queryDepth - d);
            std::uint32_t const ax = gx >> shift;
            std::uint32_t const ay = gy >> shift;
            auto const morton = mortonEncode(ax, ay);
            auto it = m_mortonToIndex.find(MortonNodeKey{morton, static_cast<std::uint8_t>(d)});
            if (it != m_mortonToIndex.end() && m_nodes[it->second].isLeaf)
            {
                return it->second;
            }
        }
        return SIZE_MAX;
    }

    QuadNode const* MortonQuadtree::findLeafContaining(
        std::uint32_t gx, std::uint32_t gy, std::uint8_t queryDepth) const
    {
        auto const idx = findLeafIndexContaining(gx, gy, queryDepth);
        return (idx != SIZE_MAX) ? &m_nodes[idx] : nullptr;
    }

    // ============================================================================
    // Recursive subdivision to a target depth
    // ============================================================================

    void MortonQuadtree::subdivideToDepth(std::size_t nodeIndex, std::uint8_t targetDepth)
    {
        if (nodeIndex >= m_nodes.size())
        {
            return;
        }
        if (m_nodes[nodeIndex].depth >= targetDepth)
        {
            return;
        }
        if (m_nodes[nodeIndex].isLeaf)
        {
            subdivide(nodeIndex);
        }
        // After subdivide, node at nodeIndex is no longer a leaf → recurse into children.
        // Re-read childIndices by index (subdivide invalidates references).
        auto const children = m_nodes[nodeIndex].childIndices;
        for (auto childIdx : children)
        {
            subdivideToDepth(childIdx, targetDepth);
        }
    }

    // ============================================================================
    // Balanced surface enforcement
    // ============================================================================

    std::size_t MortonQuadtree::ensureBalancedSurface(MortonQuadtreeConfig const& config)
    {
        std::size_t totalCreated = 0U;
        auto const targetDepth = static_cast<std::uint8_t>(config.maxDepth);
        auto const gridExtent = static_cast<std::uint32_t>(1U << targetDepth);

        // Offsets for 4 face-adjacent neighbors (dx, dy)
        static constexpr std::array<std::pair<int, int>, 4> FACE_OFFSETS = {{
            {-1, 0}, {1, 0}, {0, -1}, {0, 1}
        }};

        bool changed = true;
        while (changed)
        {
            changed = false;
            std::size_t const nodeCountBefore = m_nodes.size();

            // Collect intersecting leaf indices at targetDepth BEFORE modifying the vector.
            std::vector<std::size_t> surfaceLeaves;
            for (std::size_t i = 0U; i < nodeCountBefore; ++i)
            {
                auto const& node = m_nodes[i];
                if (node.isLeaf && node.isIntersecting && node.depth == targetDepth)
                {
                    surfaceLeaves.push_back(i);
                }
            }

            // For each surface leaf, ensure all face-neighbors exist at targetDepth.
            for (auto leafIdx : surfaceLeaves)
            {
                auto const [gx, gy] = mortonDecode(m_nodes[leafIdx].mortonCode);

                for (auto const& [dx, dy] : FACE_OFFSETS)
                {
                    auto const nx = static_cast<int64_t>(gx) + dx;
                    auto const ny = static_cast<int64_t>(gy) + dy;

                    // Bounds check
                    if (nx < 0 || ny < 0 ||
                        nx >= static_cast<int64_t>(gridExtent) ||
                        ny >= static_cast<int64_t>(gridExtent))
                    {
                        continue;
                    }

                    auto const nxu = static_cast<std::uint32_t>(nx);
                    auto const nyu = static_cast<std::uint32_t>(ny);

                    // Check if a leaf already exists at targetDepth
                    auto const neighborMorton = mortonEncode(nxu, nyu);
                    auto it = m_mortonToIndex.find(
                        MortonNodeKey{neighborMorton, targetDepth});
                    if (it != m_mortonToIndex.end() && m_nodes[it->second].isLeaf)
                    {
                        continue; // Already at proper depth
                    }

                    // Find the coarser ancestor leaf covering this position
                    auto const ancestorIdx = findLeafIndexContaining(nxu, nyu, targetDepth);
                    if (ancestorIdx == SIZE_MAX)
                    {
                        continue; // Out of tree bounds
                    }

                    if (m_nodes[ancestorIdx].depth >= targetDepth)
                    {
                        continue; // Already sufficiently refined
                    }

                    // Subdivide the ancestor down to targetDepth
                    subdivideToDepth(ancestorIdx, targetDepth);
                    changed = true;

                    // Safety limit
                    if (config.maxNodes > 0U && m_nodes.size() > config.maxNodes)
                    {
                        throw std::runtime_error(fmt::format(
                            "MortonQuadtree::ensureBalancedSurface: exceeded max nodes ({})",
                            config.maxNodes));
                    }
                }
            }

            totalCreated += m_nodes.size() - nodeCountBefore;
        }

        return totalCreated;
    }

} // namespace gladius::slicer
