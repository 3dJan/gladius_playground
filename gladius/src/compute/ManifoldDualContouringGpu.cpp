#include "ManifoldDualContouringGpu.h"
#include "ManifoldDualContouringProgram.h"
#include "../Primitives.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <unordered_set>

namespace gladius::compute
{
    namespace
    {
        struct GpuVertex
        {
            cl_float4 position;
            cl_float4 normal;
        };
        constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();

        inline std::uint64_t expandBits64(std::uint64_t v)
        {
            v = (v | (v << 32U)) & 0x1f00000000ffffULL;
            v = (v | (v << 16U)) & 0x1f0000ff0000ffULL;
            v = (v | (v << 8U)) & 0x100f00f00f00f00FULL;
            v = (v | (v << 4U)) & 0x10c30c30c30c30c3ULL;
            v = (v | (v << 2U)) & 0x1249249249249249ULL;
            return v;
        }

        inline std::uint64_t compactBits64(std::uint64_t v)
        {
            v &= 0x1249249249249249ULL;
            v = (v | (v >> 2U)) & 0x10c30c30c30c30c3ULL;
            v = (v | (v >> 4U)) & 0x100f00f00f00f00FULL;
            v = (v | (v >> 8U)) & 0x1f0000ff0000ffULL;
            v = (v | (v >> 16U)) & 0x1f00000000ffffULL;
            v = (v | (v >> 32U)) & 0x1fffffULL;
            return v;
        }

        inline std::uint64_t encodeMorton(std::uint32_t x, std::uint32_t y, std::uint32_t z)
        {
            return (expandBits64(z) << 2U) | (expandBits64(y) << 1U) | expandBits64(x);
        }

        struct MortonCoord
        {
            std::uint32_t x{0U};
            std::uint32_t y{0U};
            std::uint32_t z{0U};
        };

        inline MortonCoord decodeMorton(std::uint64_t morton)
        {
            MortonCoord coord;
            coord.x = static_cast<std::uint32_t>(compactBits64(morton));
            coord.y = static_cast<std::uint32_t>(compactBits64(morton >> 1U));
            coord.z = static_cast<std::uint32_t>(compactBits64(morton >> 2U));
            return coord;
        }

        struct EdgeInfo
        {
            std::uint8_t corner0;
            std::uint8_t corner1;
            std::uint8_t edgeIdx;
            int dx0, dy0, dz0;
            int dx1, dy1, dz1;
            int dx2, dy2, dz2;
        };

        constexpr std::array<EdgeInfo, 12U> kEdgeInfos = {{
            {0, 1, 0,  0, -1, -1,  0,  0, -1,  0, -1,  0},
            {2, 3, 1,  0,  1, -1,  0,  0, -1,  0,  1,  0},
            {4, 5, 2,  0, -1,  1,  0,  0,  1,  0, -1,  0},
            {6, 7, 3,  0,  1,  1,  0,  0,  1,  0,  1,  0},
            {0, 2, 4, -1,  0, -1,  0,  0, -1, -1,  0,  0},
            {1, 3, 5,  1,  0, -1,  0,  0, -1,  1,  0,  0},
            {4, 6, 6, -1,  0,  1,  0,  0,  1, -1,  0,  0},
            {5, 7, 7,  1,  0,  1,  0,  0,  1,  1,  0,  0},
            {0, 4, 8, -1, -1,  0,  0, -1,  0, -1,  0,  0},
            {1, 5, 9,  1, -1,  0,  0, -1,  0,  1,  0,  0},
            {2, 6,10, -1,  1,  0,  0,  1,  0, -1,  0,  0},
            {3, 7,11,  1,  1,  0,  0,  1,  0,  1,  0,  0}
        }};

    }

    ManifoldDualContouringGpu::ManifoldDualContouringGpu(ComputeCore & core)
        : m_core(core)
    {
        loadKernels();
    }

    void ManifoldDualContouringGpu::setConfig(ManifoldDualContouringConfig config)
    {
        if (config.initialDepth > config.maxDepth)
        {
            config.initialDepth = config.maxDepth;
        }
        m_config = config;
    }

    void ManifoldDualContouringGpu::loadKernels()
    {
        // Get the shared program instance from ProgramManager
        // This ensures the program has the correct model source set
        auto & programManager = m_core.getProgramManager();
        
        auto * program = programManager.getManifoldDualContouringProgram();
        if (!program)
        {
            throw std::runtime_error("ManifoldDualContouringProgram not available in ProgramManager");
        }

        // Store a pointer to the program (we don't own it - ProgramManager does)
        m_program = program;
    }

