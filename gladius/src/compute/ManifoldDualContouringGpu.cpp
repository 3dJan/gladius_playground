#include "ManifoldDualContouringGpu.h"
#include "ManifoldDualContouringProgram.h"
#include "../DualContouringMesher.h"
#include "../Primitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gladius::compute
{
    namespace
    {
        struct GpuVertex
        {
            cl_float4 position;
            cl_float4 normal;
        };

        constexpr int kNeighborSearchRange = 2;
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

        inline std::uint64_t normalizeMortonToDepth(std::uint64_t morton,
                                                     std::uint32_t sourceDepth,
                                                     std::uint32_t targetDepth)
        {
            if (targetDepth == sourceDepth || targetDepth > 63U)
            {
                return morton;
            }

            std::uint64_t normalized = morton;
            if (targetDepth > sourceDepth)
            {
                std::uint32_t const delta = targetDepth - sourceDepth;
                normalized <<= (delta * 3U);
            }
            else if (sourceDepth > targetDepth)
            {
                std::uint32_t const delta = sourceDepth - targetDepth;
                normalized >>= (delta * 3U);
            }
            return normalized;
        }

                struct QuantizedPosition
                {
                        std::int64_t x{0};
                        std::int64_t y{0};
                        std::int64_t z{0};

                        [[nodiscard]] bool operator==(QuantizedPosition const &) const = default;
                };

                struct QuantizedPositionHash
                {
                        [[nodiscard]] std::size_t operator()(QuantizedPosition const & position) const noexcept
                        {
                                std::size_t seed = std::hash<std::int64_t>{}(position.x);
                                seed ^= std::hash<std::int64_t>{}(position.y) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
                                seed ^= std::hash<std::int64_t>{}(position.z) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
                                return seed;
                        }
                };

                [[nodiscard]] float computeMatchingTolerance(Eigen::Vector3f const & bboxSize,
                                                                                                         std::uint32_t gridResolution)
                {
                        Eigen::Vector3f const safeSize = bboxSize.cwiseMax(Eigen::Vector3f::Constant(1e-4F));
                        float const resolution = static_cast<float>(std::max<std::uint32_t>(gridResolution, 1U));
                        Eigen::Vector3f const cellSize = safeSize / resolution;
                        return std::max(cellSize.maxCoeff() * 0.5F, 1e-4F);
                }

        [[nodiscard]] std::size_t computeReferenceResolution(std::size_t depth)
        {
            std::size_t const cellCount = static_cast<std::size_t>(1ULL) << depth;
            return std::max<std::size_t>(cellCount + 1U, 2U);
        }
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
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();

        // Get bounding box from compute core
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available for octree construction" << std::endl;
            return;
        }
        m_cachedBoundingBox = bbox;
        
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

        if (m_mesh.positions.empty())
        {
            return;
        }

        if (!m_cachedBoundingBox.has_value())
        {
            return;
        }

        dual_contouring::DualContouringMesh referenceMesh{};
        Eigen::Vector3f gridMin = Eigen::Vector3f::Zero();
        Eigen::Vector3f gridSpacing = Eigen::Vector3f::Ones();
        std::size_t cellWidth = 0U;
        std::size_t cellHeight = 0U;
        std::size_t cellDepth = 0U;

        try
        {
            constexpr std::size_t kMaxShift = std::numeric_limits<std::size_t>::digits - 2U;
            std::size_t const normalizedDepth = std::min<std::size_t>(m_config.maxDepth, kMaxShift);

            dual_contouring::OctreeBuildConfig buildConfig{};
            buildConfig.isoValue = m_config.isoValue;
            buildConfig.maxDepth = normalizedDepth;
            buildConfig.sdfResolution = computeReferenceResolution(normalizedDepth);
            buildConfig.forceUniform = true;
            buildConfig.enableGpuSampling = false;
            buildConfig.enableCurvatureRefinement = false;
            buildConfig.enableBalancedRefinement = false;

            dual_contouring::OctreeBuilder builder(m_core, m_cachedBoundingBox.value(), buildConfig);
            dual_contouring::OctreeMetrics metrics{};
            auto root = builder.build(metrics);
            if (!root)
            {
                std::cerr << "Reference octree build failed; indices unavailable" << std::endl;
                return;
            }

            referenceMesh = dual_contouring::buildDualContouringMesh(builder, *root, buildConfig, nullptr);

            gridMin = builder.gridMin();
            gridSpacing = builder.gridSpacing().cwiseMax(Eigen::Vector3f::Constant(1e-6F));
            cellWidth = builder.gridWidth() > 0U ? builder.gridWidth() - 1U : 0U;
            cellHeight = builder.gridHeight() > 0U ? builder.gridHeight() - 1U : 0U;
            cellDepth = builder.gridDepth() > 0U ? builder.gridDepth() - 1U : 0U;
        }
        catch (std::exception const & e)
        {
            std::cerr << "Failed to build reference dual contouring mesh: " << e.what() << std::endl;
            return;
        }

        if (referenceMesh.faces.empty())
        {
            std::cout << "Reference mesh produced no faces; indices remain empty" << std::endl;
            return;
        }

        if (m_cpuOctreeNodes.empty() || m_cpuVertexOffsets.size() != m_cpuOctreeNodes.size() ||
            m_cpuVertexCounts.size() != m_cpuOctreeNodes.size())
        {
            std::cerr << "Octree metadata not available for GPU vertex mapping" << std::endl;
            return;
        }

                float const rawTolerance = computeMatchingTolerance(m_cachedBboxSize, m_gridResolution);
                float const matchingTolerance = std::max(rawTolerance, 1e-5F);
                float const matchingToleranceSq = matchingTolerance * matchingTolerance;
                double const invTolerance = 1.0 / static_cast<double>(matchingTolerance);

                auto bucketize = [invTolerance](Eigen::Vector3f const & position)
                {
                        return QuantizedPosition{static_cast<std::int64_t>(
                                                                                std::llround(static_cast<double>(position.x()) * invTolerance)),
                                                                        static_cast<std::int64_t>(
                                                                            std::llround(static_cast<double>(position.y()) * invTolerance)),
                                                                        static_cast<std::int64_t>(
                                                                            std::llround(static_cast<double>(position.z()) * invTolerance))};
                };

                std::unordered_map<QuantizedPosition, std::vector<std::uint32_t>, QuantizedPositionHash>
                    gpuVerticesByQuantizedPosition;
                gpuVerticesByQuantizedPosition.reserve(m_mesh.positions.size());
                std::vector<bool> gpuVertexUsed(m_mesh.positions.size(), false);

                for (std::uint32_t idx = 0U; idx < m_mesh.positions.size(); ++idx)
                {
                        auto const bucket = bucketize(m_mesh.positions[idx]);
                        gpuVerticesByQuantizedPosition[bucket].push_back(idx);
                }

        std::unordered_map<std::uint64_t, std::uint32_t> gpuVerticesByMorton;
        gpuVerticesByMorton.reserve(m_cpuOctreeNodes.size());
                for (std::size_t nodeIdx = 0; nodeIdx < m_cpuOctreeNodes.size(); ++nodeIdx)
        {
            if (m_cpuVertexCounts[nodeIdx] <= 0)
            {
                continue;
            }

                        auto const nodeDepth = static_cast<std::uint32_t>(m_cpuOctreeNodes[nodeIdx].depth);
                        std::uint64_t const normalizedCode =
                            normalizeMortonToDepth(m_cpuOctreeNodes[nodeIdx].mortonCode, nodeDepth, m_octreeDepth);
                        gpuVerticesByMorton[normalizedCode] = static_cast<std::uint32_t>(m_cpuVertexOffsets[nodeIdx]);
        }

        Eigen::Vector3f const invSpacing = gridSpacing.cwiseInverse();
        int const maxX = static_cast<int>((cellWidth > 0U ? cellWidth : 1U) - 1U);
        int const maxY = static_cast<int>((cellHeight > 0U ? cellHeight : 1U) - 1U);
        int const maxZ = static_cast<int>((cellDepth > 0U ? cellDepth : 1U) - 1U);

        std::vector<std::optional<std::uint32_t>> cpuToGpu(referenceMesh.vertices.size());
        auto const findQuantizedMatch = [&](Eigen::Vector3f const & reference) -> std::optional<std::uint32_t>
        {
            QuantizedPosition const baseBucket = bucketize(reference);
            std::optional<std::uint32_t> bestIndex;
            float bestDistanceSq = std::numeric_limits<float>::max();

            for (int dx = -kNeighborSearchRange; dx <= kNeighborSearchRange; ++dx)
            {
                for (int dy = -kNeighborSearchRange; dy <= kNeighborSearchRange; ++dy)
                {
                    for (int dz = -kNeighborSearchRange; dz <= kNeighborSearchRange; ++dz)
                    {
                        QuantizedPosition const candidateBucket{
                          baseBucket.x + dx, baseBucket.y + dy, baseBucket.z + dz};
                        auto bucketIt = gpuVerticesByQuantizedPosition.find(candidateBucket);
                        if (bucketIt == gpuVerticesByQuantizedPosition.end())
                        {
                            continue;
                        }

                        for (auto const gpuIndex : bucketIt->second)
                        {
                            if (gpuVertexUsed[gpuIndex])
                            {
                                continue;
                            }

                            Eigen::Vector3f const & gpuPosition = m_mesh.positions[gpuIndex];
                            float const distanceSq = (gpuPosition - reference).squaredNorm();
                            if (distanceSq > matchingToleranceSq)
                            {
                                continue;
                            }

                            if (!bestIndex.has_value() || distanceSq < bestDistanceSq)
                            {
                                bestIndex = gpuIndex;
                                bestDistanceSq = distanceSq;
                            }
                        }
                    }
                }
            }

            return bestIndex;
        };

        std::size_t unmatchedVertices = 0U;
        std::size_t mortonMatches = 0U;
        std::size_t fallbackMatches = 0U;
        for (std::size_t i = 0; i < referenceMesh.vertices.size(); ++i)
        {
            Eigen::Vector3f normalized = (referenceMesh.vertices[i] - gridMin).cwiseProduct(invSpacing);
            Eigen::Vector3i coords = normalized.array().floor().matrix().cast<int>();
            coords = coords.cwiseMax(Eigen::Vector3i::Zero());
            coords.x() = std::min(coords.x(), maxX);
            coords.y() = std::min(coords.y(), maxY);
            coords.z() = std::min(coords.z(), maxZ);

            std::uint64_t const morton = encodeMorton(static_cast<std::uint32_t>(coords.x()),
                                                      static_cast<std::uint32_t>(coords.y()),
                                                      static_cast<std::uint32_t>(coords.z()));

            bool matched = false;
            auto it = gpuVerticesByMorton.find(morton);
            if (it != gpuVerticesByMorton.end())
            {
                cpuToGpu[i] = it->second;
                gpuVertexUsed[it->second] = true;
                gpuVerticesByMorton.erase(it);
                mortonMatches += 1U;
                matched = true;
            }
            else
            {
                auto const fallback = findQuantizedMatch(referenceMesh.vertices[i]);
                if (fallback.has_value())
                {
                    cpuToGpu[i] = fallback.value();
                    gpuVertexUsed[fallback.value()] = true;
                    fallbackMatches += 1U;
                    matched = true;
                }
            }

            if (!matched)
            {
                unmatchedVertices += 1U;
            }
        }

        std::size_t skippedFaces = 0U;
        std::size_t degenerateTriangles = 0U;
        for (auto const & face : referenceMesh.faces)
        {
            std::array<int, 3> faceIndices{face.x(), face.y(), face.z()};
            std::array<std::uint32_t, 3> mapped{};
            bool valid = true;
            for (std::size_t corner = 0U; corner < faceIndices.size(); ++corner)
            {
                int const vertexIdx = faceIndices[corner];
                if (vertexIdx < 0)
                {
                    valid = false;
                    break;
                }
                std::size_t const cpuIndex = static_cast<std::size_t>(vertexIdx);
                if (cpuIndex >= cpuToGpu.size() || !cpuToGpu[cpuIndex].has_value())
                {
                    valid = false;
                    break;
                }
                mapped[corner] = cpuToGpu[cpuIndex].value();
            }

            if (!valid)
            {
                skippedFaces += 1U;
                continue;
            }

            Eigen::Vector3f const & va = m_mesh.positions[mapped[0]];
            Eigen::Vector3f const & vb = m_mesh.positions[mapped[1]];
            Eigen::Vector3f const & vc = m_mesh.positions[mapped[2]];
            Eigen::Vector3f const edge1 = vb - va;
            Eigen::Vector3f const edge2 = vc - va;
            if (edge1.cross(edge2).squaredNorm() <= 1e-12F)
            {
                degenerateTriangles += 1U;
                continue;
            }

            m_mesh.indices.insert(m_mesh.indices.end(), mapped.begin(), mapped.end());
        }

        std::cout << "Generated " << (m_mesh.indices.size() / 3U) << " triangles";
        std::cout << " (skipped faces " << skippedFaces << ", unmatched vertices "
                  << unmatchedVertices << ", degenerate triangles discarded "
                  << degenerateTriangles << ", morton matches " << mortonMatches
                  << ", fallback matches " << fallbackMatches << ")" << std::endl;
    }

    void ManifoldDualContouringGpu::refreshCpuOctreeCache()
    {
        m_cpuOctreeNodes.clear();
        m_mortonToIndex.clear();

        if (m_octreeNodeCount == 0U || !m_octreeBuffer)
        {
            return;
        }

        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable; cannot read octree buffer" << std::endl;
            return;
        }

        try
        {
            m_cpuOctreeNodes.resize(m_octreeNodeCount);
            context->GetQueue().enqueueReadBuffer(*m_octreeBuffer,
                                                  CL_TRUE,
                                                  0,
                                                  m_cpuOctreeNodes.size() * sizeof(OctreeNode),
                                                  m_cpuOctreeNodes.data());
        }
        catch (std::exception const & e)
        {
            std::cerr << "Failed to read octree buffer: " << e.what() << std::endl;
            m_cpuOctreeNodes.clear();
            return;
        }

        m_mortonToIndex.reserve(m_cpuOctreeNodes.size());
        for (std::size_t i = 0; i < m_cpuOctreeNodes.size(); ++i)
        {
            m_mortonToIndex[m_cpuOctreeNodes[i].mortonCode] = i;
        }
    }
}
