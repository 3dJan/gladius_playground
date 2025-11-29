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

        // Cell size at this depth
        auto const cellsPerAxis = static_cast<float>(1U << depth);
        Eigen::Vector3f const cellSize = globalBboxSize / cellsPerAxis;

        // Compute bounds
        BoundingBox bounds;
        bounds.min.s[0] = globalBboxMin.x() + static_cast<float>(ix) * cellSize.x();
        bounds.min.s[1] = globalBboxMin.y() + static_cast<float>(iy) * cellSize.y();
        bounds.min.s[2] = globalBboxMin.z() + static_cast<float>(iz) * cellSize.z();
        bounds.min.s[3] = 0.0F;

        bounds.max.s[0] = bounds.min.s[0] + cellSize.x();
        bounds.max.s[1] = bounds.min.s[1] + cellSize.y();
        bounds.max.s[2] = bounds.min.s[2] + cellSize.z();
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
                if (node.cornerValues[c] < 0.0F)
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

                if (node.cornerValues[c0] * node.cornerValues[c1] < 0.0F)
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

                // Register Morton code for lookup
                m_mortonToIndex[child.mortonCode] = childNodeIdx;

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
            
            // For each intersecting leaf, ensure its 6 face-adjacent neighbors exist
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
                    
                    if (m_mortonToIndex.find(neighborMorton) == m_mortonToIndex.end())
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
        auto it = m_mortonToIndex.find(mortonCode);
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
        
        // Register in Morton lookup
        m_mortonToIndex[mortonCode] = nodeIdx;
        
        // Add to appropriate level
        if (depth < m_levels.size())
        {
            m_levels[depth].nodeIndices.push_back(nodeIdx);
        }
        
        // Evaluate corners for this node to determine if it's intersecting
        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize, 0);
        
        // Sample SDF at all 8 corners
        std::uint8_t signMask = 0U;
        std::uint8_t zeroMask = 0U;
        for (std::uint8_t c = 0U; c < 8U; ++c)
        {
            Eigen::Vector3f const cornerPos = cornerPosition(c, bounds);
            float const sdfValue = sampleSdf(cornerPos);
            node.cornerValues[c] = sdfValue;
            
            if (sdfValue >= 0.0F)
            {
                signMask |= (1U << c);
            }
            if (std::abs(sdfValue) < 1e-6F)
            {
                zeroMask |= (1U << c);
            }
        }
        
        // Check if node intersects surface
        bool const allPositive = (signMask == 0xFFU);
        bool const allNegative = (signMask == 0x00U);
        node.isIntersecting = !allPositive && !allNegative;
        
        // Detect edge crossings
        node.edgeMask = 0U;
        for (std::size_t e = 0U; e < 12U; ++e)
        {
            auto const c0 = EDGE_CORNERS[e][0];
            auto const c1 = EDGE_CORNERS[e][1];
            float const v0 = node.cornerValues[c0];
            float const v1 = node.cornerValues[c1];
            
            if (v0 * v1 < 0.0F)
            {
                node.edgeMask |= (1U << e);
            }
        }
        
        return nodeIdx;
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

                m_mortonToIndex[child.mortonCode] = childNodeIdx;
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

        for (auto& node : m_nodes)
        {
            if (!node.isLeaf || !node.isIntersecting)
            {
                continue;
            }

            // Gather Hermite samples for this cell
            gatherHermiteSamples(node);

            // Solve QEF to get vertex position
            solveQefForNode(node);
        }

        m_stats.vertexCount = m_vertexRegistry.getVertexCount();

        auto const endTime = std::chrono::high_resolution_clock::now();
        m_stats.vertexGenerationTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
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

        BoundingBox const bounds = node.computeBounds(m_globalBboxMin, m_globalBboxSize,
                                                       static_cast<std::uint32_t>(m_config.maxDepth));

        // Compute mass point as initial position
        Eigen::Vector3f massPoint = Eigen::Vector3f::Zero();
        for (auto const& sample : node.hermiteSamples)
        {
            massPoint += sample.position;
        }
        massPoint /= static_cast<float>(node.hermiteSamples.size());

        // Use QEF solver if enough samples
        Eigen::Vector3f vertex;
        Eigen::Vector3f normal = Eigen::Vector3f::Zero();

        if (node.hermiteSamples.size() >= 3U)
        {
            // Build QEF matrices
            Eigen::Matrix3f ata = Eigen::Matrix3f::Zero();
            Eigen::Vector3f atb = Eigen::Vector3f::Zero();

            for (auto const& sample : node.hermiteSamples)
            {
                Eigen::Vector3f const n = sample.gradient.normalized();
                ata += n * n.transpose();
                atb += n * n.dot(sample.position);
                normal += n;
            }

            // Solve with SVD
            Eigen::JacobiSVD<Eigen::Matrix3f> svd(ata, Eigen::ComputeFullU | Eigen::ComputeFullV);
            vertex = svd.solve(atb);

            // Clamp to cell bounds
            vertex.x() = std::clamp(vertex.x(), bounds.min.s[0], bounds.max.s[0]);
            vertex.y() = std::clamp(vertex.y(), bounds.min.s[1], bounds.max.s[1]);
            vertex.z() = std::clamp(vertex.z(), bounds.min.s[2], bounds.max.s[2]);
        }
        else
        {
            vertex = massPoint;
            for (auto const& sample : node.hermiteSamples)
            {
                normal += sample.gradient.normalized();
            }
        }

        normal.normalize();

        // Register vertex globally
        std::uint32_t const vertexIndex = m_vertexRegistry.registerCellVertex(
            node.mortonCode, vertex, normal);
        node.vertexIndices.push_back(vertexIndex);
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
        
        // Note: Hole filling disabled - boundary edges come from half-degenerate quads
        // that cannot be easily fixed without risking winding inconsistencies.
        // The mesh has consistent winding (multiple=0) but some boundary edges.
        // TODO: Address degenerate quads at generation time, not as a post-process.
        // fillBoundaryHoles(indices, positions);
        
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
        
        // Debug: print first few non-manifold edges
        std::size_t debugCount = 0;
        for (auto const& [edge, triangles] : edgeToTriangles)
        {
            std::size_t count = triangles.size();
            if (count == 1)
            {
                ++m_stats.boundaryEdges;
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
                if (debugCount < 5)
                {
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << "  Non-manifold edge (" << edge.first << "," << edge.second 
                              << ") count=" << count << " from quads: ";
                              #endif
                    for (auto t : triangles)
                    {
                        std::size_t quadIdx = t / 2;
                        bool isDiagonal = (t % 2 == 0) ? 
                            (indices[t*3] == edge.first && indices[t*3+2] == edge.second) ||
                            (indices[t*3] == edge.second && indices[t*3+2] == edge.first) :
                            (indices[t*3] == edge.first && indices[t*3+2] == edge.second) ||
                            (indices[t*3] == edge.second && indices[t*3+2] == edge.first);
                        #ifdef GLOBALMORTON_DEBUG_OUTPUT
                        std::cout << "Q" << quadIdx << (isDiagonal ? "(diag)" : "(peri)") << " ";
                        #endif
                    }
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << std::endl;
                    std::cout << "    Triangles: ";
                    #endif
                    for (auto t : triangles)
                    {
                        #ifdef GLOBALMORTON_DEBUG_OUTPUT
                        std::cout << t << "[" << indices[t*3] << "," << indices[t*3+1] << "," << indices[t*3+2] << "] ";
                        #endif
                    }
                    #ifdef GLOBALMORTON_DEBUG_OUTPUT
                    std::cout << std::endl;
                    #endif
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
        
        // Build Morton→nodeIdx map for fast lookup at uniform depth
        std::unordered_map<std::uint64_t, std::size_t> mortonToNode;
        std::map<std::uint8_t, std::size_t> depthDistribution;
        for (std::size_t nodeIdx = 0U; nodeIdx < m_nodes.size(); ++nodeIdx)
        {
            auto const& node = m_nodes[nodeIdx];
            if (node.isLeaf && node.isIntersecting)
            {
                mortonToNode[node.mortonCode] = nodeIdx;
                ++depthDistribution[node.depth];
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
        
        // Helper to get vertex index from a node (returns max if no vertex)
        auto getVertexFromNode = [this](std::size_t nodeIdx) -> std::uint32_t {
            if (nodeIdx >= m_nodes.size()) return std::numeric_limits<std::uint32_t>::max();
            auto const& node = m_nodes[nodeIdx];
            if (node.vertexIndices.empty()) return std::numeric_limits<std::uint32_t>::max();
            return node.vertexIndices[0];
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
                    // Track which neighbors are missing and why
                    static std::size_t sampleCount = 0;
                    if (sampleCount++ < 5)
                    {
                        // Check if nodes exist in the full octree but aren't intersecting
                        auto checkNode = [this](std::uint64_t mc, char const* name) {
                            auto it = m_mortonToIndex.find(mc);
                            if (it == m_mortonToIndex.end()) {
                                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                                std::cout << "    " << name << " mc=" << std::hex << mc << std::dec 
                                          << ": NOT IN OCTREE" << std::endl;
                                          #endif
                            } else {
                                auto const& n = m_nodes[it->second];
                                #ifdef GLOBALMORTON_DEBUG_OUTPUT
                                std::cout << "    " << name << " mc=" << std::hex << mc << std::dec 
                                          << ": exists, isLeaf=" << n.isLeaf 
                                          << " isIntersecting=" << n.isIntersecting << std::endl;
                                          #endif
                            }
                        };
                        
                        #ifdef GLOBALMORTON_DEBUG_OUTPUT
                        std::cout << "  Edge3 neighbor miss at (" << cx << "," << cy << "," << cz 
                                  << ") depth=" << static_cast<int>(node.depth)
                                  << " mc=" << std::hex << node.mortonCode << std::dec
                                  << ": n1(" << (it1 != mortonToNode.end()) 
                                  << ") n2(" << (it2 != mortonToNode.end())
                                  << ") n3(" << (it3 != mortonToNode.end()) << ")" << std::endl;
                        std::cout << "    Lookup codes: m1=" << std::hex << m1 
                                  << " m2=" << m2 << " m3=" << m3 << std::dec << std::endl;
                                  #endif
                        checkNode(m1, "n1");
                        checkNode(m2, "n2");
                        checkNode(m3, "n3");
                    }
                }
                else
                {
                    std::uint32_t v0 = getVertexFromNode(nodeIdx);
                    std::uint32_t v1 = getVertexFromNode(it1->second);
                    std::uint32_t v2 = getVertexFromNode(it2->second);
                    std::uint32_t v3 = getVertexFromNode(it3->second);
                    
                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        // Check for duplicate vertices
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            // Winding based on corner 7 (max corner) being inside
                            bool corner7Inside = (node.internalMask & (1U << 7)) != 0;
                            
                            Eigen::Vector3f const& p0 = positions[v0];
                            Eigen::Vector3f const& p1 = positions[v1];
                            Eigen::Vector3f const& p2 = positions[v2];
                            Eigen::Vector3f const& p3 = positions[v3];
                            
                            // Triangle 1: v0, v2, v1 (or v0, v1, v2 depending on winding)
                            // Triangle 2: v1, v2, v3 (or v1, v3, v2)
                            Eigen::Vector3f tri1_normal, tri2_normal;
                            if (corner7Inside)
                            {
                                tri1_normal = (p2 - p0).cross(p1 - p0);
                                tri2_normal = (p2 - p1).cross(p3 - p1);
                            }
                            else
                            {
                                tri1_normal = (p1 - p0).cross(p2 - p0);
                                tri2_normal = (p3 - p1).cross(p2 - p1);
                            }
                            
                            bool const tri1Valid = tri1_normal.norm() >= 1e-10F;
                            bool const tri2Valid = tri2_normal.norm() >= 1e-10F;
                            
                            if (tri1Valid != tri2Valid) ++halfDegenerateQuads;
                            
                            if (tri1Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v2);
                                    indices.push_back(v1);
                                }
                                else
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                }
                            }
                            if (tri2Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                    indices.push_back(v3);
                                }
                                else
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v3);
                                    indices.push_back(v2);
                                }
                            }
                            ++emittedQuads;
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
                    std::uint32_t v0 = getVertexFromNode(nodeIdx);
                    std::uint32_t v1 = getVertexFromNode(it1->second);
                    std::uint32_t v2 = getVertexFromNode(it2->second);
                    std::uint32_t v3 = getVertexFromNode(it3->second);
                    
                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            bool corner7Inside = (node.internalMask & (1U << 7)) != 0;
                            
                            Eigen::Vector3f const& p0 = positions[v0];
                            Eigen::Vector3f const& p1 = positions[v1];
                            Eigen::Vector3f const& p2 = positions[v2];
                            Eigen::Vector3f const& p3 = positions[v3];
                            
                            Eigen::Vector3f tri1_normal, tri2_normal;
                            // Y-edge has inverted winding compared to X/Z edges
                            if (corner7Inside)
                            {
                                tri1_normal = (p1 - p0).cross(p2 - p0);
                                tri2_normal = (p3 - p1).cross(p2 - p1);
                            }
                            else
                            {
                                tri1_normal = (p2 - p0).cross(p1 - p0);
                                tri2_normal = (p2 - p1).cross(p3 - p1);
                            }
                            
                            bool const tri1Valid = tri1_normal.norm() >= 1e-10F;
                            bool const tri2Valid = tri2_normal.norm() >= 1e-10F;
                            
                            if (tri1Valid != tri2Valid) ++halfDegenerateQuads;
                            
                            if (tri1Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                }
                                else
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v2);
                                    indices.push_back(v1);
                                }
                            }
                            if (tri2Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v3);
                                    indices.push_back(v2);
                                }
                                else
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                    indices.push_back(v3);
                                }
                            }
                            ++emittedQuads;
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
                    std::uint32_t v0 = getVertexFromNode(nodeIdx);
                    std::uint32_t v1 = getVertexFromNode(it1->second);
                    std::uint32_t v2 = getVertexFromNode(it2->second);
                    std::uint32_t v3 = getVertexFromNode(it3->second);
                    
                    if (v0 != std::numeric_limits<std::uint32_t>::max() &&
                        v1 != std::numeric_limits<std::uint32_t>::max() &&
                        v2 != std::numeric_limits<std::uint32_t>::max() &&
                        v3 != std::numeric_limits<std::uint32_t>::max())
                    {
                        if (v0 != v1 && v0 != v2 && v0 != v3 && v1 != v2 && v1 != v3 && v2 != v3)
                        {
                            bool corner7Inside = (node.internalMask & (1U << 7)) != 0;
                            
                            Eigen::Vector3f const& p0 = positions[v0];
                            Eigen::Vector3f const& p1 = positions[v1];
                            Eigen::Vector3f const& p2 = positions[v2];
                            Eigen::Vector3f const& p3 = positions[v3];
                            
                            Eigen::Vector3f tri1_normal, tri2_normal;
                            if (corner7Inside)
                            {
                                tri1_normal = (p2 - p0).cross(p1 - p0);
                                tri2_normal = (p2 - p1).cross(p3 - p1);
                            }
                            else
                            {
                                tri1_normal = (p1 - p0).cross(p2 - p0);
                                tri2_normal = (p3 - p1).cross(p2 - p1);
                            }
                            
                            bool const tri1Valid = tri1_normal.norm() >= 1e-10F;
                            bool const tri2Valid = tri2_normal.norm() >= 1e-10F;
                            
                            if (tri1Valid != tri2Valid) ++halfDegenerateQuads;
                            
                            if (tri1Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v2);
                                    indices.push_back(v1);
                                }
                                else
                                {
                                    indices.push_back(v0);
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                }
                            }
                            if (tri2Valid)
                            {
                                if (corner7Inside)
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v2);
                                    indices.push_back(v3);
                                }
                                else
                                {
                                    indices.push_back(v1);
                                    indices.push_back(v3);
                                    indices.push_back(v2);
                                }
                            }
                            ++emittedQuads;
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

        auto it = m_mortonToIndex.find(neighborMorton);
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