    void ManifoldDualContouringGpu::generateMesh()
    {
        m_mesh.positions.clear();
        m_mesh.normals.clear();
        m_mesh.indices.clear();
        m_lastVertexCount = 0U;

        if (!m_program)
        {
            std::cerr << "Program not initialized, cannot generate mesh" << std::endl;
            return;
        }

        constructOctree();
        generateVertices();
        generateIndices();
    }

    void ManifoldDualContouringGpu::constructOctree()
    {
        auto context = m_core.getComputeContext();
        auto& queue = context->GetQueue();
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();
        
        // Get bounding box from compute core
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available for octree construction" << std::endl;
            return;
        }
        
        // Get primitives
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "No primitives available" << std::endl;
            return;
        }
        
        Eigen::Vector3f bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        m_cachedBboxMin = bboxMin;
        m_cachedBboxMax = bboxMax;
        m_cachedBboxSize = bboxMax - bboxMin;
        m_octreeDepth = m_config.maxDepth;
        if (m_octreeDepth >= 31U)
        {
            m_gridResolution = std::numeric_limits<std::uint32_t>::max();
        }
        else
        {
            m_gridResolution = 1U << m_octreeDepth;
            if (m_gridResolution == 0U)
            {
                m_gridResolution = 1U;
            }
        }
        
          std::cout << "Constructing Octree. BBox: [" << bboxMin.transpose() << "] to ["
              << bboxMax.transpose() << "], Extents: " << m_cachedBboxSize.transpose()
              << ", initialDepth: " << m_config.initialDepth
              << ", maxDepth: " << m_config.maxDepth << std::endl;
        
        try {
            m_program->constructOctree(
                m_octreeBuffer,
                m_octreeNodeCount,
                bboxMin,
                bboxMax,
                static_cast<std::uint32_t>(m_config.initialDepth),
                m_config.maxDepth,
                *primitives,
                m_config.isoValue);
            
            std::cout << "Octree construction complete. Total nodes: " << m_octreeNodeCount << std::endl;
            refreshCpuOctreeCache();
        } catch (std::exception& e) {
            std::cerr << "Error in octree construction: " << e.what() << std::endl;
        }
    }

    void ManifoldDualContouringGpu::generateVertices()
    {
        auto context = m_core.getComputeContext();
        auto& queue = context->GetQueue();
        
        if (m_octreeNodeCount == 0)
        {
            std::cerr << "No octree nodes to generate vertices from" << std::endl;
            return;
        }
        
        size_t numNodes = m_octreeNodeCount;
        
        // Get primitives
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "No primitives available" << std::endl;
            return;
        }

        Eigen::Vector3f bboxMin = m_cachedBboxMin;
        Eigen::Vector3f bboxMax = m_cachedBboxMax;
        
        // 1. Count vertices
        m_countBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
        
        try {
            m_program->countVertices(*m_octreeBuffer, *m_countBuffer, numNodes);
        } catch (std::exception& e) {
            std::cerr << "Error running count_vertices: " << e.what() << std::endl;
            return;
        }
            
        // 2. Scan (Prefix Sum)
        // For this initial implementation, we perform the scan on the CPU.
        // For high performance, this should be replaced with a GPU-based scan (e.g., Blelloch scan).
        std::vector<int> counts(numNodes);
        try {
            queue.enqueueReadBuffer(*m_countBuffer, CL_TRUE, 0, numNodes * sizeof(int), counts.data());
        } catch (std::exception& e) {
             std::cerr << "Error reading count buffer: " << e.what() << std::endl;
             return;
        }
        
        std::vector<int> offsets(numNodes);
        int totalVertices = 0;
        for (size_t i = 0; i < numNodes; ++i) {
            offsets[i] = totalVertices;
            totalVertices += counts[i];
        }

        m_cpuVertexCounts = counts;
        m_cpuVertexOffsets = offsets;
        
        std::cout << "Generating " << totalVertices << " vertices from " << numNodes << " octree nodes" << std::endl;
        
        if (totalVertices == 0) {
            std::cout << "No vertices to generate" << std::endl;
            return;
        }

        m_lastVertexCount = static_cast<std::size_t>(totalVertices);
        
        m_offsetBuffer = context->createBufferChecked(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, 
            numNodes * sizeof(int), offsets.data());
            
        // 3. Emit Vertices
        // Vertex struct is float4 position + float4 normal = 32 bytes
        m_vertexBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, totalVertices * 32); 
        
        try {
            m_program->generateVertices(
                *m_octreeBuffer,
                *m_offsetBuffer,
                *m_vertexBuffer,
                numNodes,
                bboxMin,
                bboxMax,
                *primitives,
                m_config.isoValue);
                
            std::vector<GpuVertex> hostVertices(static_cast<std::size_t>(totalVertices));
            queue.enqueueReadBuffer(*m_vertexBuffer,
                                    CL_TRUE,
                                    0,
                                    hostVertices.size() * sizeof(GpuVertex),
                                    hostVertices.data());

            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_mesh.positions.reserve(hostVertices.size());
            m_mesh.normals.reserve(hostVertices.size());

            for (auto const & vertex : hostVertices)
            {
                m_mesh.positions.emplace_back(vertex.position.s[0],
                                              vertex.position.s[1],
                                              vertex.position.s[2]);
                Eigen::Vector3f normal(vertex.normal.s[0], vertex.normal.s[1], vertex.normal.s[2]);
                if (normal.squaredNorm() > 1e-12F)
                {
                    normal.normalize();
                }
                else
                {
                    normal = Eigen::Vector3f{0.0F, 1.0F, 0.0F};
                }
                m_mesh.normals.emplace_back(normal);
            }
            
            std::cout << "Generated " << m_mesh.positions.size() << " vertices" << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "Error running emit_vertices: " << e.what() << std::endl;
            m_mesh.positions.clear();
            m_mesh.normals.clear();
            m_lastVertexCount = 0U;
        }
    }

    void ManifoldDualContouringGpu::generateIndices()
    {
        m_mesh.indices.clear();

        if (m_mesh.positions.empty() || m_cpuOctreeNodes.empty())
        {
            return;
        }

        if (m_cpuVertexOffsets.size() != m_cpuOctreeNodes.size() ||
            m_cpuVertexCounts.size() != m_cpuOctreeNodes.size())
        {
            std::cerr << "Vertex metadata is inconsistent with octree nodes" << std::endl;
            return;
        }

        std::unordered_set<std::uint64_t> processedEdges;
        processedEdges.reserve(m_cpuOctreeNodes.size() * 3U);

        for (std::size_t nodeIdx = 0; nodeIdx < m_cpuOctreeNodes.size(); ++nodeIdx)
        {
            auto const vertexIndex = vertexIndexForNode(nodeIdx);
            if (!vertexIndex.has_value())
            {
                continue;
            }

            OctreeNode const & node = m_cpuOctreeNodes[nodeIdx];
            if (node.edgeMask == 0U)
            {
                continue;
            }

            for (auto const & edge : kEdgeInfos)
            {
                if ((node.edgeMask & (1U << edge.edgeIdx)) == 0U)
                {
                    continue;
                }

                std::size_t const neighbor0 = findNeighborIndex(nodeIdx, edge.dx0, edge.dy0, edge.dz0);
                std::size_t const neighbor1 = findNeighborIndex(nodeIdx, edge.dx1, edge.dy1, edge.dz1);
                std::size_t const neighbor2 = findNeighborIndex(nodeIdx, edge.dx2, edge.dy2, edge.dz2);

                cl_ulong canonicalMorton = node.mortonCode;
                if (neighbor0 != kInvalidIndex)
                {
                    canonicalMorton = std::min(canonicalMorton, m_cpuOctreeNodes[neighbor0].mortonCode);
                }
                if (neighbor1 != kInvalidIndex)
                {
                    canonicalMorton = std::min(canonicalMorton, m_cpuOctreeNodes[neighbor1].mortonCode);
                }
                if (neighbor2 != kInvalidIndex)
                {
                    canonicalMorton = std::min(canonicalMorton, m_cpuOctreeNodes[neighbor2].mortonCode);
                }

                std::uint64_t const edgeKey = (static_cast<std::uint64_t>(canonicalMorton) << 8U) |
                                               static_cast<std::uint64_t>(edge.edgeIdx);
                if (!processedEdges.insert(edgeKey).second)
                {
                    continue;
                }

                std::array<std::optional<std::uint32_t>, 4U> quadVertices{
                    vertexIndex,
                    vertexIndexForNode(neighbor0),
                    vertexIndexForNode(neighbor2),
                    vertexIndexForNode(neighbor1)
                };

                std::vector<std::uint32_t> polygon;
                polygon.reserve(4U);
                for (auto const & maybeVertex : quadVertices)
                {
                    if (maybeVertex.has_value())
                    {
                        polygon.push_back(maybeVertex.value());
                    }
                }

                if (polygon.size() < 3U)
                {
                    continue;
                }

                emitTrianglesForPolygon(polygon);
            }
        }

        std::cout << "Generated " << (m_mesh.indices.size() / 3U) << " triangles" << std::endl;
    }

    void ManifoldDualContouringGpu::refreshCpuOctreeCache()
    {
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();

        if (!m_octreeBuffer || m_octreeNodeCount == 0U)
        {
            return;
        }

        m_cpuOctreeNodes.resize(m_octreeNodeCount);
        auto context = m_core.getComputeContext();
        auto & queue = context->GetQueue();

        queue.enqueueReadBuffer(*m_octreeBuffer,
                                CL_TRUE,
                                0,
                                m_octreeNodeCount * sizeof(OctreeNode),
                                m_cpuOctreeNodes.data());

        m_mortonToIndex.reserve(m_cpuOctreeNodes.size());
        for (std::size_t i = 0; i < m_cpuOctreeNodes.size(); ++i)
        {
            m_mortonToIndex[m_cpuOctreeNodes[i].mortonCode] = i;
        }
    }

    std::optional<std::uint32_t> ManifoldDualContouringGpu::vertexIndexForNode(std::size_t nodeIdx) const
    {
        if (nodeIdx == kInvalidIndex)
        {
            return std::nullopt;
        }

        if (nodeIdx >= m_cpuVertexCounts.size() || nodeIdx >= m_cpuVertexOffsets.size())
        {
            return std::nullopt;
        }

        if (m_cpuVertexCounts[nodeIdx] == 0)
        {
            return std::nullopt;
        }

        return static_cast<std::uint32_t>(m_cpuVertexOffsets[nodeIdx]);
    }

    void ManifoldDualContouringGpu::emitTrianglesForPolygon(std::vector<std::uint32_t> const & polygon)
    {
        if (polygon.size() < 3U)
        {
            return;
        }

        auto emitTriangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c)
        {
            if (a >= m_mesh.positions.size() || b >= m_mesh.positions.size() || c >= m_mesh.positions.size())
            {
                return;
            }

            Eigen::Vector3f const & va = m_mesh.positions[a];
            Eigen::Vector3f const & vb = m_mesh.positions[b];
            Eigen::Vector3f const & vc = m_mesh.positions[c];
            Eigen::Vector3f faceNormal = (vb - va).cross(vc - va);
            Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();

            if (m_mesh.normals.size() == m_mesh.positions.size())
            {
                avgNormal = (m_mesh.normals[a] + m_mesh.normals[b] + m_mesh.normals[c]) / 3.0F;
            }

            if (avgNormal.squaredNorm() > 1e-12F && faceNormal.dot(avgNormal) < 0.0F)
            {
                std::swap(b, c);
                faceNormal = -faceNormal;
            }

            if (faceNormal.squaredNorm() <= 1e-12F)
            {
                return;
            }

            m_mesh.indices.push_back(a);
            m_mesh.indices.push_back(b);
            m_mesh.indices.push_back(c);
        };

        if (polygon.size() == 3U)
        {
            emitTriangle(polygon[0], polygon[1], polygon[2]);
        }
        else
        {
            emitTriangle(polygon[0], polygon[1], polygon[2]);
            emitTriangle(polygon[0], polygon[2], polygon[3]);
        }
    }

    std::size_t ManifoldDualContouringGpu::findNeighborIndex(std::size_t nodeIdx, int dx, int dy, int dz) const
    {
        if (nodeIdx >= m_cpuOctreeNodes.size())
        {
            return kInvalidIndex;
        }

        MortonCoord const coord = decodeMorton(m_cpuOctreeNodes[nodeIdx].mortonCode);

        auto const adjust = [](std::uint32_t value, int delta) -> std::optional<std::uint32_t>
        {
            long long const candidate = static_cast<long long>(value) + static_cast<long long>(delta);
            if (candidate < 0LL || candidate > static_cast<long long>(std::numeric_limits<std::uint32_t>::max()))
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(candidate);
        };

        auto const nx = adjust(coord.x, dx);
        auto const ny = adjust(coord.y, dy);
        auto const nz = adjust(coord.z, dz);

        if (!nx.has_value() || !ny.has_value() || !nz.has_value())
        {
            return kInvalidIndex;
        }

        if (m_gridResolution > 0U)
        {
            if (nx.value() >= m_gridResolution || ny.value() >= m_gridResolution || nz.value() >= m_gridResolution)
            {
                return kInvalidIndex;
            }
        }

        std::uint64_t const neighborMorton = encodeMorton(nx.value(), ny.value(), nz.value());
        auto const it = m_mortonToIndex.find(neighborMorton);
        if (it == m_mortonToIndex.end())
        {
            return kInvalidIndex;
        }

        return it->second;
    }
}
