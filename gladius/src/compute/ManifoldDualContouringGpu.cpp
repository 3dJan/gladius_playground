#include "ManifoldDualContouringGpu.h"
#include "ManifoldDualContouringProgram.h"
#include "../Primitives.h"
#include "../SlicerProgram.h"
#include "../Buffer.h"
#include "../ResourceContext.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
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

        constructOctree();
        generateVertices();
        generateIndices();
        
        // Post-processing for sharp features
        if (m_config.enableSharpFeaturePostProcess && !m_mesh.indices.empty())
        {
            postProcessSharpFeatures();
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
}

