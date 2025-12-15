#include "GlobalMortonOctree.h"
#include "ManifoldDualContouringProgram.h"
#include "ComputeCore.h"
#include "../Primitives.h"
#include "../DualContouringQef.h"
#include "../ResourceContext.h"

#include <Eigen/Geometry>
#include <Eigen/SVD>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_set>

// NOTE: This GlobalMortonOctree implementation is EXPERIMENTAL and DISABLED by default.
// It uses path-based Morton codes to build a hierarchical octree structure.
// Currently disabled because edge-to-cells mapping fails when neighbor cells don't
// intersect the surface (common at boundaries), causing non-manifold meshes.
// The GPU chunked approach (used when enableHierarchicalOctree=false) works correctly.
// To enable verbose debug output during development, define GLOBALMORTON_DEBUG_OUTPUT.
// #define GLOBALMORTON_DEBUG_OUTPUT

namespace gladius::compute
{
    namespace
    {
        /// Maximum bisection iterations for zero-crossing refinement
        constexpr std::size_t MAX_BISECTION_ITERATIONS = 10U;

        /// Tolerance for zero-crossing refinement
        constexpr float ZERO_CROSSING_TOLERANCE = 1e-5F;

        [[nodiscard]] bool hasEdgeCrossing(float const v0, float const v1)
        {
            // Tolerance-consistent edge-crossing detection.
            // The previous implementation treated any near-zero corner as a crossing, even
            // when both corners were on the same side. At higher depths this can create
            // spurious Hermite samples/quads and lead to non-manifold edge overuse.
            //
            // We classify corners with a small negative tolerance as "inside".
            bool const inside0 = (v0 < -ZERO_CROSSING_TOLERANCE);
            bool const inside1 = (v1 < -ZERO_CROSSING_TOLERANCE);
            return inside0 != inside1;
        }
    }

    BoundingBox GlobalOctreeNode::computeBounds(Eigen::Vector3f const& globalBboxMin,
                                                  Eigen::Vector3f const& globalBboxSize,
                                                  std::uint32_t /* maxDepth */) const
    {
        // Interpret Morton code as a path of octant indices
        // Each 3-bit group encodes an octant: bits [z,y,x]
        // Octant 0=(0,0,0), 1=(1,0,0), 2=(0,1,0), 3=(1,1,0), 4=(0,0,1), etc.
        
        std::uint32_t ix = 0U;
        std::uint32_t iy = 0U;
        std::uint32_t iz = 0U;
        
        // Extract octant indices for each depth level
        for (std::uint8_t d = 0U; d < depth; ++d)
        {
            // Get the octant index at level d (from MSB to LSB)
            auto const shift = (depth - 1U - d) * 3U;
            auto const octant = static_cast<std::uint8_t>((mortonCode >> shift) & 0x7U);
            
            // Each octant index encodes position as: bit0=x, bit1=y, bit2=z
            auto const ox = (octant >> 0U) & 1U;
            auto const oy = (octant >> 1U) & 1U;
            auto const oz = (octant >> 2U) & 1U;
            
            // Accumulate position at this level
            auto const levelScale = 1U << (depth - 1U - d);
            ix += ox * levelScale;
            iy += oy * levelScale;
            iz += oz * levelScale;
        }

        // Cell bounds at this depth.
        // IMPORTANT: Use a numerically consistent formulation for min/max so that
        // neighboring cells share *exactly* the same corner/face coordinates.
        // Computing max as (min + cellSize) can diverge (by a few ulps) from
        // computing neighbor.min as (globalMin + (i+1)*cellSize), which in turn can
        // produce inconsistent corner sampling and edge masks at higher depths.
        double const cellsPerAxis = static_cast<double>(1U << depth);
        Eigen::Array3d const globalMin = globalBboxMin.cast<double>().array();
        Eigen::Array3d const globalSize = globalBboxSize.cast<double>().array();
        Eigen::Array3d const cellSize = globalSize / cellsPerAxis;

        Eigen::Array3d const minD = globalMin + Eigen::Array3d(static_cast<double>(ix), static_cast<double>(iy), static_cast<double>(iz)) * cellSize;
        Eigen::Array3d const maxD = globalMin + Eigen::Array3d(static_cast<double>(ix + 1U), static_cast<double>(iy + 1U), static_cast<double>(iz + 1U)) * cellSize;

        BoundingBox bounds;
        bounds.min.s[0] = static_cast<float>(minD.x());
        bounds.min.s[1] = static_cast<float>(minD.y());
        bounds.min.s[2] = static_cast<float>(minD.z());
        bounds.min.s[3] = 0.0F;

        bounds.max.s[0] = static_cast<float>(maxD.x());
        bounds.max.s[1] = static_cast<float>(maxD.y());
        bounds.max.s[2] = static_cast<float>(maxD.z());
        bounds.max.s[3] = 0.0F;

        return bounds;
    }

    GlobalMortonOctree::GlobalMortonOctree(ComputeCore& core)
        : m_core(core)
    {
    }

    GlobalMortonOctree::~GlobalMortonOctree() = default;

    void GlobalMortonOctree::build(GlobalMortonOctreeConfig const& config)
    {
        auto const startTime = std::chrono::high_resolution_clock::now();

        m_config = config;
        m_stats = OctreeStats{};
        m_nodes.clear();
        m_levels.clear();
        m_mortonToIndex.clear();

        // Get the program from ProgramManager
        auto& programManager = m_core.getProgramManager();
        m_program = programManager.getManifoldDualContouringProgram();
        if (!m_program)
        {
            std::cerr << "ManifoldDualContouringProgram not available" << std::endl;
            return;
        }

        // Get global bounding box
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available" << std::endl;
            return;
        }

        m_globalBboxMin = Eigen::Vector3f(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        m_globalBboxMax = Eigen::Vector3f(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        m_globalBboxSize = m_globalBboxMax - m_globalBboxMin;

        // Initialize vertex registry with global bounds
        m_vertexRegistry.initialize(m_globalBboxMin, m_globalBboxMax,
                                     static_cast<std::uint32_t>(config.maxDepth));

        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Building GlobalMortonOctree:" << std::endl;
        std::cout << "  BBox: [" << m_globalBboxMin.transpose() << "] to ["
                  << m_globalBboxMax.transpose() << "]" << std::endl;
        std::cout << "  Depth range: " << config.initialDepth << " to " << config.maxDepth << std::endl;

#endif
        // Phase 1: Build initial coarse octree
        buildInitialOctree();

        // Phase 1b: Balance octree for watertight mesh generation
        // Ensures all intersecting cells have face-adjacent neighbors at the same depth
        balanceOctree();

        // Phase 2: Adaptive refinement
        if (config.enableAdaptiveRefinement && config.refinementPasses > 0U)
        {
            refineAdaptively();
            // Re-balance after adaptive refinement
            balanceOctree();
        }

        // Phase 3: Generate vertices using QEF
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Generating vertices..." << std::endl;
        #endif
        generateVertices();

        // Phase 3b: Generate "halo" vertices for non-intersecting neighbors that are required
        // to close owned-edge quads. This is the CPU analogue of the GPU halo approach.
        generateHaloVerticesForWatertightness();
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Vertices generated: " << m_stats.vertexCount << std::endl;

#endif
        auto const endTime = std::chrono::high_resolution_clock::now();
        m_stats.constructionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Octree construction complete:" << std::endl;
        std::cout << "  Total nodes: " << m_stats.totalNodes << std::endl;
        std::cout << "  Leaf nodes: " << m_stats.leafNodes << std::endl;
        std::cout << "  Intersecting leaves: " << m_stats.intersectingLeaves << std::endl;
        std::cout << "  Vertices: " << m_stats.vertexCount << std::endl;
        std::cout << "  Time: " << m_stats.constructionTimeMs << " ms" << std::endl;
        #endif
    }

    void GlobalMortonOctree::buildInitialOctree()
    {
        // Initialize levels array
        m_levels.resize(m_config.maxDepth + 1U);
        for (std::size_t d = 0U; d <= m_config.maxDepth; ++d)
        {
            m_levels[d].depth = static_cast<std::uint8_t>(d);
        }

        // Create root node
        std::size_t const rootIndex = allocateNode();
        m_nodes[rootIndex].mortonCode = 0U;
        m_nodes[rootIndex].depth = 0U;
        m_levels[0].nodeIndices.push_back(rootIndex);

        // Process level by level until initial depth
        // Force subdivision of ALL nodes during initial phase (forceSubdivision=true)
        for (std::size_t depth = 0U; depth < m_config.initialDepth; ++depth)
        {
            processLevel(depth, true);
        }

        // Process remaining levels only for intersecting nodes (forceSubdivision=false)
        // Include maxDepth to evaluate and mark intersections on leaf nodes
        for (std::size_t depth = m_config.initialDepth; depth <= m_config.maxDepth; ++depth)
        {
            if (m_levels[depth].nodeIndices.empty())
            {
                break;
            }
            processLevel(depth, false);
        }

        // Count final statistics
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Counting statistics for " << m_nodes.size() << " nodes..." << std::endl;
        #endif
        for (auto const& node : m_nodes)
        {
            if (node.isLeaf)
            {
                ++m_stats.leafNodes;
                if (node.isIntersecting)
                {
                    ++m_stats.intersectingLeaves;
                }
            }
        }
        m_stats.totalNodes = m_nodes.size();
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Statistics complete: " << m_stats.leafNodes << " leaves, " 
                  << m_stats.intersectingLeaves << " intersecting" << std::endl;
                  #endif
    }

    void GlobalMortonOctree::processLevel(std::size_t levelIndex, bool forceSubdivision)
    {
        auto& level = m_levels[levelIndex];
        if (level.nodeIndices.empty())
        {
            return;
        }

        // Evaluate corners
        evaluateCornersCpu(level.nodeIndices);

        // Detect intersections
        detectIntersections(level.nodeIndices);

        // Create children for intersecting nodes (if not at max depth)
        // During initial build (forceSubdivision=true), subdivide ALL nodes
        if (levelIndex < m_config.maxDepth)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "    Creating children for level " << levelIndex << "..." << std::flush;
            #endif
            createChildNodes(levelIndex, forceSubdivision);
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << " done" << std::endl << std::flush;
            #endif
        }
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Level " << levelIndex << " processing complete. Nodes in next level: " 
                  << (levelIndex < m_levels.size() - 1U ? m_levels[levelIndex + 1U].nodeIndices.size() : 0U) << std::endl << std::flush;
                  #endif
    }

    void GlobalMortonOctree::evaluateCornersCpu(std::vector<std::size_t> const& nodeIndices)
    {
        // Debug: Check SDF buffer status
        auto resources = m_core.getResourceContext();
        if (resources)
        {
            auto& sdfBuffer = resources->getPrecompSdfBuffer();
            
            // Read SDF data from GPU to CPU
            sdfBuffer.read();
            
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  SDF buffer size: " << sdfBuffer.getWidth() << "x" 
                      << sdfBuffer.getHeight() << "x" << sdfBuffer.getDepth()
                      << " (data size: " << sdfBuffer.getData().size() << ")" << std::endl;
            
            #endif
            // Print min/max of SDF data for debugging
            auto const& data = sdfBuffer.getData();
            if (!data.empty())
            {
                float minVal = data[0];
                float maxVal = data[0];
                for (float v : data)
                {
                    minVal = std::min(minVal, v);
                    maxVal = std::max(maxVal, v);
                }
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  SDF value range: [" << minVal << ", " << maxVal << "]" << std::endl;
                #endif
            }
            else
            {
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  SDF buffer is EMPTY!" << std::endl;
                #endif
            }
        }
        else
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  No resources available for SDF sampling!" << std::endl;
            #endif
        }

        for (std::size_t nodeIdx : nodeIndices)
        {
            auto& node = m_nodes[nodeIdx];
            BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                           static_cast<std::uint32_t>(m_config.maxDepth));

            for (std::uint8_t c = 0U; c < 8U; ++c)
            {
                Eigen::Vector3f const corner = cornerPosition(c, bounds);
                node.cornerValues[c] = sampleSdf(corner) - m_config.isoValue;
            }
            
