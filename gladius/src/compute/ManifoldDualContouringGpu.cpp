#include "ManifoldDualContouringGpu.h"
#include "ManifoldDualContouringProgram.h"
#include "../Primitives.h"
#include "../SlicerProgram.h"
#include "../Buffer.h"
#include "../ResourceContext.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
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

        // Ensure the program is compiled with the current model's SDF
        // This is important when switching between different models
        m_core.getProgramManager().recompileBlockingForManifoldDC();

        // Pre-fetch bounding box for chunking decision
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available" << std::endl;
            return;
        }
        m_cachedBoundingBox = bbox;
        m_cachedBboxMin = Eigen::Vector3f(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        m_cachedBboxMax = Eigen::Vector3f(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        m_cachedBboxSize = m_cachedBboxMax - m_cachedBboxMin;

        // Check if chunking is needed
        std::size_t const chunkDivisor = calculateChunkDivisor();
        bool const useChunking = m_config.enableChunking && 
                                 m_config.minFeatureSize > 0.0F && 
                                 chunkDivisor > 1U;

        if (useChunking)
        {
            std::cout << "Using chunked processing with " << chunkDivisor << "^3 = " 
                      << (chunkDivisor * chunkDivisor * chunkDivisor) << " potential chunks" << std::endl;
            std::cout << "  BBox: [" << m_cachedBboxMin.transpose() << "] to [" 
                      << m_cachedBboxMax.transpose() << "]" << std::endl;
            std::cout << "  minFeatureSize: " << m_config.minFeatureSize 
                      << ", maxDepth: " << m_config.maxDepth << std::endl;
            
            std::vector<ChunkInfo> chunks = generateChunkGrid();
            std::size_t processedChunks = 0U;
            std::size_t emptyChunks = 0U;
            
            ManifoldDualContouringMesh combinedMesh;
            
            for (auto const & chunk : chunks)
            {
                if (!isChunkNonEmpty(chunk))
                {
                    continue;
                }
                
                ++processedChunks;
                if (processedChunks <= 5U || processedChunks % 50U == 0U)
                {
                    std::cout << "  Processing chunk " << processedChunks << "/" << chunks.size()
                              << " [" << chunk.indexX << "," << chunk.indexY << "," 
                              << chunk.indexZ << "]..." << std::endl;
                }
                
                ManifoldDualContouringMesh chunkMesh;
                generateMeshForChunk(chunk, chunkMesh);
                
                // Note: Disabled clipping and snapping
                // Relying on overlap + welding to connect chunks
                // Each chunk may generate some duplicate geometry in padding region
                
                if (chunkMesh.positions.empty())
                {
                    ++emptyChunks;
                }
                else
                {
                    if (processedChunks <= 5U || processedChunks % 50U == 0U)
                    {
                        std::cout << "    Generated " << chunkMesh.positions.size() << " vertices, "
                                  << chunkMesh.indices.size() / 3U << " triangles" << std::endl;
                    }
                    mergeMeshes(combinedMesh, chunkMesh);
                }
            }
            
            m_mesh = std::move(combinedMesh);
            
            std::cout << "Chunk processing complete: " << processedChunks << " chunks processed, "
                      << emptyChunks << " produced no geometry" << std::endl;
            std::cout << "Combined mesh before welding: " << m_mesh.positions.size() << " vertices, "
                      << m_mesh.indices.size() / 3U << " triangles" << std::endl;
            
            // Weld boundary vertices to make mesh watertight
            if (!m_mesh.positions.empty())
            {
                // Calculate appropriate weld tolerance if not specified
                // Use a fraction of the voxel size at maxDepth as tolerance
                float weldTolerance = m_config.chunkWeldTolerance;
                if (weldTolerance <= 0.0F)
                {
                    // Chunk size / 2^maxDepth = voxel size within chunk
                    // Use 0.5 * voxel size as tolerance - conservative to avoid
                    // welding vertices that shouldn't be merged
                    Eigen::Vector3f const chunkSize = m_cachedBboxSize / static_cast<float>(chunkDivisor);
                    float const voxelSize = chunkSize.maxCoeff() / static_cast<float>(1U << m_config.maxDepth);
                    weldTolerance = voxelSize * 0.5F;
                    std::cout << "  Auto weld tolerance: " << weldTolerance << " (voxel size: " << voxelSize << ")" << std::endl;
                }
                weldBoundaryVertices(weldTolerance);
            }
        }
        else
        {
            // Original single-pass processing
            constructOctree();
            generateVertices();
            generateIndices();
        }
        
        // Post-processing for sharp features
        if (m_config.enableSharpFeaturePostProcess && !m_mesh.indices.empty())
        {
            postProcessSharpFeatures();
        }
        
        // Mesh simplification (after sharp feature processing)
        if (m_config.enableSimplification && !m_mesh.indices.empty())
        {
            simplifyMesh();
        }
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

        // IMPORTANT: Sort octree by Morton code BEFORE generating vertices.
        // This ensures vertexOffsets[i] corresponds to the sorted node at index i,
        // which is required for emit_indices to find correct neighbor vertices.
        m_program->sortOctreeByMorton(m_octreeBuffer, numNodes);
        
        // Get primitives
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "No primitives available" << std::endl;
            return;
        }

        Eigen::Vector3f bboxMin = m_cachedBboxMin;
        Eigen::Vector3f bboxMax = m_cachedBboxMax;
        
        // 1. Count vertices (1-4 per cell based on normal clustering)
        m_countBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
        
        try {
            m_program->countVertices(*m_octreeBuffer, *m_countBuffer, numNodes,
                                    bboxMin, bboxMax, *primitives, m_config.isoValue);
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

        m_cpuVertexOffsets = offsets;

        std::cout << "Generating " << totalVertices << " vertices from " << numNodes << " octree nodes" << std::endl;
        
        if (totalVertices == 0) {
            std::cout << "No vertices to generate" << std::endl;
            return;
        }

        m_lastVertexCount = static_cast<std::size_t>(totalVertices);
        
        m_offsetBuffer = context->createBufferChecked(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, 
            numNodes * sizeof(int), offsets.data());
            
        // Allocate vertex buffer (float4 position + float4 normal = 32 bytes)
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

        if (m_octreeNodeCount == 0U || !m_octreeBuffer)
        {
            std::cerr << "No octree data available for index generation" << std::endl;
            return;
        }

        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable" << std::endl;
            return;
        }

        auto & queue = context->GetQueue();
        std::size_t const numNodes = m_octreeNodeCount;

        try
        {
            // Note: Octree was already sorted in generateVertices()
            // The sorted order ensures vertexOffsets[i] matches sorted node i

            // Calculate maxCoord for bounds checking in kernels (2^depth - 1)
            std::uint32_t const maxCoord = m_gridResolution - 1U;

            // 1. Count quads per cell
            auto quadCountBuffer =
                context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
            m_program->countQuads(*m_octreeBuffer, *quadCountBuffer, numNodes, maxCoord);

            // 2. CPU-side prefix sum for index offsets
            std::vector<int> quadCounts(numNodes);
            queue.enqueueReadBuffer(*quadCountBuffer, CL_TRUE, 0, numNodes * sizeof(int), quadCounts.data());

            std::vector<int> indexOffsets(numNodes);
            int totalIndices = 0;
            for (std::size_t i = 0U; i < numNodes; ++i)
            {
                indexOffsets[i] = totalIndices;
                totalIndices += quadCounts[i];
            }

            if (totalIndices == 0)
            {
                return;
            }

            auto indexOffsetBuffer = context->createBufferChecked(
                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, numNodes * sizeof(int), indexOffsets.data());

            // 4. Allocate index buffer and emit indices
            m_indexBuffer =
                context->createBufferChecked(CL_MEM_READ_WRITE, totalIndices * sizeof(std::uint32_t));

            m_program->generateIndices(
                *m_octreeBuffer, *m_offsetBuffer, *indexOffsetBuffer,
                *m_indexBuffer, numNodes, maxCoord);

            // 5. Read back indices
            m_mesh.indices.resize(static_cast<std::size_t>(totalIndices));
            queue.enqueueReadBuffer(
                *m_indexBuffer, CL_TRUE, 0, totalIndices * sizeof(std::uint32_t), m_mesh.indices.data());

            std::cout << "Generated " << (m_mesh.indices.size() / 3U) << " triangles via GPU" << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "GPU index generation failed: " << e.what() << std::endl;
            m_mesh.indices.clear();
        }
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

    void ManifoldDualContouringGpu::postProcessSharpFeatures()
    {
        std::cout << "Starting sharp feature post-processing..." << std::endl;
        
        // Step 1: Detect triangles with sharp features
        auto sharpTriangles = detectSharpTriangles();
        
        if (sharpTriangles.empty())
        {
            std::cout << "No sharp triangles detected, skipping post-processing" << std::endl;
            return;
        }
        
        std::cout << "Detected " << sharpTriangles.size() << " sharp triangles for processing" << std::endl;
        
        // Step 2: Subdivide the flagged triangles
        for (std::size_t iter = 0U; iter < m_config.subdivisionIterations; ++iter)
        {
            subdivideTriangles(sharpTriangles);
            
            // Re-detect sharp triangles for next iteration
            if (iter + 1U < m_config.subdivisionIterations)
            {
                sharpTriangles = detectSharpTriangles();
            }
        }
        
        // Step 3: Project new vertices to the SDF surface
        if (m_config.projectToSurface)
        {
            projectVerticesToSurface();
        }
        
        std::cout << "Sharp feature post-processing complete" << std::endl;
    }

    std::vector<std::size_t> ManifoldDualContouringGpu::detectSharpTriangles()
    {
        std::vector<std::size_t> result;
        
        if (m_mesh.indices.size() < 3U || m_mesh.normals.empty())
        {
            return result;
        }
        
        // The threshold is already a cosine value in the config
        float const cosThreshold = m_config.sharpFeatureAngleThreshold;
        
        std::size_t const numTriangles = m_mesh.indices.size() / 3U;
        result.reserve(numTriangles / 10U); // Assume ~10% might be sharp
        
        for (std::size_t triIdx = 0U; triIdx < numTriangles; ++triIdx)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];
            
            // Get vertex normals
            if (i0 >= m_mesh.normals.size() || 
                i1 >= m_mesh.normals.size() || 
                i2 >= m_mesh.normals.size())
            {
                continue;
            }
            
            auto const & n0 = m_mesh.normals[i0];
            auto const & n1 = m_mesh.normals[i1];
            auto const & n2 = m_mesh.normals[i2];
            
            // Check dot products between vertex normals (using Eigen Vector3f member functions)
            float const dot01 = n0.x() * n1.x() + n0.y() * n1.y() + n0.z() * n1.z();
            float const dot12 = n1.x() * n2.x() + n1.y() * n2.y() + n1.z() * n2.z();
            float const dot20 = n2.x() * n0.x() + n2.y() * n0.y() + n2.z() * n0.z();
            
            // If any pair of normals differs significantly, flag this triangle
            if (dot01 < cosThreshold || dot12 < cosThreshold || dot20 < cosThreshold)
            {
                result.push_back(triIdx);
            }
        }
        
        return result;
    }

    void ManifoldDualContouringGpu::subdivideTriangles(std::vector<std::size_t> const & triangleIndices)
    {
        if (triangleIndices.empty())
        {
            return;
        }
        
        // For manifold subdivision, we need to track edges and their midpoints
        // to ensure shared edges get the same midpoint vertex
        struct Edge
        {
            std::uint32_t v0;
            std::uint32_t v1;
            
            bool operator<(Edge const & other) const
            {
                auto const minThis = std::min(v0, v1);
                auto const maxThis = std::max(v0, v1);
                auto const minOther = std::min(other.v0, other.v1);
                auto const maxOther = std::max(other.v0, other.v1);
                return (minThis < minOther) || (minThis == minOther && maxThis < maxOther);
            }
            
            bool operator==(Edge const & other) const
            {
                auto const minThis = std::min(v0, v1);
                auto const maxThis = std::max(v0, v1);
                auto const minOther = std::min(other.v0, other.v1);
                auto const maxOther = std::max(other.v0, other.v1);
                return minThis == minOther && maxThis == maxOther;
            }
        };
        
        // Map from edge to its midpoint vertex index
        std::map<Edge, std::uint32_t> edgeMidpoints;
        
        // Helper to get or create midpoint vertex for an edge
        auto getOrCreateMidpoint = [&](std::uint32_t v0, std::uint32_t v1) -> std::uint32_t
        {
            Edge edge{v0, v1};
            auto it = edgeMidpoints.find(edge);
            if (it != edgeMidpoints.end())
            {
                return it->second;
            }
            
            // Create new midpoint vertex
            auto const & p0 = m_mesh.positions[v0];
            auto const & p1 = m_mesh.positions[v1];
            Eigen::Vector3f midPos{
                (p0.x() + p1.x()) * 0.5F,
                (p0.y() + p1.y()) * 0.5F,
                (p0.z() + p1.z()) * 0.5F
            };
            
            // Interpolate normal
            auto const & n0 = m_mesh.normals[v0];
            auto const & n1 = m_mesh.normals[v1];
            Eigen::Vector3f midNormal{
                (n0.x() + n1.x()) * 0.5F,
                (n0.y() + n1.y()) * 0.5F,
                (n0.z() + n1.z()) * 0.5F
            };
            
            // Normalize the interpolated normal
            float const len = midNormal.norm();
            if (len > 1e-6F)
            {
                midNormal /= len;
            }
            
            auto const newIdx = static_cast<std::uint32_t>(m_mesh.positions.size());
            m_mesh.positions.push_back(midPos);
            m_mesh.normals.push_back(midNormal);
            
            edgeMidpoints[edge] = newIdx;
            return newIdx;
        };
        
        // Collect triangles to subdivide into a set for O(1) lookup
        std::set<std::size_t> trianglesToSubdivide(triangleIndices.begin(), triangleIndices.end());
        
        // First pass: collect all edges that will be split (from triangles to subdivide)
        std::set<Edge> edgesToSplit;
        for (auto triIdx : trianglesToSubdivide)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];
            edgesToSplit.insert(Edge{i0, i1});
            edgesToSplit.insert(Edge{i1, i2});
            edgesToSplit.insert(Edge{i2, i0});
        }
        
        // Build new index buffer
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(m_mesh.indices.size() + triangleIndices.size() * 9U);
        
        std::size_t const numTriangles = m_mesh.indices.size() / 3U;
        for (std::size_t triIdx = 0U; triIdx < numTriangles; ++triIdx)
        {
            std::size_t const baseIdx = triIdx * 3U;
            auto const i0 = m_mesh.indices[baseIdx + 0U];
            auto const i1 = m_mesh.indices[baseIdx + 1U];
            auto const i2 = m_mesh.indices[baseIdx + 2U];
            
            if (trianglesToSubdivide.count(triIdx) > 0U)
            {
                // Subdivide this triangle into 4 triangles using midpoints
                // Create midpoint on each edge
                auto const m01 = getOrCreateMidpoint(i0, i1);
                auto const m12 = getOrCreateMidpoint(i1, i2);
                auto const m20 = getOrCreateMidpoint(i2, i0);
                
                // Create 4 new triangles:
                // Triangle 1: i0, m01, m20
                newIndices.push_back(i0);
                newIndices.push_back(m01);
                newIndices.push_back(m20);
                
                // Triangle 2: m01, i1, m12
                newIndices.push_back(m01);
                newIndices.push_back(i1);
                newIndices.push_back(m12);
                
                // Triangle 3: m20, m12, i2
                newIndices.push_back(m20);
                newIndices.push_back(m12);
                newIndices.push_back(i2);
                
                // Triangle 4: m01, m12, m20 (center triangle)
                newIndices.push_back(m01);
                newIndices.push_back(m12);
                newIndices.push_back(m20);
            }
            else
            {
                // Check if any edges of this triangle need to be split
                // to match neighboring subdivided triangles (T-junction fix)
                bool const split01 = edgesToSplit.count(Edge{i0, i1}) > 0U;
                bool const split12 = edgesToSplit.count(Edge{i1, i2}) > 0U;
                bool const split20 = edgesToSplit.count(Edge{i2, i0}) > 0U;
                
                int const splitCount = (split01 ? 1 : 0) + (split12 ? 1 : 0) + (split20 ? 1 : 0);
                
                if (splitCount == 0)
                {
                    // No edges to split, keep original triangle
                    newIndices.push_back(i0);
                    newIndices.push_back(i1);
                    newIndices.push_back(i2);
                }
                else if (splitCount == 1)
                {
                    // One edge split: create 2 triangles
                    if (split01)
                    {
                        auto const m = getOrCreateMidpoint(i0, i1);
                        newIndices.push_back(i0);
                        newIndices.push_back(m);
                        newIndices.push_back(i2);
                        
                        newIndices.push_back(m);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                    else if (split12)
                    {
                        auto const m = getOrCreateMidpoint(i1, i2);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m);
                        
                        newIndices.push_back(i0);
                        newIndices.push_back(m);
                        newIndices.push_back(i2);
                    }
                    else // split20
                    {
                        auto const m = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m);
                        
                        newIndices.push_back(m);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                }
                else if (splitCount == 2)
                {
                    // Two edges split: create 3 triangles
                    if (!split01) // split12 and split20
                    {
                        auto const m12 = getOrCreateMidpoint(i1, i2);
                        auto const m20 = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(i1);
                        newIndices.push_back(m12);
                        
                        newIndices.push_back(i0);
                        newIndices.push_back(m12);
                        newIndices.push_back(m20);
                        
                        newIndices.push_back(m20);
                        newIndices.push_back(m12);
                        newIndices.push_back(i2);
                    }
                    else if (!split12) // split01 and split20
                    {
                        auto const m01 = getOrCreateMidpoint(i0, i1);
                        auto const m20 = getOrCreateMidpoint(i2, i0);
                        newIndices.push_back(i0);
                        newIndices.push_back(m01);
                        newIndices.push_back(m20);
                        
                        newIndices.push_back(m01);
                        newIndices.push_back(i1);
                        newIndices.push_back(m20);
                        
                        newIndices.push_back(m20);
                        newIndices.push_back(i1);
                        newIndices.push_back(i2);
                    }
                    else // split01 and split12
                    {
                        auto const m01 = getOrCreateMidpoint(i0, i1);
                        auto const m12 = getOrCreateMidpoint(i1, i2);
                        newIndices.push_back(i0);
                        newIndices.push_back(m01);
                        newIndices.push_back(i2);
                        
                        newIndices.push_back(m01);
                        newIndices.push_back(i1);
                        newIndices.push_back(m12);
                        
                        newIndices.push_back(m01);
                        newIndices.push_back(m12);
                        newIndices.push_back(i2);
                    }
                }
                else // splitCount == 3
                {
                    // All three edges split: create 4 triangles (same as full subdivide)
                    auto const m01 = getOrCreateMidpoint(i0, i1);
                    auto const m12 = getOrCreateMidpoint(i1, i2);
                    auto const m20 = getOrCreateMidpoint(i2, i0);
                    
                    newIndices.push_back(i0);
                    newIndices.push_back(m01);
                    newIndices.push_back(m20);
                    
                    newIndices.push_back(m01);
                    newIndices.push_back(i1);
                    newIndices.push_back(m12);
                    
                    newIndices.push_back(m20);
                    newIndices.push_back(m12);
                    newIndices.push_back(i2);
                    
                    newIndices.push_back(m01);
                    newIndices.push_back(m12);
                    newIndices.push_back(m20);
                }
            }
        }
        
        m_mesh.indices = std::move(newIndices);
        
        std::cout << "Subdivision added " << edgeMidpoints.size() << " midpoint vertices" << std::endl;
        std::cout << "New triangle count: " << (m_mesh.indices.size() / 3U) << std::endl;
    }

    void ManifoldDualContouringGpu::projectVerticesToSurface()
    {
        auto context = m_core.getComputeContext();
        if (!context)
        {
            std::cerr << "Compute context unavailable for vertex projection" << std::endl;
            return;
        }
        
        try
        {
            std::size_t const numVertices = m_mesh.positions.size();
            if (numVertices == 0U)
            {
                return;
            }
            
            // Create a VertexBuffer and populate it with positions as cl_float4
            VertexBuffer vertexBuffer(*context);
            auto& bufferData = vertexBuffer.getData();
            bufferData.resize(numVertices);
            
            for (std::size_t i = 0U; i < numVertices; ++i)
            {
                auto const & pos = m_mesh.positions[i];
                bufferData[i] = {pos.x(), pos.y(), pos.z(), 0.0F};
            }
            
            // Write to GPU
            vertexBuffer.write();
            
            // Use ComputeCore's adoptVertexOfMeshToSurface which handles primitives setup
            m_core.adoptVertexOfMeshToSurface(vertexBuffer);
            
            // Read back projected positions
            vertexBuffer.read();
            
            // Convert back to Eigen::Vector3f
            for (std::size_t i = 0U; i < numVertices; ++i)
            {
                auto const & pos = bufferData[i];
                m_mesh.positions[i] = Eigen::Vector3f{pos.s[0], pos.s[1], pos.s[2]};
            }
            
            std::cout << "Projected " << numVertices << " vertices to SDF surface" << std::endl;
        }
        catch (std::exception const & e)
        {
            std::cerr << "Vertex projection failed: " << e.what() << std::endl;
        }
    }

    float ManifoldDualContouringGpu::evaluateSdf(Eigen::Vector3f const & pos) const
    {
        // For single-point SDF evaluation, we use the precomputed SDF grid if available
        // This is a simplified approach - for production, consider caching the SDF grid
        auto resources = m_core.getResourceContext();
        if (!resources)
        {
            return 0.0F;
        }

        auto & sdfBuffer = resources->getPrecompSdfBuffer();
        auto const width = sdfBuffer.getWidth();
        auto const height = sdfBuffer.getHeight();
        auto const depth = sdfBuffer.getDepth();
        
        if (width == 0U || height == 0U || depth == 0U || !m_cachedBoundingBox.has_value())
        {
            return 0.0F;
        }

        // Transform world position to normalized coordinates within the bounding box
        Eigen::Vector3f const extent = m_cachedBboxMax - m_cachedBboxMin;
        Eigen::Vector3f const safeExtent = extent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
        Eigen::Vector3f normalized = (pos - m_cachedBboxMin).cwiseQuotient(safeExtent);
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

        float const fx = gx - static_cast<float>(x0);
        float const fy = gy - static_cast<float>(y0);
        float const fz = gz - static_cast<float>(z0);

        // Trilinear interpolation
        auto const & data = sdfBuffer.getData();
        auto index = [&](std::size_t x, std::size_t y, std::size_t z) -> std::size_t {
            return z * width * height + y * width + x;
        };

        float const v000 = data[index(x0, y0, z0)];
        float const v100 = data[index(x1, y0, z0)];
        float const v010 = data[index(x0, y1, z0)];
        float const v110 = data[index(x1, y1, z0)];
        float const v001 = data[index(x0, y0, z1)];
        float const v101 = data[index(x1, y0, z1)];
        float const v011 = data[index(x0, y1, z1)];
        float const v111 = data[index(x1, y1, z1)];

        float const c00 = v000 * (1.0F - fx) + v100 * fx;
        float const c10 = v010 * (1.0F - fx) + v110 * fx;
        float const c01 = v001 * (1.0F - fx) + v101 * fx;
        float const c11 = v011 * (1.0F - fx) + v111 * fx;
        float const c0 = c00 * (1.0F - fy) + c10 * fy;
        float const c1 = c01 * (1.0F - fy) + c11 * fy;
        
        return c0 * (1.0F - fz) + c1 * fz;
    }

    void ManifoldDualContouringGpu::simplifyMesh()
    {
        std::cout << "Starting mesh simplification..." << std::endl;
        std::size_t const initialTriangles = m_mesh.indices.size() / 3U;
        std::size_t const initialVertices = m_mesh.positions.size();
        
        if (initialTriangles < 2U || initialVertices < 4U)
        {
            std::cout << "Mesh too small for simplification" << std::endl;
            return;
        }

        // Calculate base voxel size for edge length threshold
        float baseEdgeLength = m_config.simplificationMinEdgeLength;
        if (baseEdgeLength <= 0.0F && m_cachedBoundingBox.has_value())
        {
            Eigen::Vector3f const extent = m_cachedBboxMax - m_cachedBboxMin;
            float const voxelSize = extent.maxCoeff() / static_cast<float>(m_gridResolution);
            baseEdgeLength = voxelSize * 0.5F * m_config.simplificationAggressiveness;
        }

        std::size_t totalCollapsed = 0U;
        std::size_t const maxPasses = m_config.simplificationPasses;
        
        // Multi-pass simplification - each pass can collapse more edges
        for (std::size_t pass = 0U; pass < maxPasses; ++pass)
        {
            // Increase edge length threshold slightly each pass
            float const passMultiplier = 1.0F + static_cast<float>(pass) * 0.25F;
            float const minEdgeLength = baseEdgeLength * passMultiplier;
            
            // Slightly relax the flat threshold in later passes
            float const flatThreshold = m_config.simplificationFlatThreshold - 
                                        static_cast<float>(pass) * 0.05F;
            
            std::size_t const passCollapsed = simplifyMeshPass(minEdgeLength, flatThreshold);
            totalCollapsed += passCollapsed;
            
            if (passCollapsed == 0U)
            {
                // No more edges to collapse
                break;
            }
            
            std::cout << "  Pass " << (pass + 1) << ": collapsed " << passCollapsed << " edges" << std::endl;
        }

        if (totalCollapsed == 0U)
        {
            std::cout << "No edges could be collapsed" << std::endl;
            return;
        }

        std::size_t const finalTriangles = m_mesh.indices.size() / 3U;
        std::size_t const finalVertices = m_mesh.positions.size();
        
        float const reductionPercent = 100.0F * static_cast<float>(initialTriangles - finalTriangles) / 
                                       static_cast<float>(initialTriangles);
        
        std::cout << "Mesh simplification complete:" << std::endl;
        std::cout << "  Triangles: " << initialTriangles << " -> " << finalTriangles 
                  << " (removed " << (initialTriangles - finalTriangles) 
                  << ", " << std::fixed << std::setprecision(1) << reductionPercent << "%)" << std::endl;
        std::cout << "  Vertices: " << initialVertices << " -> " << finalVertices 
                  << " (removed " << (initialVertices - finalVertices) << ")" << std::endl;
        std::cout << "  Total edges collapsed: " << totalCollapsed << std::endl;
    }
    
    std::size_t ManifoldDualContouringGpu::simplifyMeshPass(float minEdgeLength, float flatThreshold)
    {
        // Build edge-to-triangles adjacency
        struct Edge
        {
            std::uint32_t v0;
            std::uint32_t v1;
            
            bool operator==(Edge const & other) const
            {
                auto const a0 = std::min(v0, v1);
                auto const a1 = std::max(v0, v1);
                auto const b0 = std::min(other.v0, other.v1);
                auto const b1 = std::max(other.v0, other.v1);
                return a0 == b0 && a1 == b1;
            }
        };
        
        struct EdgeHash
        {
            std::size_t operator()(Edge const & e) const
            {
                auto const low = std::min(e.v0, e.v1);
                auto const high = std::max(e.v0, e.v1);
                return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(low) << 32U | high);
            }
        };

        // Map each edge to the triangles that use it
        std::unordered_map<Edge, std::vector<std::size_t>, EdgeHash> edgeTriangles;
        
        // Map each vertex to its triangles
        std::vector<std::vector<std::size_t>> vertexTriangles(m_mesh.positions.size());
        std::size_t const numTriangles = m_mesh.indices.size() / 3U;
        
        for (std::size_t t = 0U; t < numTriangles; ++t)
        {
            std::uint32_t const i0 = m_mesh.indices[t * 3U + 0U];
            std::uint32_t const i1 = m_mesh.indices[t * 3U + 1U];
            std::uint32_t const i2 = m_mesh.indices[t * 3U + 2U];
            
            vertexTriangles[i0].push_back(t);
            vertexTriangles[i1].push_back(t);
            vertexTriangles[i2].push_back(t);
            
            edgeTriangles[Edge{i0, i1}].push_back(t);
            edgeTriangles[Edge{i1, i2}].push_back(t);
            edgeTriangles[Edge{i2, i0}].push_back(t);
        }

        // Collect candidate edges for collapse
        struct CollapseCandidate
        {
            Edge edge;
            float length;
            float error; // Combined metric: length + curvature penalty
        };
        std::vector<CollapseCandidate> candidates;
        
        for (auto const & [edge, triangles] : edgeTriangles)
        {
            // Only collapse interior edges (exactly 2 triangles share this edge)
            if (triangles.size() != 2U)
            {
                continue;
            }
            
            auto const & p0 = m_mesh.positions[edge.v0];
            auto const & p1 = m_mesh.positions[edge.v1];
            float const length = (p1 - p0).norm();
            
            // Skip edges that are too long
            if (length > minEdgeLength)
            {
                continue;
            }
            
            // Check if normals are similar (flat region)
            auto const & n0 = m_mesh.normals[edge.v0];
            auto const & n1 = m_mesh.normals[edge.v1];
            float const normalDot = n0.dot(n1);
            
            // Only consider edges in relatively flat regions
            if (normalDot >= flatThreshold)
            {
                // Error metric: shorter edges with more similar normals are better
                float const curvaturePenalty = 1.0F - normalDot;
                float const error = length * (1.0F + curvaturePenalty * 10.0F);
                candidates.push_back({edge, length, error});
            }
        }
        
        if (candidates.empty())
        {
            return 0U;
        }

        // Sort candidates by error metric (lowest error first)
        std::sort(candidates.begin(), candidates.end(),
                  [](CollapseCandidate const & a, CollapseCandidate const & b)
                  {
                      return a.error < b.error;
                  });

        // Track which vertices have been collapsed
        std::vector<std::uint32_t> vertexRemap(m_mesh.positions.size());
        std::iota(vertexRemap.begin(), vertexRemap.end(), 0U);
        
        // Track which vertices have been touched this pass
        std::vector<bool> vertexTouched(m_mesh.positions.size(), false);
        
        auto getFinalVertex = [&vertexRemap](std::uint32_t v) -> std::uint32_t
        {
            while (vertexRemap[v] != v)
            {
                v = vertexRemap[v];
            }
            return v;
        };

        std::size_t collapsedCount = 0U;

        for (auto const & candidate : candidates)
        {
            std::uint32_t const v0 = getFinalVertex(candidate.edge.v0);
            std::uint32_t const v1 = getFinalVertex(candidate.edge.v1);
            
            if (v0 == v1)
            {
                continue;
            }
            
            if (vertexTouched[v0] || vertexTouched[v1])
            {
                continue;
            }

            Eigen::Vector3f const & p0 = m_mesh.positions[v0];
            Eigen::Vector3f const & p1 = m_mesh.positions[v1];
            Eigen::Vector3f const midpoint = (p0 + p1) * 0.5F;
            
            // Check SDF error at midpoint
            float const sdfError = std::abs(evaluateSdf(midpoint));
            if (sdfError > m_config.simplificationMaxError)
            {
                continue;
            }

            // Check for triangle inversions
            bool wouldInvert = false;
            
            auto checkTrianglesForInversion = [&](std::uint32_t vertex) -> bool
            {
                for (std::size_t triIdx : vertexTriangles[vertex])
                {
                    std::size_t const baseIdx = triIdx * 3U;
                    std::uint32_t triVerts[3] = {
                        getFinalVertex(m_mesh.indices[baseIdx + 0U]),
                        getFinalVertex(m_mesh.indices[baseIdx + 1U]),
                        getFinalVertex(m_mesh.indices[baseIdx + 2U])
                    };
                    
                    if (triVerts[0] == triVerts[1] || triVerts[1] == triVerts[2] || triVerts[2] == triVerts[0])
                    {
                        continue;
                    }
                    
                    bool const hasV0 = (triVerts[0] == v0 || triVerts[1] == v0 || triVerts[2] == v0);
                    bool const hasV1 = (triVerts[0] == v1 || triVerts[1] == v1 || triVerts[2] == v1);
                    if (hasV0 && hasV1)
                    {
                        continue;
                    }

                    Eigen::Vector3f const & a = m_mesh.positions[triVerts[0]];
                    Eigen::Vector3f const & b = m_mesh.positions[triVerts[1]];
                    Eigen::Vector3f const & c = m_mesh.positions[triVerts[2]];
                    
                    Eigen::Vector3f const normalBefore = (b - a).cross(c - a);
                    
                    auto getNewPos = [&](std::uint32_t idx) -> Eigen::Vector3f
                    {
                        if (idx == v0 || idx == v1)
                        {
                            return midpoint;
                        }
                        return m_mesh.positions[idx];
                    };
                    
                    Eigen::Vector3f const aPrime = getNewPos(triVerts[0]);
                    Eigen::Vector3f const bPrime = getNewPos(triVerts[1]);
                    Eigen::Vector3f const cPrime = getNewPos(triVerts[2]);
                    
                    Eigen::Vector3f const normalAfter = (bPrime - aPrime).cross(cPrime - aPrime);
                    
                    float const normalLenBefore = normalBefore.norm();
                    float const normalLenAfter = normalAfter.norm();
                    
                    if (normalLenBefore > 1e-10F && normalLenAfter > 1e-10F)
                    {
                        if (normalBefore.dot(normalAfter) < 0.0F)
                        {
                            return true;
                        }
                    }
                }
                return false;
            };
            
            wouldInvert = checkTrianglesForInversion(v0) || checkTrianglesForInversion(v1);

            if (wouldInvert)
            {
                continue;
            }

            // Perform the collapse
            vertexRemap[v1] = v0;
            vertexTouched[v0] = true;
            vertexTouched[v1] = true;
            
            m_mesh.positions[v0] = midpoint;
            
            Eigen::Vector3f avgNormal = (m_mesh.normals[v0] + m_mesh.normals[v1]).normalized();
            m_mesh.normals[v0] = avgNormal;
            
            ++collapsedCount;
        }

        if (collapsedCount == 0U)
        {
            return 0U;
        }

        // Rebuild index buffer
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(m_mesh.indices.size());
        
        for (std::size_t t = 0U; t < numTriangles; ++t)
        {
            std::size_t const baseIdx = t * 3U;
            std::uint32_t const i0 = getFinalVertex(m_mesh.indices[baseIdx + 0U]);
            std::uint32_t const i1 = getFinalVertex(m_mesh.indices[baseIdx + 1U]);
            std::uint32_t const i2 = getFinalVertex(m_mesh.indices[baseIdx + 2U]);
            
            if (i0 != i1 && i1 != i2 && i2 != i0)
            {
                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
            }
        }

        m_mesh.indices = std::move(newIndices);

        // Compact vertex buffer
        std::vector<bool> vertexUsed(m_mesh.positions.size(), false);
        for (auto idx : m_mesh.indices)
        {
            vertexUsed[idx] = true;
        }

        std::vector<std::uint32_t> newVertexIndices(m_mesh.positions.size(), 0U);
        std::vector<Eigen::Vector3f> newPositions;
        std::vector<Eigen::Vector3f> newNormals;
        newPositions.reserve(m_mesh.positions.size());
        newNormals.reserve(m_mesh.normals.size());

        std::uint32_t newIdx = 0U;
        for (std::size_t i = 0U; i < m_mesh.positions.size(); ++i)
        {
            if (vertexUsed[i])
            {
                newVertexIndices[i] = newIdx++;
                newPositions.push_back(m_mesh.positions[i]);
                if (i < m_mesh.normals.size())
                {
                    newNormals.push_back(m_mesh.normals[i]);
                }
            }
        }

        for (auto & idx : m_mesh.indices)
        {
            idx = newVertexIndices[idx];
        }

        m_mesh.positions = std::move(newPositions);
        m_mesh.normals = std::move(newNormals);

        return collapsedCount;
    }

    // ============================================================================
    // Chunked Processing for Large Models with Fine Features
    // ============================================================================

    std::size_t ManifoldDualContouringGpu::calculateRequiredDepth(float bboxExtent, float minFeatureSize) const
    {
        if (minFeatureSize <= 0.0F || bboxExtent <= 0.0F)
        {
            return m_config.maxDepth;
        }
        
        // Cell size at depth d = bboxExtent / 2^d
        // We need cell size <= minFeatureSize
        // So 2^d >= bboxExtent / minFeatureSize
        // d >= log2(bboxExtent / minFeatureSize)
        float const ratio = bboxExtent / minFeatureSize;
        auto const requiredDepth = static_cast<std::size_t>(std::ceil(std::log2(ratio)));
        return std::max(requiredDepth, std::size_t{1U});
    }

    std::size_t ManifoldDualContouringGpu::calculateChunkDivisor() const
    {
        if (m_config.minFeatureSize <= 0.0F || !m_cachedBoundingBox.has_value())
        {
            return 1U; // No chunking needed
        }
        
        Eigen::Vector3f const bboxSize = m_cachedBboxMax - m_cachedBboxMin;
        float const maxExtent = bboxSize.maxCoeff();
        
        std::size_t const requiredDepth = calculateRequiredDepth(maxExtent, m_config.minFeatureSize);
        
        if (requiredDepth <= m_config.maxDepth)
        {
            return 1U; // Can handle with single octree
        }
        
        // Number of subdivisions needed: 2^(requiredDepth - maxDepth)
        std::size_t const depthDiff = requiredDepth - m_config.maxDepth;
        return std::size_t{1U} << depthDiff;
    }

    std::vector<ManifoldDualContouringGpu::ChunkInfo> ManifoldDualContouringGpu::generateChunkGrid() const
    {
        std::vector<ChunkInfo> chunks;
        
        if (!m_cachedBoundingBox.has_value())
        {
            return chunks;
        }
        
        std::size_t const divisor = calculateChunkDivisor();
        
        // Global grid approach: all chunks share the same voxel grid
        // Total voxels across full bbox = divisor * 2^maxDepth
        std::size_t const cellsPerChunk = std::size_t{1U} << m_config.maxDepth;
        std::size_t const totalCells = divisor * cellsPerChunk;
        
        // Global voxel size - same for all chunks
        Eigen::Vector3f const globalVoxelSize = m_cachedBboxSize / static_cast<float>(totalCells);
        
        // Chunk size in world units (aligned to global grid)
        Eigen::Vector3f const chunkSize = globalVoxelSize * static_cast<float>(cellsPerChunk);
        
        // For DC, we need 1 extra cell at each internal boundary to generate boundary quads
        // The boundary cells will be duplicated in adjacent chunks, but at exact same positions
        Eigen::Vector3f const boundaryPadding = globalVoxelSize;
        
        chunks.reserve(divisor * divisor * divisor);
        
        for (std::size_t iz = 0U; iz < divisor; ++iz)
        {
            for (std::size_t iy = 0U; iy < divisor; ++iy)
            {
                for (std::size_t ix = 0U; ix < divisor; ++ix)
                {
                    ChunkInfo chunk;
                    chunk.indexX = ix;
                    chunk.indexY = iy;
                    chunk.indexZ = iz;
                    
                    // Core region: exactly aligned chunk (no overlap)
                    chunk.coreMin = m_cachedBboxMin + Eigen::Vector3f(
                        static_cast<float>(ix) * chunkSize.x(),
                        static_cast<float>(iy) * chunkSize.y(),
                        static_cast<float>(iz) * chunkSize.z()
                    );
                    chunk.coreMax = chunk.coreMin + chunkSize;
                    
                    // Processing region: add 1-cell padding at internal boundaries
                    // This allows DC to generate quads at boundaries
                    chunk.min = chunk.coreMin;
                    chunk.max = chunk.coreMax;
                    
                    // Extend by 1 voxel at internal boundaries (not at global bbox edges)
                    if (ix > 0U)
                    {
                        chunk.min.x() -= boundaryPadding.x();
                    }
                    if (iy > 0U)
                    {
                        chunk.min.y() -= boundaryPadding.y();
                    }
                    if (iz > 0U)
                    {
                        chunk.min.z() -= boundaryPadding.z();
                    }
                    if (ix < divisor - 1U)
                    {
                        chunk.max.x() += boundaryPadding.x();
                    }
                    if (iy < divisor - 1U)
                    {
                        chunk.max.y() += boundaryPadding.y();
                    }
                    if (iz < divisor - 1U)
                    {
                        chunk.max.z() += boundaryPadding.z();
                    }
                    
                    chunks.push_back(chunk);
                }
            }
        }
        
        return chunks;
    }

    bool ManifoldDualContouringGpu::isChunkNonEmpty(ChunkInfo const & chunk) const
    {
        // For now, assume all chunks are potentially non-empty.
        // The octree construction will quickly determine if a chunk is empty
        // (no surface crossings = no output nodes), so the overhead is minimal.
        // 
        // A more sophisticated approach would be to use GPU-based SDF sampling,
        // but that requires additional infrastructure. For typical TPMS structures,
        // most chunks will contain surface anyway.
        (void)chunk;
        return true;
    }

    void ManifoldDualContouringGpu::generateMeshForChunk(
        ChunkInfo const & chunk, 
        ManifoldDualContouringMesh & chunkMesh)
    {
        chunkMesh.positions.clear();
        chunkMesh.normals.clear();
        chunkMesh.indices.clear();
        
        // Clear the member mesh before generating for this chunk
        m_mesh.positions.clear();
        m_mesh.normals.clear();
        m_mesh.indices.clear();
        
        auto primitives = m_core.getPrimitives();
        if (!primitives)
        {
            std::cerr << "  Chunk [" << chunk.indexX << "," << chunk.indexY << "," 
                      << chunk.indexZ << "]: No primitives available" << std::endl;
            return;
        }
        
        // Temporarily override cached bbox for this chunk
        Eigen::Vector3f const savedBboxMin = m_cachedBboxMin;
        Eigen::Vector3f const savedBboxMax = m_cachedBboxMax;
        Eigen::Vector3f const savedBboxSize = m_cachedBboxSize;
        
        m_cachedBboxMin = chunk.min;
        m_cachedBboxMax = chunk.max;
        m_cachedBboxSize = chunk.max - chunk.min;
        
        // Build octree for this chunk
        m_octreeDepth = static_cast<std::uint32_t>(m_config.maxDepth);
        m_gridResolution = 1U << m_octreeDepth;
        
        try
        {
            m_program->constructOctree(
                m_octreeBuffer,
                m_octreeNodeCount,
                m_cachedBboxMin,
                m_cachedBboxMax,
                static_cast<std::uint32_t>(m_config.initialDepth),
                m_config.maxDepth,
                *primitives,
                m_config.isoValue);
            
            if (m_octreeNodeCount > 0)
            {
                refreshCpuOctreeCache();
                generateVertices();
                generateIndices();
            }
        }
        catch (std::exception const & e)
        {
            std::cerr << "Error processing chunk [" << chunk.indexX << "," 
                      << chunk.indexY << "," << chunk.indexZ << "]: " << e.what() << std::endl;
        }
        
        // Copy results to chunk mesh
        chunkMesh = m_mesh;
        
        // Restore original bbox
        m_cachedBboxMin = savedBboxMin;
        m_cachedBboxMax = savedBboxMax;
        m_cachedBboxSize = savedBboxSize;
    }

    void ManifoldDualContouringGpu::mergeMeshes(
        ManifoldDualContouringMesh & target, 
        ManifoldDualContouringMesh const & source)
    {
        if (source.positions.empty())
        {
            return;
        }
        
        std::uint32_t const vertexOffset = static_cast<std::uint32_t>(target.positions.size());
        
        // Append vertices
        target.positions.insert(target.positions.end(), 
                                source.positions.begin(), 
                                source.positions.end());
        target.normals.insert(target.normals.end(), 
                              source.normals.begin(), 
                              source.normals.end());
        
        // Append indices with offset
        target.indices.reserve(target.indices.size() + source.indices.size());
        for (std::uint32_t idx : source.indices)
        {
            target.indices.push_back(idx + vertexOffset);
        }
    }

    void ManifoldDualContouringGpu::clipMeshToCore(
        ManifoldDualContouringMesh & mesh, 
        ChunkInfo const & chunk)
    {
        if (mesh.indices.empty())
        {
            return;
        }
        
        // Use centroid-based triangle ownership:
        // Keep a triangle if its centroid is within the core region.
        // This ensures each triangle is generated by exactly one chunk.
        Eigen::Vector3f const & coreMin = chunk.coreMin;
        Eigen::Vector3f const & coreMax = chunk.coreMax;
        
        auto isCentroidInCore = [&coreMin, &coreMax](
            Eigen::Vector3f const & p0,
            Eigen::Vector3f const & p1,
            Eigen::Vector3f const & p2) -> bool
        {
            Eigen::Vector3f const centroid = (p0 + p1 + p2) / 3.0F;
            return centroid.x() >= coreMin.x() && centroid.x() < coreMax.x() &&
                   centroid.y() >= coreMin.y() && centroid.y() < coreMax.y() &&
                   centroid.z() >= coreMin.z() && centroid.z() < coreMax.z();
        };
        
        // Find triangles to keep (centroid inside core)
        std::vector<std::uint32_t> newIndices;
        newIndices.reserve(mesh.indices.size());
        
        std::vector<bool> vertexUsed(mesh.positions.size(), false);
        
        for (std::size_t t = 0U; t < mesh.indices.size(); t += 3U)
        {
            std::uint32_t const i0 = mesh.indices[t + 0U];
            std::uint32_t const i1 = mesh.indices[t + 1U];
            std::uint32_t const i2 = mesh.indices[t + 2U];
            
            // Keep triangle if centroid is inside core region
            if (isCentroidInCore(mesh.positions[i0], mesh.positions[i1], mesh.positions[i2]))
            {
                newIndices.push_back(i0);
                newIndices.push_back(i1);
                newIndices.push_back(i2);
                vertexUsed[i0] = true;
                vertexUsed[i1] = true;
                vertexUsed[i2] = true;
            }
        }
        
        // Build vertex compaction map
        std::vector<std::uint32_t> compactMap(mesh.positions.size(), 0U);
        std::uint32_t newCount = 0U;
        for (std::size_t i = 0U; i < mesh.positions.size(); ++i)
        {
            if (vertexUsed[i])
            {
                compactMap[i] = newCount++;
            }
        }
        
        // Compact vertices
        std::vector<Eigen::Vector3f> newPositions(newCount);
        std::vector<Eigen::Vector3f> newNormals(newCount);
        for (std::size_t i = 0U; i < mesh.positions.size(); ++i)
        {
            if (vertexUsed[i])
            {
                newPositions[compactMap[i]] = mesh.positions[i];
                newNormals[compactMap[i]] = mesh.normals[i];
            }
        }
        
        // Remap indices
        for (auto & idx : newIndices)
        {
            idx = compactMap[idx];
        }
        
        mesh.positions = std::move(newPositions);
        mesh.normals = std::move(newNormals);
        mesh.indices = std::move(newIndices);
    }

    void ManifoldDualContouringGpu::weldBoundaryVertices(float tolerance)
    {
        if (m_mesh.positions.size() < 2U)
        {
            return;
        }
        
        float const toleranceSq = tolerance * tolerance;
        std::size_t const numVertices = m_mesh.positions.size();
        
        // Build a simple spatial hash for faster neighbor lookup
        float const cellSize = tolerance * 2.0F;
        float const invCellSize = 1.0F / cellSize;
        
        auto hashPos = [invCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
        {
            auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));
            
            // Simple hash combining
            std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
            std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
            std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
            return (hx << 42) | (hy << 21) | hz;
        };
        
        // Map from cell hash to vertex indices in that cell
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> spatialHash;
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            std::uint64_t const hash = hashPos(m_mesh.positions[i]);
            spatialHash[hash].push_back(i);
        }
        
        // Vertex remapping: vertexRemap[old] = new (canonical vertex)
        std::vector<std::uint32_t> vertexRemap(numVertices);
        std::iota(vertexRemap.begin(), vertexRemap.end(), 0U);
        
        // For each vertex, find nearby vertices and potentially merge
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] != i)
            {
                continue; // Already remapped
            }
            
            Eigen::Vector3f const & pos = m_mesh.positions[i];
            
            // Check neighboring cells
            auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));
            
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        std::uint64_t const hx = static_cast<std::uint64_t>(ix + dx) & 0x1FFFFF;
                        std::uint64_t const hy = static_cast<std::uint64_t>(iy + dy) & 0x1FFFFF;
                        std::uint64_t const hz = static_cast<std::uint64_t>(iz + dz) & 0x1FFFFF;
                        std::uint64_t const neighborHash = (hx << 42) | (hy << 21) | hz;
                        
                        auto it = spatialHash.find(neighborHash);
                        if (it == spatialHash.end())
                        {
                            continue;
                        }
                        
                        for (std::uint32_t j : it->second)
                        {
                            if (j <= i || vertexRemap[j] != j)
                            {
                                continue; // Skip self, already processed, or already remapped
                            }
                            
                            float const distSq = (m_mesh.positions[j] - pos).squaredNorm();
                            if (distSq < toleranceSq)
                            {
                                // Merge j into i
                                vertexRemap[j] = i;
                            }
                        }
                    }
                }
            }
        }
        
        // Count how many vertices survive and build compaction map
        std::vector<std::uint32_t> compactMap(numVertices, std::numeric_limits<std::uint32_t>::max());
        std::uint32_t newVertexCount = 0U;
        
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] == i)
            {
                compactMap[i] = newVertexCount++;
            }
        }
        
        // Build final remap: old index -> new compacted index
        std::vector<std::uint32_t> finalRemap(numVertices);
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            std::uint32_t canonical = vertexRemap[i];
            finalRemap[i] = compactMap[canonical];
        }
        
        // Compact vertex buffer
        std::vector<Eigen::Vector3f> newPositions(newVertexCount);
        std::vector<Eigen::Vector3f> newNormals(newVertexCount);
        
        for (std::uint32_t i = 0U; i < numVertices; ++i)
        {
            if (vertexRemap[i] == i)
            {
                std::uint32_t const newIdx = compactMap[i];
                newPositions[newIdx] = m_mesh.positions[i];
                newNormals[newIdx] = m_mesh.normals[i];
            }
        }
        
        // Remap indices
        for (auto & idx : m_mesh.indices)
        {
            idx = finalRemap[idx];
        }
        
        // Remove degenerate triangles
        std::vector<std::uint32_t> validIndices;
        validIndices.reserve(m_mesh.indices.size());
        
        for (std::size_t t = 0U; t < m_mesh.indices.size(); t += 3U)
        {
            std::uint32_t const i0 = m_mesh.indices[t + 0U];
            std::uint32_t const i1 = m_mesh.indices[t + 1U];
            std::uint32_t const i2 = m_mesh.indices[t + 2U];
            
            if (i0 != i1 && i1 != i2 && i2 != i0)
            {
                validIndices.push_back(i0);
                validIndices.push_back(i1);
                validIndices.push_back(i2);
            }
        }
        
        std::size_t const vertsBefore = m_mesh.positions.size();
        std::size_t const trisBefore = m_mesh.indices.size() / 3U;
        
        m_mesh.positions = std::move(newPositions);
        m_mesh.normals = std::move(newNormals);
        m_mesh.indices = std::move(validIndices);
        
        std::size_t const vertsAfter = m_mesh.positions.size();
        std::size_t const trisAfter = m_mesh.indices.size() / 3U;
        
        std::cout << "  Vertex welding: " << vertsBefore << " -> " << vertsAfter 
                  << " vertices, " << trisBefore << " -> " << trisAfter << " triangles" << std::endl;
    }
}