            // Debug: Print first node's corner values
            if (nodeIdx == 0U)
            {
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  Root node corner values: ";
                #endif
                for (int c = 0; c < 8; ++c)
                {
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << node.cornerValues[c] << " ";
                    #endif
                }
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << std::endl;
                #endif
            }
        }
    }

    void GlobalMortonOctree::detectIntersections(std::vector<std::size_t> const& nodeIndices)
    {
        static std::size_t debugCount = 0;
        static std::size_t negativeCornerCount = 0;
        static std::size_t positiveCornerCount = 0;
        
        for (std::size_t nodeIdx : nodeIndices)
        {
            auto& node = m_nodes[nodeIdx];

            // Check each corner for sign
            node.internalMask = 0U;
            for (std::uint8_t c = 0U; c < 8U; ++c)
            {
                if (node.cornerValues[c] < -ZERO_CROSSING_TOLERANCE)
                {
                    node.internalMask |= (1U << c);
                    ++negativeCornerCount;
                }
                else
                {
                    ++positiveCornerCount;
                }
            }

            // Check each edge for zero-crossing
            node.edgeMask = 0U;
            for (std::uint8_t e = 0U; e < 12U; ++e)
            {
                std::uint8_t const c0 = EDGE_CORNERS[e][0];
                std::uint8_t const c1 = EDGE_CORNERS[e][1];

                if (hasEdgeCrossing(node.cornerValues[c0], node.cornerValues[c1]))
                {
                    node.edgeMask |= (1U << e);
                }
            }

            node.isIntersecting = (node.edgeMask != 0U);
            
            // Debug: print first few leaf nodes - both intersecting and non-intersecting
            if (node.depth == 5 && debugCount < 5)
            {
                BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                               static_cast<std::uint32_t>(m_config.maxDepth));
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  Leaf node " << nodeIdx << " at depth " << (int)node.depth 
                          << " isIntersecting=" << node.isIntersecting
                          << " edgeMask=" << node.edgeMask
                          << " corners: ";
                          #endif
                for (int c = 0; c < 8; ++c)
                {
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << node.cornerValues[c] << " ";
                    #endif
                }
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "\n    bounds: [" << bounds.min.x << "," << bounds.min.y << "," << bounds.min.z 
                          << "] to [" << bounds.max.x << "," << bounds.max.y << "," << bounds.max.z << "]";
                std::cout << std::endl;
                #endif
                ++debugCount;
            }
            
            // Also print first few intersecting nodes
            static std::size_t intersectingDebugCount = 0;
            if (node.isIntersecting && intersectingDebugCount < 3)
            {
                BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                               static_cast<std::uint32_t>(m_config.maxDepth));
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  INTERSECTING node " << nodeIdx << " at depth " << (int)node.depth 
                          << " edgeMask=" << node.edgeMask
                          << " corners: ";
                          #endif
                for (int c = 0; c < 8; ++c)
                {
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << node.cornerValues[c] << " ";
                    #endif
                }
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "\n    bounds: [" << bounds.min.x << "," << bounds.min.y << "," << bounds.min.z 
                          << "] to [" << bounds.max.x << "," << bounds.max.y << "," << bounds.max.z << "]";
                std::cout << std::endl;
                #endif
                ++intersectingDebugCount;
            }
        }
        
        // Print summary of corner values at end
        static bool printedSummary = false;
        if (!printedSummary && nodeIndices.size() > 1000)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  Corner value summary: " << negativeCornerCount << " negative, "
                      << positiveCornerCount << " positive" << std::endl;
                      #endif
            printedSummary = true;
        }
    }

    void GlobalMortonOctree::createChildNodes(std::size_t parentLevelIndex, bool forceSubdivision)
    {
        auto& parentLevel = m_levels[parentLevelIndex];
        auto& childLevel = m_levels[parentLevelIndex + 1U];
        std::uint8_t const childDepth = static_cast<std::uint8_t>(parentLevelIndex + 1U);

        // Pre-reserve capacity to prevent reallocation during child creation
        // Each parent creates up to 8 children
        std::size_t potentialNewNodes = parentLevel.nodeIndices.size() * 8U;
        m_nodes.reserve(m_nodes.size() + potentialNewNodes);

        std::size_t subdivideCount = 0;
        for (std::size_t parentIdx : parentLevel.nodeIndices)
        {
            // Note: Don't hold references to m_nodes elements across allocations!
            // Check intersection status before any allocations
            if (!forceSubdivision && !m_nodes[parentIdx].isIntersecting)
            {
                continue;
            }
            
            ++subdivideCount;
            if (subdivideCount <= 3 || subdivideCount % 1000 == 0)
            {
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << " [subdividing node " << parentIdx << " at depth " << (int)m_nodes[parentIdx].depth 
                          << ", count=" << subdivideCount << "]" << std::flush;
                          #endif
            }

            m_nodes[parentIdx].isLeaf = false;

            // Cache parent values before creating children (reservation ensures no reallocation)
            std::uint64_t const parentMorton = m_nodes[parentIdx].mortonCode;
            std::uint8_t const parentDepth = m_nodes[parentIdx].depth;

            // Create 8 children
            for (std::uint8_t childIndex = 0U; childIndex < 8U; ++childIndex)
            {
                std::size_t const childNodeIdx = allocateNode();
                m_nodes[parentIdx].childIndices[childIndex] = childNodeIdx;

                auto& child = m_nodes[childNodeIdx];
                child.mortonCode = computeChildMorton(parentMorton, childIndex, parentDepth);
                child.depth = childDepth;

                // Register Morton code for lookup (depth-aware to avoid collisions)
                m_mortonToIndex[MortonNodeKey{child.mortonCode, child.depth}] = childNodeIdx;

                childLevel.nodeIndices.push_back(childNodeIdx);
            }
        }
    }

    void GlobalMortonOctree::balanceOctree()
    {
        // 2:1 octree balancing: ensure all intersecting cells have face-adjacent neighbors
        // at the same depth. This is critical for proper quad formation.
        
        std::cout << "  Balancing octree for watertight mesh generation..." << std::endl;
        
        std::size_t totalNeighborsCreated = 0U;
        std::size_t passes = 0U;
        constexpr std::size_t MAX_BALANCE_PASSES = 10U;
        
        // Repeat until no new neighbors are created (convergence)
        bool changed = true;
        while (changed && passes < MAX_BALANCE_PASSES)
        {
            changed = false;
            ++passes;
            std::size_t neighborsThisPass = 0U;
            
            // Collect all current intersecting leaves
            std::vector<std::size_t> intersectingLeaves;
            for (std::size_t i = 0U; i < m_nodes.size(); ++i)
            {
                if (m_nodes[i].isLeaf && m_nodes[i].isIntersecting)
                {
                    intersectingLeaves.push_back(i);
                }
            }
            
            // For each intersecting leaf, ensure its face-adjacent neighbors exist.
            // Additionally, ensure that for each *owned* edge with a sign-change we have the
            // full 2x2 neighborhood of cells around that edge (4 cells per edge).
            // This mirrors the GPU quad emission rule and avoids missing quads.
            for (std::size_t nodeIdx : intersectingLeaves)
            {
                auto const& node = m_nodes[nodeIdx];
                
                // Decode this cell's coordinates
                std::uint32_t cx = 0U;
                std::uint32_t cy = 0U;
                std::uint32_t cz = 0U;
                decodePathMorton(node.mortonCode, node.depth, cx, cy, cz);
                
                auto const maxCoord = (1U << node.depth) - 1U;
                
                // 6 face-adjacent neighbors: +x, -x, +y, -y, +z, -z
                std::array<std::tuple<int, int, int>, 6> const offsets = {{
                    {1, 0, 0}, {-1, 0, 0},
                    {0, 1, 0}, {0, -1, 0},
                    {0, 0, 1}, {0, 0, -1}
                }};
                
                for (auto const& [dx, dy, dz] : offsets)
                {
                    // Check bounds
                    auto const nx = static_cast<std::int32_t>(cx) + dx;
                    auto const ny = static_cast<std::int32_t>(cy) + dy;
                    auto const nz = static_cast<std::int32_t>(cz) + dz;
                    
                    if (nx < 0 || nx > static_cast<std::int32_t>(maxCoord) ||
                        ny < 0 || ny > static_cast<std::int32_t>(maxCoord) ||
                        nz < 0 || nz > static_cast<std::int32_t>(maxCoord))
                    {
                        continue; // Out of bounds
                    }
                    
                    // Check if neighbor exists
                    std::uint64_t const neighborMorton = encodePathMorton(
                        static_cast<std::uint32_t>(nx),
                        static_cast<std::uint32_t>(ny),
                        static_cast<std::uint32_t>(nz),
                        node.depth);
                    
                    if (m_mortonToIndex.find(MortonNodeKey{neighborMorton, node.depth}) == m_mortonToIndex.end())
                    {
                        // Neighbor doesn't exist - create it
                        std::size_t const newNodeIdx = createNodeAtCoordinates(
                            static_cast<std::uint32_t>(nx),
                            static_cast<std::uint32_t>(ny),
                            static_cast<std::uint32_t>(nz),
                            node.depth);
                        
                        if (newNodeIdx != std::numeric_limits<std::size_t>::max())
                        {
                            ++neighborsThisPass;
                            changed = true;
                        }
                    }
                }

                // Owned-edge neighbor completion (CPU edge numbering):
                // - Edge 3: X-axis at (y=max, z=max)
                // - Edge 7: Y-axis at (x=max, z=max)
                // - Edge 11: Z-axis at (x=max, y=max)
                // Each quad needs 4 cells; we create the 3 neighbors around the owned edge.
                auto ensureCellAt = [&](std::int32_t x, std::int32_t y, std::int32_t z)
                {
                    if (x < 0 || x > static_cast<std::int32_t>(maxCoord) ||
                        y < 0 || y > static_cast<std::int32_t>(maxCoord) ||
                        z < 0 || z > static_cast<std::int32_t>(maxCoord))
                    {
                        return;
                    }

                    std::uint64_t const morton = encodePathMorton(
                        static_cast<std::uint32_t>(x),
                        static_cast<std::uint32_t>(y),
                        static_cast<std::uint32_t>(z),
                        node.depth);

                    if (m_mortonToIndex.find(MortonNodeKey{morton, node.depth}) == m_mortonToIndex.end())
                    {
                        std::size_t const newNodeIdx = createNodeAtCoordinates(
                            static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y),
                            static_cast<std::uint32_t>(z),
                            node.depth);
                        if (newNodeIdx != std::numeric_limits<std::size_t>::max())
                        {
                            ++neighborsThisPass;
                            changed = true;
                        }
                    }
                };

                if ((node.edgeMask & (1U << 3U)) != 0U)
                {
                    ensureCellAt(static_cast<std::int32_t>(cx) + 0, static_cast<std::int32_t>(cy) + 1, static_cast<std::int32_t>(cz) + 0);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 0, static_cast<std::int32_t>(cy) + 0, static_cast<std::int32_t>(cz) + 1);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 0, static_cast<std::int32_t>(cy) + 1, static_cast<std::int32_t>(cz) + 1);
                }
                if ((node.edgeMask & (1U << 7U)) != 0U)
                {
                    ensureCellAt(static_cast<std::int32_t>(cx) + 1, static_cast<std::int32_t>(cy) + 0, static_cast<std::int32_t>(cz) + 0);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 0, static_cast<std::int32_t>(cy) + 0, static_cast<std::int32_t>(cz) + 1);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 1, static_cast<std::int32_t>(cy) + 0, static_cast<std::int32_t>(cz) + 1);
                }
                if ((node.edgeMask & (1U << 11U)) != 0U)
                {
                    ensureCellAt(static_cast<std::int32_t>(cx) + 1, static_cast<std::int32_t>(cy) + 0, static_cast<std::int32_t>(cz) + 0);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 0, static_cast<std::int32_t>(cy) + 1, static_cast<std::int32_t>(cz) + 0);
                    ensureCellAt(static_cast<std::int32_t>(cx) + 1, static_cast<std::int32_t>(cy) + 1, static_cast<std::int32_t>(cz) + 0);
                }
            }
            
            totalNeighborsCreated += neighborsThisPass;
            
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            if (neighborsThisPass > 0U)
            {
                std::cout << "    Balance pass " << passes << ": created " 
                          << neighborsThisPass << " neighbor cells" << std::endl;
            }
            #endif
        }
        
        std::cout << "  Balancing complete: " << totalNeighborsCreated << " neighbor cells created in "
                  << passes << " passes" << std::endl;
    }

    std::size_t GlobalMortonOctree::createNodeAtCoordinates(std::uint32_t x, std::uint32_t y,
                                                             std::uint32_t z, std::uint8_t depth)
    {
        // First check if node already exists
        std::uint64_t const mortonCode = encodePathMorton(x, y, z, depth);
        auto it = m_mortonToIndex.find(MortonNodeKey{mortonCode, depth});
        if (it != m_mortonToIndex.end())
        {
            return it->second; // Already exists
        }
        
        // Allocate and initialize the node
        std::size_t const nodeIdx = allocateNode();
        auto& node = m_nodes[nodeIdx];
        node.mortonCode = mortonCode;
        node.depth = depth;
        node.isLeaf = true;
        node.isIntersecting = false; // Will be evaluated below
        node.needsRefinement = false;
        
        // Register in Morton lookup (depth-aware)
        m_mortonToIndex[MortonNodeKey{mortonCode, depth}] = nodeIdx;
        
        // Add to appropriate level
        if (depth < m_levels.size())
        {
            m_levels[depth].nodeIndices.push_back(nodeIdx);
        }
        
        // Evaluate corners for this node to determine if it's intersecting.
        // Must match the normal evaluation path: cornerValues store (sdf - isoValue),
        // internalMask marks corners with value < 0, and edgeMask is derived from sign changes.
        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                   static_cast<std::uint32_t>(m_config.maxDepth));
        
        // Sample SDF at all 8 corners
        node.internalMask = 0U;
        for (std::uint8_t c = 0U; c < 8U; ++c)
        {
            Eigen::Vector3f const cornerPos = cornerPosition(c, bounds);
            float const v = sampleSdf(cornerPos) - m_config.isoValue;
            node.cornerValues[c] = v;
            if (v < -ZERO_CROSSING_TOLERANCE)
            {
                node.internalMask |= (1U << c);
            }
        }
        
        // Detect edge crossings
        node.edgeMask = 0U;
        for (std::size_t e = 0U; e < 12U; ++e)
        {
            auto const c0 = EDGE_CORNERS[e][0];
            auto const c1 = EDGE_CORNERS[e][1];
            float const v0 = node.cornerValues[c0];
            float const v1 = node.cornerValues[c1];

            if (hasEdgeCrossing(v0, v1))
            {
                node.edgeMask |= (1U << e);
            }
        }

        node.isIntersecting = (node.edgeMask != 0U);
        
        return nodeIdx;
    }

    void GlobalMortonOctree::ensureProjectedVertex(GlobalOctreeNode& node)
    {
        if (!node.vertexIndices.empty())
        {
            return;
        }

        std::uint32_t existingIndex = 0U;
        if (m_vertexRegistry.tryGetCellVertexIndex(node.mortonCode, node.depth, existingIndex, 0U))
        {
            node.vertexIndices.push_back(existingIndex);
            return;
        }

        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                       static_cast<std::uint32_t>(m_config.maxDepth));

        Eigen::Vector3f p;
        p.x() = 0.5F * (bounds.min.s[0] + bounds.max.s[0]);
        p.y() = 0.5F * (bounds.min.s[1] + bounds.max.s[1]);
        p.z() = 0.5F * (bounds.min.s[2] + bounds.max.s[2]);

        float const cellSize = bounds.max.s[0] - bounds.min.s[0];
        float const epsilon = std::max(cellSize * 0.01F, 1e-6F);

        for (std::size_t iter = 0U; iter < 16U; ++iter)
        {
            float const value = sampleSdf(p) - m_config.isoValue;
            if (std::abs(value) < ZERO_CROSSING_TOLERANCE)
            {
                break;
            }

            Eigen::Vector3f const grad = sampleGradient(p, epsilon);
            float const g2 = grad.squaredNorm();
            if (g2 < 1e-20F)
            {
                break;
            }

            p = p - (value / g2) * grad;
            p.x() = std::clamp(p.x(), bounds.min.s[0], bounds.max.s[0]);
            p.y() = std::clamp(p.y(), bounds.min.s[1], bounds.max.s[1]);
            p.z() = std::clamp(p.z(), bounds.min.s[2], bounds.max.s[2]);
        }

        Eigen::Vector3f normal = sampleGradient(p, epsilon);
        float const nLen = normal.norm();
        if (nLen > 1e-12F)
        {
            normal /= nLen;
        }
        else
        {
            normal = Eigen::Vector3f(1.0F, 0.0F, 0.0F);
        }

        std::uint32_t const vertexIndex = m_vertexRegistry.registerCellVertex(node.mortonCode, node.depth, p, normal, 0U);
        node.vertexIndices.push_back(vertexIndex);
    }

    void GlobalMortonOctree::generateHaloVerticesForWatertightness()
    {
        // We only ever emit faces from intersecting cells, but we may still need vertices
        // in adjacent non-intersecting cells to avoid boundary edges (holes).

        std::vector<std::size_t> intersectingLeaves;
        intersectingLeaves.reserve(m_nodes.size());
        for (std::size_t i = 0U; i < m_nodes.size(); ++i)
        {
            if (m_nodes[i].isLeaf && m_nodes[i].isIntersecting)
            {
                intersectingLeaves.push_back(i);
            }
        }

        auto ensureNeighborVertex = [&](GlobalOctreeNode const& baseNode, std::uint32_t cx, std::uint32_t cy, std::uint32_t cz,
                                        std::int32_t dx, std::int32_t dy, std::int32_t dz)
        {
            auto const maxCoord = (1U << baseNode.depth) - 1U;
            auto const nx = static_cast<std::int32_t>(cx) + dx;
            auto const ny = static_cast<std::int32_t>(cy) + dy;
            auto const nz = static_cast<std::int32_t>(cz) + dz;

            if (nx < 0 || nx > static_cast<std::int32_t>(maxCoord) ||
                ny < 0 || ny > static_cast<std::int32_t>(maxCoord) ||
                nz < 0 || nz > static_cast<std::int32_t>(maxCoord))
            {
                return;
            }

            std::uint64_t const morton = encodePathMorton(
                static_cast<std::uint32_t>(nx),
                static_cast<std::uint32_t>(ny),
                static_cast<std::uint32_t>(nz),
                baseNode.depth);

            auto it = m_mortonToIndex.find(MortonNodeKey{morton, baseNode.depth});
            if (it == m_mortonToIndex.end())
            {
                // Should not happen after balancing, but keep this safe.
                std::size_t const created = createNodeAtCoordinates(
                    static_cast<std::uint32_t>(nx),
                    static_cast<std::uint32_t>(ny),
                    static_cast<std::uint32_t>(nz),
                    baseNode.depth);
                if (created == std::numeric_limits<std::size_t>::max())
                {
                    return;
                }
                it = m_mortonToIndex.find(MortonNodeKey{morton, baseNode.depth});
                if (it == m_mortonToIndex.end())
                {
                    return;
                }
            }

            auto& neighbor = m_nodes[it->second];
            if (!neighbor.isLeaf)
            {
                return;
            }

            // Only create halo vertices for non-intersecting cells. Intersecting cells should
            // already have a QEF vertex from generateVertices().
            if (!neighbor.isIntersecting)
            {
                ensureProjectedVertex(neighbor);
            }
        };

        for (std::size_t nodeIdx : intersectingLeaves)
        {
            auto const& node = m_nodes[nodeIdx];
            if (node.depth == 0U)
            {
                continue;
            }

            std::uint32_t cx = 0U, cy = 0U, cz = 0U;
            decodePathMorton(node.mortonCode, node.depth, cx, cy, cz);

            // Owned edges only (3, 7, 11): ensure the other three cells have vertices.
            if ((node.edgeMask & (1U << 3U)) != 0U)
            {
                ensureNeighborVertex(node, cx, cy, cz, 0, +1, 0);
                ensureNeighborVertex(node, cx, cy, cz, 0, 0, +1);
                ensureNeighborVertex(node, cx, cy, cz, 0, +1, +1);
            }
            if ((node.edgeMask & (1U << 7U)) != 0U)
            {
                ensureNeighborVertex(node, cx, cy, cz, +1, 0, 0);
                ensureNeighborVertex(node, cx, cy, cz, 0, 0, +1);
                ensureNeighborVertex(node, cx, cy, cz, +1, 0, +1);
            }
            if ((node.edgeMask & (1U << 11U)) != 0U)
            {
                ensureNeighborVertex(node, cx, cy, cz, +1, 0, 0);
                ensureNeighborVertex(node, cx, cy, cz, 0, +1, 0);
                ensureNeighborVertex(node, cx, cy, cz, +1, +1, 0);
            }
        }

        m_stats.vertexCount = m_vertexRegistry.getVertexCount();
    }

    void GlobalMortonOctree::ensureNeighborsExist(std::size_t nodeIndex)
    {
        // This is a wrapper that ensures face-adjacent neighbors exist for a single node.
        // The actual implementation is in balanceOctree which does it for all nodes.
        auto const& node = m_nodes[nodeIndex];
        if (!node.isLeaf)
        {
            return;
        }
        
        std::uint32_t cx = 0U;
        std::uint32_t cy = 0U;
        std::uint32_t cz = 0U;
        decodePathMorton(node.mortonCode, node.depth, cx, cy, cz);
        
        auto const maxCoord = (1U << node.depth) - 1U;
        
        // 6 face-adjacent neighbors
        std::array<std::tuple<int, int, int>, 6> const offsets = {{
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1}
        }};
        
        for (auto const& [dx, dy, dz] : offsets)
        {
            auto const nx = static_cast<std::int32_t>(cx) + dx;
            auto const ny = static_cast<std::int32_t>(cy) + dy;
            auto const nz = static_cast<std::int32_t>(cz) + dz;
            
            if (nx < 0 || nx > static_cast<std::int32_t>(maxCoord) ||
                ny < 0 || ny > static_cast<std::int32_t>(maxCoord) ||
                nz < 0 || nz > static_cast<std::int32_t>(maxCoord))
            {
                continue;
            }
            
            (void)createNodeAtCoordinates(
                static_cast<std::uint32_t>(nx),
                static_cast<std::uint32_t>(ny),
                static_cast<std::uint32_t>(nz),
                node.depth);
        }
    }

    void GlobalMortonOctree::decodePathMorton(std::uint64_t mortonCode, std::uint8_t depth,
                                               std::uint32_t& x, std::uint32_t& y, std::uint32_t& z) const
    {
        x = 0U;
        y = 0U;
        z = 0U;
        
        for (std::uint8_t d = 0U; d < depth; ++d)
        {
            auto const shift = (depth - 1U - d) * 3U;
            auto const octant = static_cast<std::uint8_t>((mortonCode >> shift) & 0x7U);
            
            auto const ox = (octant >> 0U) & 1U;
            auto const oy = (octant >> 1U) & 1U;
            auto const oz = (octant >> 2U) & 1U;
            
            auto const levelScale = 1U << (depth - 1U - d);
            x += ox * levelScale;
            y += oy * levelScale;
            z += oz * levelScale;
        }
    }

    std::uint64_t GlobalMortonOctree::encodePathMorton(std::uint32_t x, std::uint32_t y,
                                                        std::uint32_t z, std::uint8_t depth) const
    {
        std::uint64_t code = 0U;
        for (std::uint8_t d = 0U; d < depth; ++d)
        {
            auto const shift = depth - 1U - d;
            auto const ox = (x >> shift) & 1U;
            auto const oy = (y >> shift) & 1U;
            auto const oz = (z >> shift) & 1U;
            code = (code << 3U) | (ox | (oy << 1U) | (oz << 2U));
        }
        return code;
    }

    void GlobalMortonOctree::refineAdaptively()
    {
        for (std::size_t pass = 0U; pass < m_config.refinementPasses; ++pass)
        {
            // Collect current intersecting leaves
            std::vector<std::size_t> leaves;
            for (std::size_t i = 0U; i < m_nodes.size(); ++i)
            {
                if (m_nodes[i].isLeaf && m_nodes[i].isIntersecting &&
                    m_nodes[i].depth < m_config.maxDepth)
                {
                    leaves.push_back(i);
                }
            }

            if (leaves.empty())
            {
                break;
            }

            // Estimate curvature and mark high-curvature leaves
            estimateCurvatureGpu(leaves);

            // Count how many need refinement
            std::size_t refineCount = 0U;
            for (std::size_t idx : leaves)
            {
                if (m_nodes[idx].needsRefinement)
                {
                    ++refineCount;
                }
            }

            if (refineCount == 0U)
            {
                break;
            }

            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  Refinement pass " << (pass + 1U) << ": subdividing "
                      << refineCount << " leaves" << std::endl;

#endif
            // Subdivide marked leaves
            subdivideMarkedLeaves();
        }
    }

    void GlobalMortonOctree::estimateCurvatureGpu(std::vector<std::size_t> const& leafIndices)
    {
        // For now, use CPU curvature estimation
        // TODO: GPU acceleration using HierarchicalDCProgram::estimateCurvature

        float const epsilon = m_globalBboxSize.maxCoeff() / static_cast<float>(1U << m_config.maxDepth) * 0.5F;

        for (std::size_t nodeIdx : leafIndices)
        {
            auto& node = m_nodes[nodeIdx];
            BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                           static_cast<std::uint32_t>(m_config.maxDepth));

            Eigen::Vector3f const center(
                (bounds.min.s[0] + bounds.max.s[0]) * 0.5F,
                (bounds.min.s[1] + bounds.max.s[1]) * 0.5F,
                (bounds.min.s[2] + bounds.max.s[2]) * 0.5F);

            // Central gradient
            Eigen::Vector3f const gradCenter = sampleGradient(center, epsilon);
            Eigen::Vector3f const normCenter = gradCenter.normalized();

            // Sample gradients at 6 neighbors and compute variance
            Eigen::Vector3f const offsets[6] = {
                {epsilon, 0.0F, 0.0F}, {-epsilon, 0.0F, 0.0F},
                {0.0F, epsilon, 0.0F}, {0.0F, -epsilon, 0.0F},
                {0.0F, 0.0F, epsilon}, {0.0F, 0.0F, -epsilon}};

            float variance = 0.0F;
            for (auto const& offset : offsets)
            {
                Eigen::Vector3f const gradNeighbor = sampleGradient(center + offset, epsilon);
                Eigen::Vector3f const normNeighbor = gradNeighbor.normalized();
                Eigen::Vector3f const diff = normCenter - normNeighbor;
                variance += diff.squaredNorm();
            }
            variance /= 6.0F;

            node.curvatureMetric = variance;
            node.needsRefinement = (variance > m_config.curvatureThreshold);
        }
    }

    void GlobalMortonOctree::subdivideMarkedLeaves()
    {
        std::vector<std::size_t> toSubdivide;
        for (std::size_t i = 0U; i < m_nodes.size(); ++i)
        {
            if (m_nodes[i].needsRefinement)
            {
                toSubdivide.push_back(i);
            }
        }

        // Reserve capacity to avoid reallocation during child creation
        m_nodes.reserve(m_nodes.size() + toSubdivide.size() * 8U);

        for (std::size_t parentIdx : toSubdivide)
        {
            m_nodes[parentIdx].isLeaf = false;
            m_nodes[parentIdx].needsRefinement = false;

            std::uint8_t const childDepth = m_nodes[parentIdx].depth + 1U;
            std::uint64_t const parentMorton = m_nodes[parentIdx].mortonCode;
            std::uint8_t const parentDepth = m_nodes[parentIdx].depth;

            // Create 8 children
            for (std::uint8_t childIndex = 0U; childIndex < 8U; ++childIndex)
            {
                std::size_t const childNodeIdx = allocateNode();
                m_nodes[parentIdx].childIndices[childIndex] = childNodeIdx;

                auto& child = m_nodes[childNodeIdx];
                child.mortonCode = computeChildMorton(parentMorton, childIndex, parentDepth);
                child.depth = childDepth;

                m_mortonToIndex[MortonNodeKey{child.mortonCode, child.depth}] = childNodeIdx;
                m_levels[childDepth].nodeIndices.push_back(childNodeIdx);
            }
        }

        // Process new nodes
        for (std::size_t depth = 0U; depth <= m_config.maxDepth; ++depth)
        {
            auto& level = m_levels[depth];

            // Filter to only new nodes (not yet evaluated)
            std::vector<std::size_t> newNodes;
            for (std::size_t idx : level.nodeIndices)
            {
                if (m_nodes[idx].edgeMask == 0U && m_nodes[idx].internalMask == 0U)
                {
                    // Not yet evaluated
                    newNodes.push_back(idx);
                }
            }

            if (!newNodes.empty())
            {
                evaluateCornersCpu(newNodes);
                detectIntersections(newNodes);
            }
        }

        // Update statistics
        m_stats.leafNodes = 0U;
        m_stats.intersectingLeaves = 0U;
        for (auto const& node : m_nodes)
        {
            if (node.isLeaf)
            {
                ++m_stats.leafNodes;
                if (node.isIntersecting)
                {
                    ++m_stats.intersectingLeaves;
                }
            }
        }
    }

    void GlobalMortonOctree::generateVertices()
    {
        auto const startTime = std::chrono::high_resolution_clock::now();

        bool const debugProgress = (std::getenv("GLADIUS_DEBUG_MDC_CONFIG") != nullptr);
        std::size_t const totalNodes = m_nodes.size();
        std::size_t intersectingLeaves = 0U;
        for (auto const& n : m_nodes)
        {
            if (n.isLeaf && n.isIntersecting)
            {
                ++intersectingLeaves;
            }
        }
        if (debugProgress)
        {
            std::cout << "  Generating vertices for " << intersectingLeaves << " intersecting leaves (totalNodes=" << totalNodes << ")..." << std::endl;
        }

        std::size_t processedIntersecting = 0U;
        std::size_t nextProgress = 0U;
        if (debugProgress)
        {
            // Print progress roughly 20 times across the run (minimum interval guard).
            nextProgress = std::max<std::size_t>(intersectingLeaves / 20U, 25000U);
        }

        for (auto& node : m_nodes)
        {
            if (!node.isLeaf || !node.isIntersecting)
            {
                continue;
            }

            ++processedIntersecting;
            if (debugProgress && (processedIntersecting % nextProgress == 0U))
            {
                float const pct = intersectingLeaves > 0U ? (100.0F * static_cast<float>(processedIntersecting) / static_cast<float>(intersectingLeaves)) : 100.0F;
                std::cout << "    vertexGen progress: " << processedIntersecting << "/" << intersectingLeaves << " (" << pct << "%)" << std::endl;
            }

            // Gather Hermite samples for this cell
            gatherHermiteSamples(node);

            // Solve QEF to get vertex position
            solveQefForNode(node);
        }

        m_stats.vertexCount = m_vertexRegistry.getVertexCount();

        auto const endTime = std::chrono::high_resolution_clock::now();
        m_stats.vertexGenerationTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        if (debugProgress)
        {
            std::cout << "  Vertex generation complete in " << m_stats.vertexGenerationTimeMs << " ms (vertices=" << m_stats.vertexCount << ")" << std::endl;
        }
    }

    void GlobalMortonOctree::gatherHermiteSamples(GlobalOctreeNode& node)
    {
        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                       static_cast<std::uint32_t>(m_config.maxDepth));

        float const epsilon = (bounds.max.s[0] - bounds.min.s[0]) * 0.01F;

        for (std::uint8_t e = 0U; e < 12U; ++e)
        {
            if (!(node.edgeMask & (1U << e)))
            {
                continue;
            }

            std::uint8_t const c0 = EDGE_CORNERS[e][0];
            std::uint8_t const c1 = EDGE_CORNERS[e][1];

            Eigen::Vector3f const start = cornerPosition(c0, bounds);
            Eigen::Vector3f const end = cornerPosition(c1, bounds);

            Eigen::Vector3f position;
            refineZeroCrossing(start, end, node.cornerValues[c0], node.cornerValues[c1], position);

            Eigen::Vector3f const gradient = sampleGradient(position, epsilon);

            HermiteSample sample;
            sample.position = position;
            sample.gradient = gradient;
            sample.value = 0.0F;
            sample.edgeIndex = e;

            node.hermiteSamples.push_back(sample);
        }
    }

    void GlobalMortonOctree::refineZeroCrossing(Eigen::Vector3f const& start,
                                                  Eigen::Vector3f const& end,
                                                  float startValue,
                                                  float endValue,
                                                  Eigen::Vector3f& outPosition)
    {
        if (std::abs(startValue) <= ZERO_CROSSING_TOLERANCE)
        {
            outPosition = start;
            return;
        }

        if (std::abs(endValue) <= ZERO_CROSSING_TOLERANCE)
        {
            outPosition = end;
            return;
        }

        // Defensive: if the edge does not actually bracket the iso-surface, fall back
        // to the midpoint to avoid unstable bisection behavior.
        if ((startValue < 0.0F) == (endValue < 0.0F))
        {
            outPosition = (start + end) * 0.5F;
            return;
        }

        Eigen::Vector3f lo = start;
        Eigen::Vector3f hi = end;
        float vLo = startValue;
        float vHi = endValue;

        for (std::size_t iter = 0U; iter < MAX_BISECTION_ITERATIONS; ++iter)
        {
            Eigen::Vector3f const mid = (lo + hi) * 0.5F;
            float const vMid = sampleSdf(mid) - m_config.isoValue;

            if (std::abs(vMid) < ZERO_CROSSING_TOLERANCE)
            {
                outPosition = mid;
                return;
            }

            if (vMid * vLo < 0.0F)
            {
                hi = mid;
                vHi = vMid;
            }
            else
            {
                lo = mid;
                vLo = vMid;
            }
        }

        outPosition = (lo + hi) * 0.5F;
    }

    void GlobalMortonOctree::solveQefForNode(GlobalOctreeNode& node)
    {
        if (node.hermiteSamples.empty())
        {
            return;
        }

        node.vertexIndices.clear();
    node.edgeComponents.fill(0U);
        node.edge3Component = 0U;
        node.edge7Component = 0U;
        node.edge11Component = 0U;

        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                       static_cast<std::uint32_t>(m_config.maxDepth));

        // Compute mass point as initial position
        Eigen::Vector3f massPoint = Eigen::Vector3f::Zero();
        for (auto const& sample : node.hermiteSamples)
        {
            massPoint += sample.position;
        }
        massPoint /= static_cast<float>(node.hermiteSamples.size());

        // Manifold Dual Contouring requires multiple vertices in ambiguous cells.
        // Instead of clustering by gradients (which is unreliable for topology), we
        // compute per-cell components by connecting edge intersections through the
        // 6 faces using marching-squares connectivity (with asymptotic decider for
        // ambiguous cases 5/10).
        float constexpr NORMAL_EPS = 1e-6F;

        // Map edgeIndex -> hermite sample index (or -1 if no intersection on that edge).
        std::array<int, 12> sampleIndexByEdge;
        sampleIndexByEdge.fill(-1);
        for (std::size_t i = 0U; i < node.hermiteSamples.size(); ++i)
        {
            std::uint8_t const e = node.hermiteSamples[i].edgeIndex;
            if (e < sampleIndexByEdge.size())
            {
                sampleIndexByEdge[e] = static_cast<int>(i);
            }
        }

        // Union-Find over hermite samples.
        std::vector<int> parent(node.hermiteSamples.size(), -1);
        std::vector<int> rank(node.hermiteSamples.size(), 0);
        for (std::size_t i = 0U; i < parent.size(); ++i)
        {
            parent[i] = static_cast<int>(i);
        }

        auto findRoot = [&](int x)
        {
            while (parent[static_cast<std::size_t>(x)] != x)
            {
                parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
                x = parent[static_cast<std::size_t>(x)];
            }
            return x;
        };

        auto unite = [&](int a, int b)
        {
            if (a < 0 || b < 0)
            {
                return;
            }
            int ra = findRoot(a);
            int rb = findRoot(b);
            if (ra == rb)
            {
                return;
            }
            int& rra = rank[static_cast<std::size_t>(ra)];
            int& rrb = rank[static_cast<std::size_t>(rb)];
            if (rra < rrb)
            {
                parent[static_cast<std::size_t>(ra)] = rb;
            }
            else if (rrb < rra)
            {
                parent[static_cast<std::size_t>(rb)] = ra;
            }
            else
            {
                parent[static_cast<std::size_t>(rb)] = ra;
                ++rra;
            }
        };

        struct FaceConnectivity
        {
            std::array<std::uint8_t, 4> corners;
            std::array<std::uint8_t, 4> edges; // in order: (0-1),(1-2),(2-3),(3-0)
        };

        // Corner order is around the face (0,1,2,3) with edges accordingly.
        constexpr std::array<FaceConnectivity, 6> FACES = {{
            // z=0
            FaceConnectivity{{0U, 1U, 3U, 2U}, {0U, 5U, 1U, 4U}},
            // z=1
            FaceConnectivity{{4U, 5U, 7U, 6U}, {2U, 7U, 3U, 6U}},
            // y=0
            FaceConnectivity{{0U, 1U, 5U, 4U}, {0U, 9U, 2U, 8U}},
            // y=1
            FaceConnectivity{{2U, 3U, 7U, 6U}, {1U, 11U, 3U, 10U}},
            // x=0
            FaceConnectivity{{0U, 2U, 6U, 4U}, {4U, 10U, 6U, 8U}},
            // x=1
            FaceConnectivity{{1U, 3U, 7U, 5U}, {5U, 11U, 7U, 9U}},
        }};

        auto isCornerInside = [&](std::uint8_t cornerIndex) -> bool
        {
            return (node.internalMask & (1U << cornerIndex)) != 0U;
        };

        auto connectFace = [&, this](FaceConnectivity const& f)
        {
            std::uint8_t ccase = 0U;
            if (isCornerInside(f.corners[0])) { ccase |= 1U; }
            if (isCornerInside(f.corners[1])) { ccase |= 2U; }
            if (isCornerInside(f.corners[2])) { ccase |= 4U; }
            if (isCornerInside(f.corners[3])) { ccase |= 8U; }

            if (ccase == 0U || ccase == 15U)
            {
                return;
            }

            auto edgeSampleIndex = [&](std::uint8_t faceEdgeIndex) -> int
            {
                std::uint8_t const cellEdge = f.edges[faceEdgeIndex];
                if (cellEdge >= sampleIndexByEdge.size())
                {
                    return -1;
                }
                return sampleIndexByEdge[cellEdge];
            };

            auto connectEdges = [&](std::uint8_t a, std::uint8_t b)
            {
                unite(edgeSampleIndex(a), edgeSampleIndex(b));
            };

            switch (ccase)
            {
                case 1U:  connectEdges(3U, 0U); break;
                case 2U:  connectEdges(0U, 1U); break;
                case 3U:  connectEdges(3U, 1U); break;
                case 4U:  connectEdges(1U, 2U); break;
                case 6U:  connectEdges(0U, 2U); break;
                case 7U:  connectEdges(3U, 2U); break;
                case 8U:  connectEdges(2U, 3U); break;
                case 9U:  connectEdges(0U, 2U); break;
                case 11U: connectEdges(1U, 2U); break;
                case 12U: connectEdges(1U, 3U); break;
                case 13U: connectEdges(0U, 1U); break;
                case 14U: connectEdges(3U, 0U); break;
                case 5U:
                case 10U:
                {
                    // Ambiguous marching-squares cases on this face. For robust component
                    // labeling on non-trilinear fields, sample the actual SDF at the face
                    // center (rather than relying purely on corner values).

                    Eigen::Vector3f const p0 = cornerPosition(f.corners[0], bounds);
                    Eigen::Vector3f const p1 = cornerPosition(f.corners[1], bounds);
                    Eigen::Vector3f const p2 = cornerPosition(f.corners[2], bounds);
                    Eigen::Vector3f const p3 = cornerPosition(f.corners[3], bounds);
                    Eigen::Vector3f const faceCenter = 0.25F * (p0 + p1 + p2 + p3);

                    bool const centerInside = (sampleSdf(faceCenter) - m_config.isoValue) < 0.0F;
                    bool const diagonal02Inside = isCornerInside(f.corners[0]) && isCornerInside(f.corners[2]);

                    // If center sign matches the (0,2) diagonal's inside-ness, connect edges
                    // around the opposite diagonal. This generalizes the asymptotic decider
                    // for both ambiguous cases 5 and 10.
                    if (centerInside == diagonal02Inside)
                    {
                        connectEdges(0U, 1U);
                        connectEdges(2U, 3U);
                    }
                    else
                    {
                        connectEdges(0U, 3U);
                        connectEdges(1U, 2U);
                    }
                    break;
                }
                default:
                    break;
            }
        };

        for (auto const& f : FACES)
        {
            connectFace(f);
        }

        // Assign compact component ids.
        std::unordered_map<int, std::uint8_t> rootToComponent;
        rootToComponent.reserve(node.hermiteSamples.size());

        std::vector<std::uint8_t> sampleComponent(node.hermiteSamples.size(), 0U);
        std::uint8_t componentCount = 0U;
        for (std::size_t i = 0U; i < node.hermiteSamples.size(); ++i)
        {
            int const root = findRoot(static_cast<int>(i));
            auto it = rootToComponent.find(root);
            if (it == rootToComponent.end())
            {
                if (componentCount < std::numeric_limits<std::uint8_t>::max())
                {
                    std::uint8_t const c = componentCount;
                    rootToComponent.emplace(root, c);
                    ++componentCount;
                    sampleComponent[i] = c;
                }
                else
                {
                    // Extremely unlikely in practice, but keep the code safe.
                    sampleComponent[i] = 0U;
                }
            }
            else
            {
                sampleComponent[i] = it->second;
            }
        }

        if (componentCount == 0U)
        {
            componentCount = 1U;
        }

        // Edge→component mapping for quad emission.
        // Any of the 4 cells around a geometric edge may reference that same edge
        // via a different local edge index, so we need a mapping for all 12 edges.
        for (std::uint8_t e = 0U; e < node.edgeComponents.size(); ++e)
        {
            int const idx = sampleIndexByEdge[e];
            if (idx >= 0)
            {
                node.edgeComponents[e] = sampleComponent[static_cast<std::size_t>(idx)];
            }
        }
        node.edge3Component = node.edgeComponents[3U];
        node.edge7Component = node.edgeComponents[7U];
        node.edge11Component = node.edgeComponents[11U];

        auto solveComponent = [&](std::uint8_t comp) -> std::pair<Eigen::Vector3f, Eigen::Vector3f>
        {
            std::vector<HermiteSample const*> samples;
            samples.reserve(node.hermiteSamples.size());
            for (std::size_t i = 0U; i < node.hermiteSamples.size(); ++i)
            {
                if (sampleComponent[i] == comp)
                {
                    samples.push_back(&node.hermiteSamples[i]);
                }
            }

            Eigen::Vector3f vertex = massPoint;
            Eigen::Vector3f normal = Eigen::Vector3f::Zero();
            Eigen::Vector3f centroid = Eigen::Vector3f::Zero();

            if (samples.size() >= 3U)
            {
                Eigen::Matrix3f ata = Eigen::Matrix3f::Zero();
                Eigen::Vector3f atb = Eigen::Vector3f::Zero();
                for (auto const* s : samples)
                {
                    Eigen::Vector3f n = s->gradient;
                    float const nLen = n.norm();
                    if (nLen > NORMAL_EPS)
                    {
                        n /= nLen;
                    }
                    ata += n * n.transpose();
                    atb += n * n.dot(s->position);
                    normal += n;
                    centroid += s->position;
                }

                centroid /= static_cast<float>(samples.size());

                Eigen::JacobiSVD<Eigen::Matrix3f> svd(ata, Eigen::ComputeFullU | Eigen::ComputeFullV);
                Eigen::Vector3f const solved = svd.solve(atb);

                // If the unconstrained solution is outside the cell, use the component centroid.
                // This avoids multiple adjacent cells clamping to the same boundary corner/edge,
                // which can collapse edges and create topological cracks.
                float const cellSizeX = bounds.max.s[0] - bounds.min.s[0];
                float const cellSizeY = bounds.max.s[1] - bounds.min.s[1];
                float const cellSizeZ = bounds.max.s[2] - bounds.min.s[2];
                float const eps = 1e-6F * std::max({cellSizeX, cellSizeY, cellSizeZ, 1.0F});

                bool const outside = (solved.x() < bounds.min.s[0] - eps) || (solved.x() > bounds.max.s[0] + eps) ||
                                     (solved.y() < bounds.min.s[1] - eps) || (solved.y() > bounds.max.s[1] + eps) ||
                                     (solved.z() < bounds.min.s[2] - eps) || (solved.z() > bounds.max.s[2] + eps);

                vertex = outside ? centroid : solved;
            }
            else if (!samples.empty())
            {
                // Mass point for this component.
                vertex = Eigen::Vector3f::Zero();
                for (auto const* s : samples)
                {
                    vertex += s->position;
                    centroid += s->position;
                    Eigen::Vector3f n = s->gradient;
                    float const nLen = n.norm();
                    if (nLen > NORMAL_EPS)
                    {
                        n /= nLen;
                    }
                    normal += n;
                }
                vertex /= static_cast<float>(samples.size());
                centroid /= static_cast<float>(samples.size());
            }

            // Clamp to cell bounds
            vertex.x() = std::clamp(vertex.x(), bounds.min.s[0], bounds.max.s[0]);
            vertex.y() = std::clamp(vertex.y(), bounds.min.s[1], bounds.max.s[1]);
            vertex.z() = std::clamp(vertex.z(), bounds.min.s[2], bounds.max.s[2]);

            float const nLen = normal.norm();
            if (nLen > NORMAL_EPS)
            {
                normal /= nLen;
            }
            else
            {
                normal = Eigen::Vector3f(1.0F, 0.0F, 0.0F);
            }

            return {vertex, normal};
        };

        for (std::uint8_t comp = 0U; comp < componentCount; ++comp)
        {
            auto const [vertex, normal] = solveComponent(comp);
            std::uint32_t const vertexIndex = m_vertexRegistry.registerCellVertex(node.mortonCode, node.depth, vertex, normal, comp);
            node.vertexIndices.push_back(vertexIndex);
        }
    }

    void GlobalMortonOctree::extractMesh(std::vector<Eigen::Vector3f>& positions,
                                          std::vector<Eigen::Vector3f>& normals,
                                          std::vector<std::uint32_t>& indices)
    {
        auto const startTime = std::chrono::high_resolution_clock::now();

        // Copy vertices from registry
        positions = m_vertexRegistry.getPositions();
        normals = m_vertexRegistry.getNormals();
        indices.clear();

        // Generate quads from shared edges
        generateQuads(indices);

        // Remove geometrically degenerate triangles (e.g., when two distinct vertex indices
        // end up at identical positions). These triangles contribute spurious boundary edges
        // but do not cover area.
        {
            float const domainScale = m_globalBboxSize.norm();
            float const eps = std::max(1e-7F * domainScale, 1e-9F);
            float const eps2 = eps * eps;
            float const areaEps = std::max(1e-10F * domainScale * domainScale, 1e-12F);

            std::vector<std::uint32_t> filtered;
            filtered.reserve(indices.size());
            std::size_t removed = 0U;

            for (std::size_t i = 0U; i + 2U < indices.size(); i += 3U)
            {
                std::uint32_t const a = indices[i + 0U];
                std::uint32_t const b = indices[i + 1U];
                std::uint32_t const c = indices[i + 2U];

                if (a == b || b == c || a == c)
                {
                    ++removed;
                    continue;
                }

                Eigen::Vector3f const& pa = positions[a];
                Eigen::Vector3f const& pb = positions[b];
                Eigen::Vector3f const& pc = positions[c];

                Eigen::Vector3f const ab = pb - pa;
                Eigen::Vector3f const bc = pc - pb;
                Eigen::Vector3f const ca = pa - pc;

                if (ab.squaredNorm() < eps2 || bc.squaredNorm() < eps2 || ca.squaredNorm() < eps2)
                {
                    ++removed;
                    continue;
                }

                float const area2 = (ab.cross(pc - pa)).squaredNorm();
                if (area2 < areaEps * areaEps)
                {
                    ++removed;
                    continue;
                }

                filtered.push_back(a);
                filtered.push_back(b);
                filtered.push_back(c);
            }

            if (removed > 0U)
            {
                indices.swap(filtered);
            }
        }

        // Note: Boundary hole filling is intentionally disabled here.
        // The current naive loop triangulation can introduce significant non-manifold
        // topology. Holes must be fixed by correct quad generation / neighbor completion.
        
        // Fix orientation of connected components using gradient voting
        fixTriangleOrientation(indices);
        
        // Post-fix directed edge analysis
        {
            std::unordered_map<std::uint64_t, int> directedEdgeCount;
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                std::uint32_t const v0 = indices[i + 0];
                std::uint32_t const v1 = indices[i + 1];
                std::uint32_t const v2 = indices[i + 2];
                
                auto makeDirectedKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t {
                    return (static_cast<std::uint64_t>(a) << 32) | b;
                };
                
                ++directedEdgeCount[makeDirectedKey(v0, v1)];
                ++directedEdgeCount[makeDirectedKey(v1, v2)];
                ++directedEdgeCount[makeDirectedKey(v2, v0)];
            }
            
            std::size_t pairedEdges = 0, singleEdges = 0, multipleEdges = 0;
            for (auto const& [edgeKey, count] : directedEdgeCount)
            {
                std::uint32_t const a = static_cast<std::uint32_t>(edgeKey >> 32);
                std::uint32_t const b = static_cast<std::uint32_t>(edgeKey & 0xFFFFFFFF);
                std::uint64_t const reverseKey = (static_cast<std::uint64_t>(b) << 32) | a;
                
                if (count > 1)
                {
                    ++multipleEdges;
                }
                else if (directedEdgeCount.count(reverseKey) && directedEdgeCount.at(reverseKey) == 1)
                {
                    ++pairedEdges;
                }
                else
                {
                    ++singleEdges;
                }
            }
            
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  POST-FIX directed edge analysis: paired=" << (pairedEdges / 2) 
                      << ", single=" << singleEdges 
                      << ", multiple=" << multipleEdges << std::endl;
                      #endif
        }

        m_stats.triangleCount = indices.size() / 3U;

        auto const endTime = std::chrono::high_resolution_clock::now();
        m_stats.meshExtractionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        // Validate mesh
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeCount;
        for (std::size_t i = 0U; i < indices.size(); i += 3U)
        {
            for (int e = 0; e < 3; ++e)
            {
                std::uint32_t v0 = indices[i + e];
                std::uint32_t v1 = indices[i + (e + 1) % 3];
                auto edge = std::minmax(v0, v1);
                edgeCount[edge]++;
            }
        }

        m_stats.boundaryEdges = 0U;
        m_stats.nonManifoldEdges = 0U;
        std::size_t internalEdges = 0U;
        std::size_t nonManifold3 = 0U, nonManifold4 = 0U, nonManifoldMore = 0U;
        
        // Track which triangle each edge comes from for debugging
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edgeToTriangles;
        for (std::size_t i = 0U; i < indices.size(); i += 3U)
        {
            std::size_t triIdx = i / 3U;
            for (int e = 0; e < 3; ++e)
            {
                std::uint32_t v0 = indices[i + e];
                std::uint32_t v1 = indices[i + (e + 1) % 3];
                auto edge = std::minmax(v0, v1);
                edgeToTriangles[edge].push_back(triIdx);
            }
        }
        
        // Debug: print first few problematic edges
        std::size_t debugCount = 0;
        std::size_t boundaryDebugCount = 0;
        for (auto const& [edge, triangles] : edgeToTriangles)
        {
            std::size_t count = triangles.size();
            if (count == 1)
            {
                ++m_stats.boundaryEdges;

                if (boundaryDebugCount < 12)
                {
                    float const edgeLen = (positions[edge.first] - positions[edge.second]).norm();
                    std::cout << "  Boundary edge (" << edge.first << "," << edge.second
                              << ") len=" << edgeLen << " tri=" << triangles.front()
                              << " [" << indices[triangles.front() * 3] << "," << indices[triangles.front() * 3 + 1]
                              << "," << indices[triangles.front() * 3 + 2] << "]" << std::endl;
                    ++boundaryDebugCount;
                }
            }
            else if (count == 2)
            {
                ++internalEdges;
            }
            else if (count > 2)
            {
                ++m_stats.nonManifoldEdges;
                if (count == 3) ++nonManifold3;
                else if (count == 4) ++nonManifold4;
                else ++nonManifoldMore;
                
                // Debug: print details for first few non-manifold edges
                if (debugCount < 12)
                {
                    float const edgeLen = (positions[edge.first] - positions[edge.second]).norm();
                    std::cout << "  Non-manifold edge (" << edge.first << "," << edge.second
                              << ") count=" << count
                              << " len=" << edgeLen
                              << (edge.first == edge.second ? " [DEGENERATE]" : "")
                              << std::endl;
                    for (auto t : triangles)
                    {
                        std::size_t quadIdx = t / 2;
                        bool isDiagonal = (t % 2 == 0) ? 
                            (indices[t*3] == edge.first && indices[t*3+2] == edge.second) ||
                            (indices[t*3] == edge.second && indices[t*3+2] == edge.first) :
                            (indices[t*3] == edge.first && indices[t*3+2] == edge.second) ||
                            (indices[t*3] == edge.second && indices[t*3+2] == edge.first);
                        (void)quadIdx;
                        (void)isDiagonal;
                    }
                    std::cout << "    Triangles: ";
                    for (auto t : triangles)
                    {
                        std::cout << t << "[" << indices[t*3] << "," << indices[t*3+1] << "," << indices[t*3+2] << "] ";
                    }
                    std::cout << std::endl;
                    ++debugCount;
                }
            }
        }

        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "Mesh extraction complete:" << std::endl;
        std::cout << "  Edge analysis: internal=" << internalEdges 
                  << ", boundary=" << m_stats.boundaryEdges 
                  << ", non-manifold=" << m_stats.nonManifoldEdges 
                  << " (3x=" << nonManifold3 << ", 4x=" << nonManifold4 << ", 5+x=" << nonManifoldMore << ")" << std::endl;
        std::cout << "  Vertices: " << positions.size() << std::endl;
        std::cout << "  Triangles: " << m_stats.triangleCount << std::endl;
        std::cout << "  Boundary edges: " << m_stats.boundaryEdges << std::endl;
        std::cout << "  Non-manifold edges: " << m_stats.nonManifoldEdges << std::endl;
        std::cout << "  Time: " << m_stats.meshExtractionTimeMs << " ms" << std::endl;
        #endif
    }

    void GlobalMortonOctree::generateQuads(std::vector<std::uint32_t>& indices)
    {
        // NEW APPROACH: Explicit neighbor lookup (like GPU emit_indices)
        // For each intersecting leaf, check 3 "owned" edges (3, 7, 11 - edges at max corner).
        // NOTE: GPU uses different edge numbering (6, 5, 10), but CPU EDGE_CORNERS uses:
        //   Edge 3: X-aligned at y=max, z=max (corners 6-7)
        //   Edge 7: Y-aligned at x=max, z=max (corners 5-7)
        //   Edge 11: Z-aligned at x=max, y=max (corners 3-7)
        // For each edge with sign-change, explicitly look up the 3 neighbors that share it.
        // This guarantees we find all 4 cells if they exist.

        auto const& positions = m_vertexRegistry.getPositions();
        
        std::size_t intersectingLeafCount = 0;
        std::size_t emittedQuads = 0;
        std::size_t halfDegenerateQuads = 0;
        std::size_t boundaryEdges = 0;  // Edges where neighbors don't exist
        std::size_t missingVertexQuads = 0;  // Quads where a cell lacks a vertex

        // Guard against accidental duplicate quad emission for the same geometric edge.
        // If an edge is emitted more than once, shared perimeter edges can become non-manifold.
        std::unordered_set<std::uint64_t> emittedOwnedEdges;
        std::size_t duplicateOwnedEdgesSkipped = 0U;

        auto makeOwnedEdgeKey = [](std::uint8_t axis,
                                   std::uint32_t ex,
                                   std::uint32_t ey,
                                   std::uint32_t ez,
                                   std::uint8_t depth) -> std::uint64_t
        {
            // Pack (depth, axis, ex, ey, ez) into a 64-bit key.
            // Coordinates are grid-vertex coordinates in [0, 2^depth].
            return static_cast<std::uint64_t>(depth) |
                   (static_cast<std::uint64_t>(axis) << 8U) |
                   (static_cast<std::uint64_t>(ex) << 10U) |
                   (static_cast<std::uint64_t>(ey) << 26U) |
                   (static_cast<std::uint64_t>(ez) << 42U);
        };
        
        // Build Morton→nodeIdx map for fast lookup at uniform depth.
        // Note: neighbors required for quad closure may be non-intersecting but still have a
        // "halo" vertex, so we include any leaf that has a vertex.
        std::unordered_map<std::uint64_t, std::size_t> mortonToNode;
        std::map<std::uint8_t, std::size_t> depthDistribution;
        std::uint8_t minIntersectDepth = std::numeric_limits<std::uint8_t>::max();
        std::uint8_t maxIntersectDepth = 0U;
        for (std::size_t nodeIdx = 0U; nodeIdx < m_nodes.size(); ++nodeIdx)
        {
            auto const& node = m_nodes[nodeIdx];
            if (node.isLeaf && !node.vertexIndices.empty())
            {
                mortonToNode[node.mortonCode] = nodeIdx;
                if (node.isIntersecting)
                {
                    ++depthDistribution[node.depth];
                    minIntersectDepth = std::min(minIntersectDepth, node.depth);
                    maxIntersectDepth = std::max(maxIntersectDepth, node.depth);
                }
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  mortonToNode map size: " << mortonToNode.size() << std::endl;
        std::cout << "  Intersecting leaf depth distribution:" << std::endl;
        #endif
        for (auto const& [depth, count] : depthDistribution)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "    Depth " << static_cast<int>(depth) << ": " << count << " leaves" << std::endl;
            #endif
        }
        
        // Debug: verify Morton encoding/decoding round-trip
        std::size_t roundTripErrors = 0;
        for (std::size_t nodeIdx = 0U; nodeIdx < m_nodes.size(); ++nodeIdx)
        {
            auto const& node = m_nodes[nodeIdx];
            if (!node.isLeaf || !node.isIntersecting) continue;
            
            // Decode coordinates
            std::uint32_t x = 0U, y = 0U, z = 0U;
            for (std::uint8_t d = 0U; d < node.depth; ++d)
            {
                auto const shift = (node.depth - 1U - d) * 3U;
                auto const octant = static_cast<std::uint8_t>((node.mortonCode >> shift) & 0x7U);
                auto const ox = (octant >> 0U) & 1U;
                auto const oy = (octant >> 1U) & 1U;
                auto const oz = (octant >> 2U) & 1U;
                auto const levelScale = 1U << (node.depth - 1U - d);
                x += ox * levelScale;
                y += oy * levelScale;
                z += oz * levelScale;
            }
            
            // Re-encode
            std::uint64_t reencoded = 0U;
            for (std::uint8_t d = 0U; d < node.depth; ++d)
            {
                auto const shift = node.depth - 1U - d;
                auto const ox = (x >> shift) & 1U;
                auto const oy = (y >> shift) & 1U;
                auto const oz = (z >> shift) & 1U;
                reencoded = (reencoded << 3U) | (ox | (oy << 1U) | (oz << 2U));
            }
            
            if (reencoded != node.mortonCode && roundTripErrors < 5)
            {
                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                std::cout << "  Round-trip error: node " << nodeIdx 
                          << " mc=" << std::hex << node.mortonCode 
                          << " -> (" << std::dec << x << "," << y << "," << z 
                          << ") -> " << std::hex << reencoded << std::dec << std::endl;
                          #endif
                ++roundTripErrors;
            }
        }
        if (roundTripErrors > 0)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  Total round-trip errors: " << roundTripErrors << std::endl;
            #endif
        }
        
        // Helper to get vertex index from a node for a specific edge (returns max if no vertex)
        auto getVertexFromNodeForEdge = [this](std::size_t nodeIdx, std::uint8_t edgeNum) -> std::uint32_t {
            if (nodeIdx >= m_nodes.size())
            {
                return std::numeric_limits<std::uint32_t>::max();
            }

            auto const& node = m_nodes[nodeIdx];
            if (node.vertexIndices.empty())
            {
                return std::numeric_limits<std::uint32_t>::max();
            }

            if (edgeNum >= node.edgeComponents.size())
            {
                return std::numeric_limits<std::uint32_t>::max();
            }

            std::uint8_t const component = node.edgeComponents[edgeNum];
            if (component >= node.vertexIndices.size())
            {
                // Do not silently fall back to component 0: that can connect unrelated
                // components and create non-manifold topology.
                return std::numeric_limits<std::uint32_t>::max();
            }
            return node.vertexIndices[component];
        };

        auto triangleArea = [&positions](std::uint32_t a, std::uint32_t b, std::uint32_t c) -> float {
            Eigen::Vector3f const& pa = positions[a];
            Eigen::Vector3f const& pb = positions[b];
            Eigen::Vector3f const& pc = positions[c];
            return (pb - pa).cross(pc - pa).norm();
        };

        auto makeUndirectedEdgeKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t
        {
            if (a > b)
            {
                std::swap(a, b);
            }
            return (static_cast<std::uint64_t>(a) << 32U) | b;
        };

        struct QuadCandidate
        {
            std::uint8_t axis;
            std::array<std::uint32_t, 6> diag12Tris;
            std::array<std::uint32_t, 6> diag03Tris;
            float area12a;
            float area12b;
            float area03a;
            float area03b;
            std::uint64_t diag12Key;
            std::uint64_t diag03Key;
            std::array<std::uint64_t, 4> perimeterEdges;
        };

        auto computePerimeterEdgesFromTris = [&](std::array<std::uint32_t, 6> const& tris) -> std::array<std::uint64_t, 4>
        {
            // The two-triangle representation contains 6 undirected edges where the internal
            // diagonal appears twice. The remaining 4 unique edges are the quad perimeter.
            std::array<std::uint64_t, 6> edges = {
              makeUndirectedEdgeKey(tris[0], tris[1]),
              makeUndirectedEdgeKey(tris[1], tris[2]),
              makeUndirectedEdgeKey(tris[2], tris[0]),
              makeUndirectedEdgeKey(tris[3], tris[4]),
              makeUndirectedEdgeKey(tris[4], tris[5]),
              makeUndirectedEdgeKey(tris[5], tris[3]),
            };

            std::array<std::uint64_t, 6> uniqueEdges{};
            std::array<std::uint8_t, 6> uniqueCounts{};
            std::size_t uniqueCount = 0U;

            for (std::size_t i = 0U; i < edges.size(); ++i)
            {
                bool found = false;
                for (std::size_t j = 0U; j < uniqueCount; ++j)
                {
                    if (uniqueEdges[j] == edges[i])
                    {
                        ++uniqueCounts[j];
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    uniqueEdges[uniqueCount] = edges[i];
                    uniqueCounts[uniqueCount] = 1U;
                    ++uniqueCount;
                }
            }

            std::array<std::uint64_t, 4> perimeter{};
            std::size_t out = 0U;
            for (std::size_t j = 0U; j < uniqueCount; ++j)
            {
                if (uniqueCounts[j] == 1U)
                {
                    if (out < perimeter.size())
                    {
                        perimeter[out++] = uniqueEdges[j];
                    }
                }
            }
            return perimeter;
        };

        auto computeDiagonalEdgeFromTris = [&](std::array<std::uint32_t, 6> const& tris) -> std::uint64_t
        {
            std::array<std::uint64_t, 6> edges = {
              makeUndirectedEdgeKey(tris[0], tris[1]),
              makeUndirectedEdgeKey(tris[1], tris[2]),
              makeUndirectedEdgeKey(tris[2], tris[0]),
              makeUndirectedEdgeKey(tris[3], tris[4]),
              makeUndirectedEdgeKey(tris[4], tris[5]),
              makeUndirectedEdgeKey(tris[5], tris[3]),
            };

            std::array<std::uint64_t, 6> uniqueEdges{};
            std::array<std::uint8_t, 6> uniqueCounts{};
            std::size_t uniqueCount = 0U;

            for (std::size_t i = 0U; i < edges.size(); ++i)
            {
                bool found = false;
                for (std::size_t j = 0U; j < uniqueCount; ++j)
                {
                    if (uniqueEdges[j] == edges[i])
                    {
                        ++uniqueCounts[j];
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    uniqueEdges[uniqueCount] = edges[i];
                    uniqueCounts[uniqueCount] = 1U;
                    ++uniqueCount;
                }
            }

            for (std::size_t j = 0U; j < uniqueCount; ++j)
            {
                if (uniqueCounts[j] == 2U)
                {
                    return uniqueEdges[j];
                }
            }
            return 0U;
        };

        // First pass: gather all valid quads (candidates) and build an order-independent view
        // of which edges will be used as perimeter edges.
        std::vector<QuadCandidate> candidates;
        candidates.reserve(160000U);
        
        auto addCandidate = [&](std::uint8_t axis,
                                std::array<std::uint32_t, 6> const& diag12,
                                std::array<std::uint32_t, 6> const& diag03)
        {
            float constexpr MIN_AREA = 1e-10F;
            QuadCandidate c;
            c.axis = axis;
            c.diag12Tris = diag12;
            c.diag03Tris = diag03;
            c.area12a = triangleArea(diag12[0], diag12[1], diag12[2]);
            c.area12b = triangleArea(diag12[3], diag12[4], diag12[5]);
            c.area03a = triangleArea(diag03[0], diag03[1], diag03[2]);
            c.area03b = triangleArea(diag03[3], diag03[4], diag03[5]);
            c.diag12Key = computeDiagonalEdgeFromTris(diag12);
            c.diag03Key = computeDiagonalEdgeFromTris(diag03);
            c.perimeterEdges = computePerimeterEdgesFromTris(diag12);

            // Only keep candidates with 4 perimeter edges (defensive: should always be 4).
            // If perimeterEdges contains zeros due to unexpected topology, we still keep it;
            // it will just weaken the diagonal-vs-perimeter check rather than crashing.
            candidates.push_back(c);
            (void)MIN_AREA;
        };

        for (std::size_t nodeIdx = 0U; nodeIdx < m_nodes.size(); ++nodeIdx)
        {
            auto const& node = m_nodes[nodeIdx];
            if (!node.isLeaf || !node.isIntersecting || node.vertexIndices.empty())
            {
                continue;
            }
            ++intersectingLeafCount;

            // Decode this cell's coordinates
            std::uint32_t cx = 0U, cy = 0U, cz = 0U;
            for (std::uint8_t d = 0U; d < node.depth; ++d)
            {
                auto const shift = (node.depth - 1U - d) * 3U;
                auto const octant = static_cast<std::uint8_t>((node.mortonCode >> shift) & 0x7U);
                auto const ox = (octant >> 0U) & 1U;
                auto const oy = (octant >> 1U) & 1U;
                auto const oz = (octant >> 2U) & 1U;
                auto const levelScale = 1U << (node.depth - 1U - d);
                cx += ox * levelScale;
                cy += oy * levelScale;
                cz += oz * levelScale;
            }

            auto const maxCoord = (1U << node.depth) - 1U;

            // Check owned edges 3, 7, 11 (edges at max corner)
            // CPU EDGE_CORNERS uses different numbering than GPU:
            //   CPU Edge 3 = GPU Edge 6: X-axis at (y=max, z=max), corners 6-7
            // Shared by: (x,y,z), (x,y+1,z), (x,y,z+1), (x,y+1,z+1)
            if ((node.edgeMask & (1U << 3)) && cy < maxCoord && cz < maxCoord)
            {
                // Unique key for this geometric edge (axis X) at grid vertex coordinate (cx, cy+1, cz+1)
                {
                    std::uint64_t const edgeKey = makeOwnedEdgeKey(0U, cx, cy + 1U, cz + 1U, node.depth);
                    if (!emittedOwnedEdges.insert(edgeKey).second)
                    {
                        ++duplicateOwnedEdgesSkipped;
                        continue;
                    }
                }

                // Look up 3 neighbors by coordinate
                auto encodeMorton = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint64_t {
                    std::uint64_t code = 0U;
                    for (std::uint8_t d = 0U; d < node.depth; ++d)
                    {
                        auto const shift = node.depth - 1U - d;
                        auto const ox = (x >> shift) & 1U;
                        auto const oy = (y >> shift) & 1U;
                        auto const oz = (z >> shift) & 1U;
                        code = (code << 3U) | (ox | (oy << 1U) | (oz << 2U));
                    }
                    return code;
                };

                std::uint64_t const m1 = encodeMorton(cx, cy + 1, cz);
                std::uint64_t const m2 = encodeMorton(cx, cy, cz + 1);
                std::uint64_t const m3 = encodeMorton(cx, cy + 1, cz + 1);

                auto it1 = mortonToNode.find(m1);
                auto it2 = mortonToNode.find(m2);
                auto it3 = mortonToNode.find(m3);

                bool const foundAll = (it1 != mortonToNode.end() && it2 != mortonToNode.end() && it3 != mortonToNode.end());
                if (!foundAll)
                {
                    ++boundaryEdges;

                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    // Track which neighbors are missing and why
                    static std::size_t sampleCount = 0;
                    if (sampleCount++ < 5)
                    {
                        std::uint8_t const depth = node.depth;
                        // Check if nodes exist in the full octree but aren't intersecting
                        auto checkNode = [this, depth](std::uint64_t mc, char const* name)
                        {
                            auto it = m_mortonToIndex.find(MortonNodeKey{mc, depth});
                            if (it == m_mortonToIndex.end())
                            {
                                std::cout << "    " << name << " mc=" << std::hex << mc << std::dec
                                          << ": NOT IN OCTREE" << std::endl;
                            }
                            else
                            {
                                auto const& n = m_nodes[it->second];
                                std::cout << "    " << name << " mc=" << std::hex << mc << std::dec
                                          << ": exists, isLeaf=" << n.isLeaf
                                          << " isIntersecting=" << n.isIntersecting << std::endl;
                            }
                        };

                        std::cout << "  Edge3 neighbor miss at (" << cx << "," << cy << "," << cz
                                  << ") depth=" << static_cast<int>(node.depth)
                                  << " mc=" << std::hex << node.mortonCode << std::dec
                                  << ": n1(" << (it1 != mortonToNode.end())
                                  << ") n2(" << (it2 != mortonToNode.end())
                                  << ") n3(" << (it3 != mortonToNode.end()) << ")" << std::endl;
                        std::cout << "    Lookup codes: m1=" << std::hex << m1
                                  << " m2=" << m2 << " m3=" << m3 << std::dec << std::endl;

                        checkNode(m1, "n1");
                        checkNode(m2, "n2");
                        checkNode(m3, "n3");
                    }
                    #endif
                }
                else
                {
                    // NOTE: Each of the 4 cells around this geometric edge refers to it with
                    // a different local edge index. This matters for multi-vertex/component
                    // selection.
                    // Geometric edge = X-axis at (y=max,z=max) for base cell (edge 3).
                    // Neighbors use: (y=min,z=max)=edge2, (y=max,z=min)=edge1, (y=min,z=min)=edge0.
                    // Order vertices cyclically around the edge in the (y,z) neighborhood plane:
                    //   (cy,cz) -> (cy+1,cz) -> (cy+1,cz+1) -> (cy,cz+1)
                    std::uint32_t v0 = getVertexFromNodeForEdge(nodeIdx, 3U);
                    std::uint32_t v1 = getVertexFromNodeForEdge(it1->second, 2U);
                    std::uint32_t v2 = getVertexFromNodeForEdge(it3->second, 0U);
                    std::uint32_t v3 = getVertexFromNodeForEdge(it2->second, 1U);

                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        // Check for duplicate vertices
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            // Two triangulations of the quad:
                            //  - diagonal (v0,v2): (v0,v1,v2) + (v0,v2,v3)
                            //  - diagonal (v1,v3): (v0,v1,v3) + (v1,v2,v3)
                            std::array<std::uint32_t, 6> diag12{v0, v1, v2,  v0, v2, v3};
                            std::array<std::uint32_t, 6> diag03{v0, v1, v3,  v1, v2, v3};

                            addCandidate(0U, diag12, diag03);
                        }
                    }
                    else
                    {
                        ++missingVertexQuads;
                    }
                }  // closes foundAll
            }
            
            // CPU Edge 7 = GPU Edge 5: Y-axis at (x=max, z=max), corners 5-7
            // Shared by: (x,y,z), (x+1,y,z), (x,y,z+1), (x+1,y,z+1)
            if ((node.edgeMask & (1U << 7)) && cx < maxCoord && cz < maxCoord)
            {
                // Unique key for this geometric edge (axis Y) at grid vertex coordinate (cx+1, cy, cz+1)
                {
                    std::uint64_t const edgeKey = makeOwnedEdgeKey(1U, cx + 1U, cy, cz + 1U, node.depth);
                    if (!emittedOwnedEdges.insert(edgeKey).second)
                    {
                        ++duplicateOwnedEdgesSkipped;
                        continue;
                    }
                }

                auto encodeMorton = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint64_t {
                    std::uint64_t code = 0U;
                    for (std::uint8_t d = 0U; d < node.depth; ++d)
                    {
                        auto const shift = node.depth - 1U - d;
                        auto const ox = (x >> shift) & 1U;
                        auto const oy = (y >> shift) & 1U;
                        auto const oz = (z >> shift) & 1U;
                        code = (code << 3U) | (ox | (oy << 1U) | (oz << 2U));
                    }
                    return code;
                };
                
                std::uint64_t const m1 = encodeMorton(cx + 1, cy, cz);
                std::uint64_t const m2 = encodeMorton(cx, cy, cz + 1);
                std::uint64_t const m3 = encodeMorton(cx + 1, cy, cz + 1);
                
                auto it1 = mortonToNode.find(m1);
                auto it2 = mortonToNode.find(m2);
                auto it3 = mortonToNode.find(m3);
                
                if (it1 != mortonToNode.end() && it2 != mortonToNode.end() && it3 != mortonToNode.end())
                {
                    // Geometric edge = Y-axis at (x=max,z=max) for base cell (edge 7).
                    // Neighbors use: (x=min,z=max)=edge6, (x=max,z=min)=edge5, (x=min,z=min)=edge4.
                    // Order vertices cyclically around the edge in the (x,z) neighborhood plane:
                    //   (cx,cz) -> (cx+1,cz) -> (cx+1,cz+1) -> (cx,cz+1)
                    std::uint32_t v0 = getVertexFromNodeForEdge(nodeIdx, 7U);
                    std::uint32_t v1 = getVertexFromNodeForEdge(it1->second, 6U);
                    std::uint32_t v2 = getVertexFromNodeForEdge(it3->second, 4U);
                    std::uint32_t v3 = getVertexFromNodeForEdge(it2->second, 5U);
                    
                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            std::array<std::uint32_t, 6> diag12{v0, v1, v2,  v0, v2, v3};
                            std::array<std::uint32_t, 6> diag03{v0, v1, v3,  v1, v2, v3};

                            addCandidate(1U, diag12, diag03);
                        }
                    }
                    else
                    {
                        ++missingVertexQuads;
                    }
                }
                else
                {
                    ++boundaryEdges;
                }
            }
            
            // CPU Edge 11 = GPU Edge 10: Z-axis at (x=max, y=max), corners 3-7
            // Shared by: (x,y,z), (x+1,y,z), (x,y+1,z), (x+1,y+1,z)
            if ((node.edgeMask & (1U << 11)) && cx < maxCoord && cy < maxCoord)
            {
                // Unique key for this geometric edge (axis Z) at grid vertex coordinate (cx+1, cy+1, cz)
                {
                    std::uint64_t const edgeKey = makeOwnedEdgeKey(2U, cx + 1U, cy + 1U, cz, node.depth);
                    if (!emittedOwnedEdges.insert(edgeKey).second)
                    {
                        ++duplicateOwnedEdgesSkipped;
                        continue;
                    }
                }

                auto encodeMorton = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> std::uint64_t {
                    std::uint64_t code = 0U;
                    for (std::uint8_t d = 0U; d < node.depth; ++d)
                    {
                        auto const shift = node.depth - 1U - d;
                        auto const ox = (x >> shift) & 1U;
                        auto const oy = (y >> shift) & 1U;
                        auto const oz = (z >> shift) & 1U;
                        code = (code << 3U) | (ox | (oy << 1U) | (oz << 2U));
                    }
                    return code;
                };
                
                std::uint64_t const m1 = encodeMorton(cx + 1, cy, cz);
                std::uint64_t const m2 = encodeMorton(cx, cy + 1, cz);
                std::uint64_t const m3 = encodeMorton(cx + 1, cy + 1, cz);
                
                auto it1 = mortonToNode.find(m1);
                auto it2 = mortonToNode.find(m2);
                auto it3 = mortonToNode.find(m3);
                
                if (it1 != mortonToNode.end() && it2 != mortonToNode.end() && it3 != mortonToNode.end())
                {
                    // Geometric edge = Z-axis at (x=max,y=max) for base cell (edge 11).
                    // Neighbors use: (x=min,y=max)=edge10, (x=max,y=min)=edge9, (x=min,y=min)=edge8.
                    // Order vertices cyclically around the edge in the (x,y) neighborhood plane:
                    //   (cx,cy) -> (cx+1,cy) -> (cx+1,cy+1) -> (cx,cy+1)
                    std::uint32_t v0 = getVertexFromNodeForEdge(nodeIdx, 11U);
                    std::uint32_t v1 = getVertexFromNodeForEdge(it1->second, 10U);
                    std::uint32_t v2 = getVertexFromNodeForEdge(it3->second, 8U);
                    std::uint32_t v3 = getVertexFromNodeForEdge(it2->second, 9U);
                    
                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            std::array<std::uint32_t, 6> diag12{v0, v1, v2,  v0, v2, v3};
                            std::array<std::uint32_t, 6> diag03{v0, v1, v3,  v1, v2, v3};

                            addCandidate(2U, diag12, diag03);
                        }
                    }
                    else
                    {
                        ++missingVertexQuads;
                    }
                }
                else
                {
                    ++boundaryEdges;
                }
            }
        }

        // Build an order-independent map of all perimeter edges that will appear regardless
        // of quad diagonal choices. If a quad diagonal equals any perimeter edge, that edge
        // would be used by 3+ triangles (2 from the diagonal + at least 1 from perimeter),
        // creating non-manifold topology.
        std::unordered_map<std::uint64_t, std::uint16_t> perimeterEdgeCount;
        perimeterEdgeCount.reserve(candidates.size() * 4U);

        // Collect small diagnostics about which edge-axis types contribute to each perimeter edge.
        std::unordered_map<std::uint64_t, std::array<std::uint8_t, 3>> perimeterEdgeAxes;
        std::unordered_map<std::uint64_t, std::uint8_t> perimeterEdgeAxesCount;
        perimeterEdgeAxes.reserve(candidates.size() * 4U);
        perimeterEdgeAxesCount.reserve(candidates.size() * 4U);

        for (auto const& c : candidates)
        {
            for (auto const e : c.perimeterEdges)
            {
                // Skip zeros (defensive).
                if (e == 0U)
                {
                    continue;
                }
                ++perimeterEdgeCount[e];

                auto& axes = perimeterEdgeAxes[e];
                std::uint8_t& axesCount = perimeterEdgeAxesCount[e];
                if (axesCount < axes.size())
                {
                    axes[axesCount++] = c.axis;
                }
            }
        }

        std::size_t perimeterEdgesOverused = 0U;
        std::uint16_t maxPerimeterEdgeUse = 0U;
        for (auto const& [edgeKey, count] : perimeterEdgeCount)
        {
            maxPerimeterEdgeUse = std::max(maxPerimeterEdgeUse, count);
            if (count > 2U)
            {
                ++perimeterEdgesOverused;
            }
        }

        if (perimeterEdgesOverused > 0U)
        {
            std::size_t printed = 0U;
            for (auto const& [edgeKey, count] : perimeterEdgeCount)
            {
                if (count <= 2U)
                {
                    continue;
                }

                auto const axesIt = perimeterEdgeAxes.find(edgeKey);
                auto const axesCountIt = perimeterEdgeAxesCount.find(edgeKey);

                std::uint8_t const axesCount = (axesCountIt != perimeterEdgeAxesCount.end()) ? axesCountIt->second : 0U;
                std::array<std::uint8_t, 3> axes{};
                if (axesIt != perimeterEdgeAxes.end())
                {
                    axes = axesIt->second;
                }

                std::cout << "  Overused perimeter edge key=0x" << std::hex << edgeKey << std::dec
                          << " count=" << count
                          << " axes=[";
                for (std::uint8_t i = 0U; i < axesCount; ++i)
                {
                    std::cout << static_cast<int>(axes[i]);
                    if (i + 1U < axesCount)
                    {
                        std::cout << ",";
                    }
                }
                std::cout << "]" << std::endl;

                if (++printed >= 6U)
                {
                    break;
                }
            }
        }

        // Track current undirected edge usage in the generated triangle mesh.
        // We still use this (order-dependent) to avoid diagonal-diagonal collisions.
        std::unordered_map<std::uint64_t, std::uint8_t> edgeUseCount;
        edgeUseCount.reserve(512000U);

        auto optionNonManifoldPenalty = [&](std::array<std::uint32_t, 6> const& tris) -> std::uint32_t
        {
            // Count per-edge increments for this option (6 edges per triangle pair, with shared
            // diagonal appearing twice).
            std::array<std::uint64_t, 6> edges = {
              makeUndirectedEdgeKey(tris[0], tris[1]),
              makeUndirectedEdgeKey(tris[1], tris[2]),
              makeUndirectedEdgeKey(tris[2], tris[0]),
              makeUndirectedEdgeKey(tris[3], tris[4]),
              makeUndirectedEdgeKey(tris[4], tris[5]),
              makeUndirectedEdgeKey(tris[5], tris[3]),
            };

            std::array<std::uint64_t, 6> uniqueEdges{};
            std::array<std::uint8_t, 6> inc{};
            std::size_t uniqueCount = 0U;
            for (std::size_t i = 0U; i < edges.size(); ++i)
            {
                bool found = false;
                for (std::size_t j = 0U; j < uniqueCount; ++j)
                {
                    if (uniqueEdges[j] == edges[i])
                    {
                        ++inc[j];
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    uniqueEdges[uniqueCount] = edges[i];
                    inc[uniqueCount] = 1U;
                    ++uniqueCount;
                }
            }

            std::uint32_t penalty = 0U;
            for (std::size_t j = 0U; j < uniqueCount; ++j)
            {
                auto const it = edgeUseCount.find(uniqueEdges[j]);
                std::uint8_t const current = (it != edgeUseCount.end()) ? it->second : 0U;
                std::uint8_t const next = static_cast<std::uint8_t>(current + inc[j]);
                if (next > 2U)
                {
                    penalty += static_cast<std::uint32_t>(next - 2U);
                }
            }
            return penalty;
        };

        auto commitOption = [&](std::array<std::uint32_t, 6> const& tris)
        {
            indices.push_back(tris[0]);
            indices.push_back(tris[1]);
            indices.push_back(tris[2]);
            indices.push_back(tris[3]);
            indices.push_back(tris[4]);
            indices.push_back(tris[5]);

            auto bump = [&](std::uint32_t a, std::uint32_t b)
            {
                std::uint64_t const k = makeUndirectedEdgeKey(a, b);
                auto it = edgeUseCount.find(k);
                if (it == edgeUseCount.end())
                {
                    edgeUseCount.emplace(k, 1U);
                }
                else
                {
                    it->second = static_cast<std::uint8_t>(it->second + 1U);
                }
            };

            bump(tris[0], tris[1]);
            bump(tris[1], tris[2]);
            bump(tris[2], tris[0]);
            bump(tris[3], tris[4]);
            bump(tris[4], tris[5]);
            bump(tris[5], tris[3]);
        };

        // Second pass: choose diagonals in a way that is order-independent w.r.t.
        // diagonal-vs-perimeter collisions.
        float constexpr MIN_AREA = 1e-10F;
        std::size_t zeroDiagonalKeys = 0U;
        for (auto const& c : candidates)
        {
            if (c.diag12Key == 0U || c.diag03Key == 0U)
            {
                ++zeroDiagonalKeys;
            }

            bool const diag12Ok = (c.area12a >= MIN_AREA) && (c.area12b >= MIN_AREA);
            bool const diag03Ok = (c.area03a >= MIN_AREA) && (c.area03b >= MIN_AREA);

            bool useDiag12 = diag12Ok;
            if (!diag12Ok && diag03Ok)
            {
                useDiag12 = false;
            }
            else if (diag12Ok && diag03Ok)
            {
                useDiag12 = std::min(c.area12a, c.area12b) >= std::min(c.area03a, c.area03b);
            }

            // Hard rule: never pick a diagonal that equals any perimeter edge in the full quad set.
            bool const diag12IsPerimeter = perimeterEdgeCount.find(c.diag12Key) != perimeterEdgeCount.end();
            bool const diag03IsPerimeter = perimeterEdgeCount.find(c.diag03Key) != perimeterEdgeCount.end();
            if (diag12IsPerimeter != diag03IsPerimeter)
            {
                useDiag12 = !diag12IsPerimeter;
            }

            // If both are (unfortunately) forbidden or both allowed, fall back to incremental
            // non-manifold penalty to avoid diagonal-diagonal collisions.
            std::array<std::uint32_t, 6> const& diag12 = c.diag12Tris;
            std::array<std::uint32_t, 6> const& diag03 = c.diag03Tris;
            std::uint32_t const penalty12 = optionNonManifoldPenalty(diag12);
            std::uint32_t const penalty03 = optionNonManifoldPenalty(diag03);
            if (penalty12 != penalty03)
            {
                useDiag12 = (penalty12 < penalty03);
            }

            auto const& chosen = useDiag12 ? diag12 : diag03;
            float const chosenAreaA = useDiag12 ? c.area12a : c.area03a;
            float const chosenAreaB = useDiag12 ? c.area12b : c.area03b;
            if (chosenAreaA < MIN_AREA || chosenAreaB < MIN_AREA)
            {
                ++halfDegenerateQuads;
            }

            commitOption(chosen);
            ++emittedQuads;
        }

        if (boundaryEdges > 0U || missingVertexQuads > 0U || halfDegenerateQuads > 0U || duplicateOwnedEdgesSkipped > 0U ||
            perimeterEdgesOverused > 0U || zeroDiagonalKeys > 0U)
        {
            std::cout << "  Quad generation (hierarchical): intersectingLeaves=" << intersectingLeafCount
                      << ", emittedQuads=" << emittedQuads
                      << ", neighborMisses=" << boundaryEdges
                      << ", missingVertexQuads=" << missingVertexQuads
                      << ", halfDegenerateQuads=" << halfDegenerateQuads
                      << ", duplicateOwnedEdgesSkipped=" << duplicateOwnedEdgesSkipped
                      << ", perimeterEdgesOverused=" << perimeterEdgesOverused
                      << ", maxPerimeterEdgeUse=" << maxPerimeterEdgeUse
                      << ", zeroDiagonalKeys=" << zeroDiagonalKeys
                      << ", depthRange=[" << static_cast<int>(minIntersectDepth)
                      << "," << static_cast<int>(maxIntersectDepth) << "]"
                      << std::endl;
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Quad generation (explicit neighbor lookup):" << std::endl;
        std::cout << "    Intersecting leaves: " << intersectingLeafCount << std::endl;
        std::cout << "    Emitted quads: " << emittedQuads << std::endl;
        std::cout << "    Half-degenerate quads: " << halfDegenerateQuads << std::endl;
        std::cout << "    Boundary edges (neighbors missing): " << boundaryEdges << std::endl;
        std::cout << "    Missing vertex quads: " << missingVertexQuads << std::endl;
        std::cout << "    Triangles: " << (indices.size() / 3) << std::endl;
        
        #endif
        // Debug: check for degenerate triangles
        std::size_t degenerateCount = 0;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            if (indices[i] == indices[i+1] || indices[i+1] == indices[i+2] || indices[i] == indices[i+2])
            {
                ++degenerateCount;
            }
        }
        if (degenerateCount > 0)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "    WARNING: " << degenerateCount << " degenerate triangles!" << std::endl;
            #endif
        }
        
        // Debug: Check directed edge consistency
        std::unordered_map<std::uint64_t, int> directedEdgeCount;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            std::uint32_t const v0 = indices[i + 0];
            std::uint32_t const v1 = indices[i + 1];
            std::uint32_t const v2 = indices[i + 2];
            
            auto makeDirectedKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t {
                return (static_cast<std::uint64_t>(a) << 32) | b;
            };
            
            ++directedEdgeCount[makeDirectedKey(v0, v1)];
            ++directedEdgeCount[makeDirectedKey(v1, v2)];
            ++directedEdgeCount[makeDirectedKey(v2, v0)];
        }
        
        std::size_t pairedEdges = 0;   // Edge appears once, opposite appears once
        std::size_t singleEdges = 0;   // Edge appears once, no opposite
        std::size_t sameDirectionEdges = 0;  // Edge appears but so does its opposite in SAME direction (wrong!)
        std::size_t multipleEdges = 0; // Edge appears more than once
        
        for (auto const& [edgeKey, count] : directedEdgeCount)
        {
            std::uint32_t const a = static_cast<std::uint32_t>(edgeKey >> 32);
            std::uint32_t const b = static_cast<std::uint32_t>(edgeKey & 0xFFFFFFFF);
            std::uint64_t const reverseKey = (static_cast<std::uint64_t>(b) << 32) | a;
            
            if (count > 1)
            {
                ++multipleEdges;
            }
            else if (directedEdgeCount.count(reverseKey) && directedEdgeCount.at(reverseKey) == 1)
            {
                ++pairedEdges;
            }
            else
            {
                ++singleEdges;
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Directed edge analysis: paired=" << (pairedEdges / 2) 
                  << ", single=" << singleEdges 
                  << ", multiple=" << multipleEdges << std::endl;
        
        #endif
        // Debug: sample some multiple edges
        if (multipleEdges > 0)
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  Multiple edge samples (first 10):" << std::endl;
            #endif
            int sampleCount = 0;
            for (auto const& [edgeKey, count] : directedEdgeCount)
            {
                if (count > 1 && sampleCount < 10)
                {
                    std::uint32_t const a = static_cast<std::uint32_t>(edgeKey >> 32);
                    std::uint32_t const b = static_cast<std::uint32_t>(edgeKey & 0xFFFFFFFF);
                    
                    // Find which triangles have this edge
                    std::size_t const numTris = indices.size() / 3;
                    std::vector<std::size_t> trianglesWithEdge;
                    for (std::size_t tri = 0; tri < numTris; ++tri)
                    {
                        std::uint32_t const v0 = indices[tri * 3 + 0];
                        std::uint32_t const v1 = indices[tri * 3 + 1];
                        std::uint32_t const v2 = indices[tri * 3 + 2];
                        
                        if ((v0 == a && v1 == b) || (v1 == a && v2 == b) || (v2 == a && v0 == b))
                        {
                            trianglesWithEdge.push_back(tri);
                        }
                    }
                    
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << "    Edge " << a << "->" << b << " appears " << count << " times in triangles: ";
                    #endif
                    for (std::size_t tri : trianglesWithEdge)
                    {
                        #ifdef GLOBALMORTON_DEBUG_OUTPUT
                        std::cout << tri << " ";
                        #endif
                    }
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << std::endl;
                    #endif
                    ++sampleCount;
                }
            }
        }
    }

    void GlobalMortonOctree::fillBoundaryHoles(std::vector<std::uint32_t>& indices,
                                                std::vector<Eigen::Vector3f> const& positions)
    {
        // Fill holes in the mesh by identifying boundary loops and triangulating them.
        // Boundary edges are edges that appear in only one triangle.
        
        if (indices.size() < 3)
        {
            return;
        }
        
        std::size_t const numTriangles = indices.size() / 3;
        
        // Build directed edge map: for each directed edge, store the triangle it belongs to
        // Boundary edges will only have one direction
        std::unordered_map<std::uint64_t, std::size_t> directedEdgeToTri;
        
        auto makeDirectedKey = [](std::uint32_t a, std::uint32_t b) -> std::uint64_t {
            return (static_cast<std::uint64_t>(a) << 32) | b;
        };
        
        for (std::size_t tri = 0; tri < numTriangles; ++tri)
        {
            std::uint32_t const v0 = indices[tri * 3 + 0];
            std::uint32_t const v1 = indices[tri * 3 + 1];
            std::uint32_t const v2 = indices[tri * 3 + 2];
            
            directedEdgeToTri[makeDirectedKey(v0, v1)] = tri;
            directedEdgeToTri[makeDirectedKey(v1, v2)] = tri;
            directedEdgeToTri[makeDirectedKey(v2, v0)] = tri;
        }
        
        // Find boundary edges (edges without a reverse counterpart)
        // Store as directed edges pointing along the boundary
        std::unordered_map<std::uint32_t, std::uint32_t> boundaryNext;  // vertex -> next vertex along boundary
        
        for (auto const& [edgeKey, tri] : directedEdgeToTri)
        {
            std::uint32_t const a = static_cast<std::uint32_t>(edgeKey >> 32);
            std::uint32_t const b = static_cast<std::uint32_t>(edgeKey & 0xFFFFFFFF);
            std::uint64_t const reverseKey = makeDirectedKey(b, a);
            
            if (directedEdgeToTri.find(reverseKey) == directedEdgeToTri.end())
            {
                // This is a boundary edge going a->b
                // To close the hole, we need to add a triangle on the other side
                // The boundary loop goes in the OPPOSITE direction (b->a for the new triangles)
                boundaryNext[b] = a;
            }
        }
        
        if (boundaryNext.empty())
        {
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "  No boundary edges to fill" << std::endl;
            #endif
            return;
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Filling holes: " << boundaryNext.size() << " boundary edges" << std::endl;
        
        #endif
        // Extract boundary loops
        std::unordered_set<std::uint32_t> visited;
        std::vector<std::vector<std::uint32_t>> loops;
        
        for (auto const& [start, next] : boundaryNext)
        {
            if (visited.count(start)) continue;
            
            std::vector<std::uint32_t> loop;
            std::uint32_t current = start;
            
            while (visited.find(current) == visited.end())
            {
                visited.insert(current);
                loop.push_back(current);
                
                auto it = boundaryNext.find(current);
                if (it == boundaryNext.end())
                {
                    break;  // Open loop, shouldn't happen in a valid mesh
                }
                current = it->second;
                
                if (current == start)
                {
                    break;  // Closed the loop
                }
            }
            
            if (loop.size() >= 3)
            {
                loops.push_back(std::move(loop));
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Found " << loops.size() << " boundary loops" << std::endl;
        
        #endif
        // Triangulate each loop using ear clipping or simple fan triangulation
        std::size_t addedTriangles = 0;
        
        for (auto const& loop : loops)
        {
            if (loop.size() < 3) continue;
            
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "    Loop with " << loop.size() << " vertices" << std::endl;
            
            #endif
            // For small loops (3-4 vertices), use simple fan triangulation
            // For larger loops, we'd need proper ear clipping
            if (loop.size() == 3)
            {
                // Single triangle to fill
                indices.push_back(loop[0]);
                indices.push_back(loop[1]);
                indices.push_back(loop[2]);
                ++addedTriangles;
            }
            else if (loop.size() == 4)
            {
                // Quad - split into two triangles
                // Choose the shorter diagonal to avoid thin triangles
                float const diag02 = (positions[loop[0]] - positions[loop[2]]).squaredNorm();
                float const diag13 = (positions[loop[1]] - positions[loop[3]]).squaredNorm();
                
                if (diag02 < diag13)
                {
                    indices.push_back(loop[0]);
                    indices.push_back(loop[1]);
                    indices.push_back(loop[2]);
                    
                    indices.push_back(loop[0]);
                    indices.push_back(loop[2]);
                    indices.push_back(loop[3]);
                }
                else
                {
                    indices.push_back(loop[0]);
                    indices.push_back(loop[1]);
                    indices.push_back(loop[3]);
                    
                    indices.push_back(loop[1]);
                    indices.push_back(loop[2]);
                    indices.push_back(loop[3]);
                }
                addedTriangles += 2;
            }
            else
            {
                // Larger loop - use fan triangulation from centroid
                // First, compute centroid
                Eigen::Vector3f centroid = Eigen::Vector3f::Zero();
                for (std::uint32_t v : loop)
                {
                    centroid += positions[v];
                }
                centroid /= static_cast<float>(loop.size());
                
                // For now, use simple fan from first vertex
                // This may create thin triangles but is simple
                for (std::size_t i = 1; i + 1 < loop.size(); ++i)
                {
                    indices.push_back(loop[0]);
                    indices.push_back(loop[i]);
                    indices.push_back(loop[i + 1]);
                    ++addedTriangles;
                }
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Added " << addedTriangles << " triangles to fill holes" << std::endl;
        #endif
    }

    void GlobalMortonOctree::fixTriangleOrientation(std::vector<std::uint32_t>& indices)
    {
        // Fix triangle orientation by propagating consistent winding through the mesh.
        // This ensures each mesh edge is traversed in opposite directions by its two triangles.
        
        if (indices.size() < 3)
        {
            return;
        }
        
        auto const& positions = m_vertexRegistry.getPositions();
        std::size_t const numTriangles = indices.size() / 3;
        
        // Build edge-to-triangle map for adjacency
        // Key: undirected edge (min,max)
        // Value: list of triangles sharing this edge
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> edgeToTriangles;
        
        auto makeEdgeKey = [](std::uint32_t v1, std::uint32_t v2) -> std::uint64_t {
            if (v1 > v2) std::swap(v1, v2);
            return (static_cast<std::uint64_t>(v1) << 32) | v2;
        };
        
        // For each triangle, store the directed edges
        // We need to track the direction to check winding consistency
        struct TriangleEdges {
            std::uint32_t v0, v1, v2;  // Original vertices in order
        };
        std::vector<TriangleEdges> triEdges(numTriangles);
        
        for (std::size_t tri = 0; tri < numTriangles; ++tri)
        {
            std::uint32_t const v0 = indices[tri * 3 + 0];
            std::uint32_t const v1 = indices[tri * 3 + 1];
            std::uint32_t const v2 = indices[tri * 3 + 2];
            
            triEdges[tri] = {v0, v1, v2};
            
            edgeToTriangles[makeEdgeKey(v0, v1)].push_back(tri);
            edgeToTriangles[makeEdgeKey(v1, v2)].push_back(tri);
            edgeToTriangles[makeEdgeKey(v2, v0)].push_back(tri);
        }
        
        // Helper to get the directed edge for a triangle on a given undirected edge
        auto getDirectedEdge = [&triEdges](std::size_t tri, std::uint32_t va, std::uint32_t vb) 
            -> std::pair<std::uint32_t, std::uint32_t> {
            auto const& te = triEdges[tri];
            // Check each edge of the triangle
            if ((te.v0 == va && te.v1 == vb) || (te.v0 == vb && te.v1 == va))
                return {te.v0, te.v1};
            if ((te.v1 == va && te.v2 == vb) || (te.v1 == vb && te.v2 == va))
                return {te.v1, te.v2};
            if ((te.v2 == va && te.v0 == vb) || (te.v2 == vb && te.v0 == va))
                return {te.v2, te.v0};
            return {0, 0};  // Shouldn't happen
        };
        
        // BFS to propagate consistent winding
        // -1 = unvisited, 0 = keep winding, 1 = flip winding
        std::vector<int> flipState(numTriangles, -1);
        std::size_t numComponents = 0;
        std::size_t flippedTriangles = 0;
        
        for (std::size_t startTri = 0; startTri < numTriangles; ++startTri)
        {
            if (flipState[startTri] != -1) continue;
            
            // Start a new component
            ++numComponents;
            std::queue<std::size_t> bfsQueue;
            bfsQueue.push(startTri);
            flipState[startTri] = 0;  // Start by keeping the first triangle's winding
            
            while (!bfsQueue.empty())
            {
                std::size_t const currentTri = bfsQueue.front();
                bfsQueue.pop();
                
                auto const& te = triEdges[currentTri];
                std::uint32_t const edges[3][2] = {
                    {te.v0, te.v1}, {te.v1, te.v2}, {te.v2, te.v0}
                };
                
                for (int e = 0; e < 3; ++e)
                {
                    std::uint64_t const edgeKey = makeEdgeKey(edges[e][0], edges[e][1]);
                    auto const& neighbors = edgeToTriangles[edgeKey];
                    
                    for (std::size_t neighborTri : neighbors)
                    {
                        if (neighborTri == currentTri) continue;
                        
                        // Check if neighbor needs consistent winding
                        // For proper manifold, the shared edge should go in OPPOSITE directions
                        auto const currentDir = getDirectedEdge(currentTri, edges[e][0], edges[e][1]);
                        auto const neighborDir = getDirectedEdge(neighborTri, edges[e][0], edges[e][1]);
                        
                        // After accounting for current triangle's flip state, determine what neighbor needs
                        bool const currentFlipped = (flipState[currentTri] == 1);
                        
                        // Current triangle's effective edge direction
                        std::pair<std::uint32_t, std::uint32_t> effectiveCurrentDir = currentDir;
                        if (currentFlipped)
                        {
                            // Flipping a triangle reverses its edge directions
                            effectiveCurrentDir = {currentDir.second, currentDir.first};
                        }
                        
                        // For consistent manifold, neighbor should have opposite direction
                        bool const neighborSameDir = (neighborDir.first == effectiveCurrentDir.first);
                        
                        // If neighbor has same direction, it needs to be flipped
                        int const requiredFlip = neighborSameDir ? 1 : 0;
                        
                        if (flipState[neighborTri] == -1)
                        {
                            flipState[neighborTri] = requiredFlip;
                            bfsQueue.push(neighborTri);
                        }
                        // else: already visited, should be consistent (otherwise mesh is non-manifold)
                    }
                }
            }
        }
        
        // Apply flips
        for (std::size_t tri = 0; tri < numTriangles; ++tri)
        {
            if (flipState[tri] == 1)
            {
                std::swap(indices[tri * 3 + 1], indices[tri * 3 + 2]);
                ++flippedTriangles;
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Orientation propagation: " << numComponents << " components, "
                  << flippedTriangles << " triangles flipped" << std::endl;
        
        #endif
        // Now vote on whether each component should be globally flipped
        // based on gradient alignment
        float const cellSize = (m_globalBboxSize.x() + m_globalBboxSize.y() + m_globalBboxSize.z()) / 
                              (3.0F * static_cast<float>(1U << m_config.maxDepth));
        float const gradEpsilon = cellSize * 0.01F;
        
        // Build component mapping
        std::vector<std::size_t> triComponent(numTriangles);
        std::vector<std::vector<std::size_t>> componentTriangles(numComponents);
        {
            std::vector<int> componentId(numTriangles, -1);
            std::size_t nextComponentId = 0;
            for (std::size_t tri = 0; tri < numTriangles; ++tri)
            {
                if (componentId[tri] == -1)
                {
                    // BFS to find all triangles in this component
                    std::queue<std::size_t> q;
                    q.push(tri);
                    componentId[tri] = static_cast<int>(nextComponentId);
                    
                    while (!q.empty())
                    {
                        std::size_t const t = q.front();
                        q.pop();
                        
                        componentTriangles[nextComponentId].push_back(t);
                        triComponent[t] = nextComponentId;
                        
                        auto const& te = triEdges[t];
                        std::uint64_t const edges[3] = {
                            makeEdgeKey(te.v0, te.v1),
                            makeEdgeKey(te.v1, te.v2),
                            makeEdgeKey(te.v2, te.v0)
                        };
                        
                        for (int e = 0; e < 3; ++e)
                        {
                            for (std::size_t neighbor : edgeToTriangles[edges[e]])
                            {
                                if (componentId[neighbor] == -1)
                                {
                                    componentId[neighbor] = static_cast<int>(nextComponentId);
                                    q.push(neighbor);
                                }
                            }
                        }
                    }
                    ++nextComponentId;
                }
            }
        }
        
        // Vote on global orientation for each component
        std::size_t globallyFlippedComponents = 0;
        std::size_t globallyFlippedTriangles = 0;
        
        for (std::size_t comp = 0; comp < numComponents; ++comp)
        {
            int correctVotes = 0;
            int wrongVotes = 0;
            int skippedVotes = 0;
            
            for (std::size_t tri : componentTriangles[comp])
            {
                std::uint32_t const v0 = indices[tri * 3 + 0];
                std::uint32_t const v1 = indices[tri * 3 + 1];
                std::uint32_t const v2 = indices[tri * 3 + 2];
                
                Eigen::Vector3f const& p0 = positions[v0];
                Eigen::Vector3f const& p1 = positions[v1];
                Eigen::Vector3f const& p2 = positions[v2];
                
                Eigen::Vector3f const triNormal = (p1 - p0).cross(p2 - p0);
                if (triNormal.norm() < 1e-10F) continue;
                
                // Use the midpoint of edge 0 (which is on the surface) for sampling
                Eigen::Vector3f const edgeMid = (p0 + p1) * 0.5F;
                float const sdfValue = sampleSdf(edgeMid);
                
                // Only vote if the sample point is close to the surface
                // Points far from the surface may have unreliable gradients
                if (std::abs(sdfValue) > cellSize * 0.5F)
                {
                    ++skippedVotes;
                    continue;
                }
                
                Eigen::Vector3f const gradient = sampleGradient(edgeMid, gradEpsilon);
                
                // For outward-facing normals, the normal should point AWAY from the solid (positive SDF)
                // The gradient points from negative to positive SDF, i.e., OUTWARD from solid
                // So if normal · gradient > 0, normal points outward = correct
                float const dot = triNormal.normalized().dot(gradient.normalized());
                if (dot > 0.0F) ++correctVotes;
                else if (dot < 0.0F) ++wrongVotes;
            }
            
            // More detailed breakdown of votes
            int const totalVotes = correctVotes + wrongVotes;
            float const correctRatio = totalVotes > 0 ? 
                static_cast<float>(correctVotes) / static_cast<float>(totalVotes) : 0.5F;
            
            #ifdef GLOBALMORTON_DEBUG_OUTPUT
            std::cout << "    Component " << comp << ": " << componentTriangles[comp].size() 
                      << " triangles, correct=" << correctVotes << " (" << (correctRatio * 100.0F) << "%)"
                      << " wrong=" << wrongVotes << " (" << ((1.0F - correctRatio) * 100.0F) << "%)"
                      << " skipped=" << skippedVotes
                      << " -> " << (wrongVotes > correctVotes ? "FLIP" : "keep") << std::endl;
            
            #endif
            if (wrongVotes > correctVotes)
            {
                ++globallyFlippedComponents;
                for (std::size_t tri : componentTriangles[comp])
                {
                    std::swap(indices[tri * 3 + 1], indices[tri * 3 + 2]);
                    ++globallyFlippedTriangles;
                }
            }
        }
        
        #ifdef GLOBALMORTON_DEBUG_OUTPUT
        std::cout << "  Global orientation: flipped " << globallyFlippedComponents 
                  << " components (" << globallyFlippedTriangles << " triangles)" << std::endl;
                  #endif
    }

    std::size_t GlobalMortonOctree::allocateNode()
    {
        std::size_t const index = m_nodes.size();
        m_nodes.emplace_back();
        return index;
    }

    Eigen::Vector3f GlobalMortonOctree::cornerPosition(std::uint8_t cornerIndex,
                                                         BoundingBox const& bounds) const
    {
        return Eigen::Vector3f(
            (cornerIndex & 1U) ? bounds.max.s[0] : bounds.min.s[0],
            (cornerIndex & 2U) ? bounds.max.s[1] : bounds.min.s[1],
            (cornerIndex & 4U) ? bounds.max.s[2] : bounds.min.s[2]);
    }

    bool GlobalMortonOctree::hasSignChange(GlobalOctreeNode const& node) const
    {
        return node.edgeMask != 0U;
    }

    std::uint64_t GlobalMortonOctree::computeChildMorton(std::uint64_t parentMorton,
                                                           std::uint8_t childIndex,
                                                           std::uint8_t parentDepth) const
    {
        // Child Morton code is parent shifted left by 3 bits plus child octant index
        // Each level adds 3 bits (one for each axis)
        return (parentMorton << 3U) | static_cast<std::uint64_t>(childIndex);
    }

    std::size_t GlobalMortonOctree::findNeighborNode(std::uint64_t mortonCode,
                                                       int dx, int dy, int dz,
                                                       std::uint8_t depth) const
    {
        // Decode path-based Morton to coordinates
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        std::uint32_t z = 0U;
        decodePathMorton(mortonCode, depth, x, y, z);

        // Apply offset
        auto const maxCoord = (1U << depth) - 1U;
        if ((dx < 0 && x == 0U) || (dx > 0 && x == maxCoord) ||
            (dy < 0 && y == 0U) || (dy > 0 && y == maxCoord) ||
            (dz < 0 && z == 0U) || (dz > 0 && z == maxCoord))
        {
            return std::numeric_limits<std::size_t>::max(); // Out of bounds
        }

        x = static_cast<std::uint32_t>(static_cast<int>(x) + dx);
        y = static_cast<std::uint32_t>(static_cast<int>(y) + dy);
        z = static_cast<std::uint32_t>(static_cast<int>(z) + dz);

        std::uint64_t const neighborMorton = encodePathMorton(x, y, z, depth);

        auto it = m_mortonToIndex.find(MortonNodeKey{neighborMorton, depth});
        if (it != m_mortonToIndex.end())
        {
            return it->second;
        }

        return std::numeric_limits<std::size_t>::max();
    }

    float GlobalMortonOctree::sampleSdf(Eigen::Vector3f const& position) const
    {
        // Use precomputed SDF buffer for evaluation
        auto resources = m_core.getResourceContext();
        if (!resources)
        {
            return 0.0F;
        }

        auto& sdfBuffer = resources->getPrecompSdfBuffer();
        auto const width = sdfBuffer.getWidth();
        auto const height = sdfBuffer.getHeight();
        auto const depth = sdfBuffer.getDepth();

        if (width == 0U || height == 0U || depth == 0U)
        {
            return 0.0F;
        }

        // Transform world position to normalized coordinates within the bounding box
        Eigen::Vector3f const safeExtent = m_globalBboxSize.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
        Eigen::Vector3f normalized = (position - m_globalBboxMin).cwiseQuotient(safeExtent);
        normalized = normalized.cwiseMax(Eigen::Vector3f::Zero()).cwiseMin(Eigen::Vector3f::Ones());

        // Convert to grid indices
        float const gx = normalized.x() * static_cast<float>(width - 1U);
        float const gy = normalized.y() * static_cast<float>(height - 1U);
        float const gz = normalized.z() * static_cast<float>(depth - 1U);

        auto const x0 = static_cast<std::size_t>(std::floor(gx));
        auto const y0 = static_cast<std::size_t>(std::floor(gy));
        auto const z0 = static_cast<std::size_t>(std::floor(gz));
        auto const x1 = std::min(x0 + 1U, width - 1U);
        auto const y1 = std::min(y0 + 1U, height - 1U);
        auto const z1 = std::min(z0 + 1U, depth - 1U);

        // Trilinear interpolation weights
        float const fx = gx - static_cast<float>(x0);
        float const fy = gy - static_cast<float>(y0);
        float const fz = gz - static_cast<float>(z0);

        // Sample 8 corners
        auto const& data = sdfBuffer.getData();
        auto sampleAt = [&](std::size_t x, std::size_t y, std::size_t z) -> float {
            std::size_t const index = z * width * height + y * width + x;
            return (index < data.size()) ? data[index] : 0.0F;
        };

        float const v000 = sampleAt(x0, y0, z0);
        float const v100 = sampleAt(x1, y0, z0);
        float const v010 = sampleAt(x0, y1, z0);
        float const v110 = sampleAt(x1, y1, z0);
        float const v001 = sampleAt(x0, y0, z1);
        float const v101 = sampleAt(x1, y0, z1);
        float const v011 = sampleAt(x0, y1, z1);
        float const v111 = sampleAt(x1, y1, z1);

        // Trilinear interpolation
        float const v00 = v000 * (1.0F - fx) + v100 * fx;
        float const v01 = v001 * (1.0F - fx) + v101 * fx;
        float const v10 = v010 * (1.0F - fx) + v110 * fx;
        float const v11 = v011 * (1.0F - fx) + v111 * fx;

        float const v0 = v00 * (1.0F - fy) + v10 * fy;
        float const v1 = v01 * (1.0F - fy) + v11 * fy;

        return v0 * (1.0F - fz) + v1 * fz;
    }

    Eigen::Vector3f GlobalMortonOctree::sampleGradient(Eigen::Vector3f const& position,
                                                         float epsilon) const
    {
        // Central differences
        float const dx = sampleSdf(position + Eigen::Vector3f(epsilon, 0.0F, 0.0F)) -
                         sampleSdf(position - Eigen::Vector3f(epsilon, 0.0F, 0.0F));
        float const dy = sampleSdf(position + Eigen::Vector3f(0.0F, epsilon, 0.0F)) -
                         sampleSdf(position - Eigen::Vector3f(0.0F, epsilon, 0.0F));
        float const dz = sampleSdf(position + Eigen::Vector3f(0.0F, 0.0F, epsilon)) -
                         sampleSdf(position - Eigen::Vector3f(0.0F, 0.0F, epsilon));

        return Eigen::Vector3f(dx, dy, dz) / (2.0F * epsilon);
    }
}
