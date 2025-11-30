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
        Eigen::Vector3f originalBboxMin = Eigen::Vector3f(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f originalBboxMax = Eigen::Vector3f(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        
        // Add margin to bounding box to ensure surface at boundaries is properly captured.
        // The margin should be at least 2 voxels at the finest level to allow proper
        // sign change detection at the surface boundary.
        Eigen::Vector3f const originalSize = originalBboxMax - originalBboxMin;
        float const maxExtent = originalSize.maxCoeff();
        float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
        float const margin = 2.0f * voxelSize;
        
        m_cachedBboxMin = originalBboxMin - Eigen::Vector3f(margin, margin, margin);
        m_cachedBboxMax = originalBboxMax + Eigen::Vector3f(margin, margin, margin);
        m_cachedBboxSize = m_cachedBboxMax - m_cachedBboxMin;

        // Use hierarchical octree approach if enabled (produces watertight meshes)
        if (m_config.enableHierarchicalOctree)
        {
            generateMeshHierarchical();
        }
        else
        {
            // Fallback to chunked or single-pass approach
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
            
            // Enable chunked mode - disables maxCoord boundary check in generateIndices()
            m_isChunkedMode = true;
            
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
                
                // Clip triangles to core region to avoid duplicate geometry in overlap areas
                // Each triangle is kept only if its centroid is within the chunk's core region
                clipMeshToCore(chunkMesh, chunk);
                
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
                Eigen::Vector3f const chunkSize = m_cachedBboxSize / static_cast<float>(chunkDivisor);
                float const voxelSize = chunkSize.maxCoeff() / static_cast<float>(1U << m_config.maxDepth);
                
                if (weldTolerance <= 0.0F)
                {
                    // Chunk size / 2^maxDepth = voxel size within chunk
                    // Use 0.5 * voxel size as tolerance - conservative to avoid
                    // welding vertices that shouldn't be merged
                    weldTolerance = voxelSize * 0.5F;
                    std::cout << "  Auto weld tolerance: " << weldTolerance << " (voxel size: " << voxelSize << ")" << std::endl;
                }
                weldBoundaryVertices(weldTolerance);
                
                // Fill gaps between unconnected boundary edges
                // Use voxel size as search radius - edges from neighboring chunks
                // should be within this distance
                fillBoundaryGaps(voxelSize * 1.5F);
            }
            
            // Reset chunked mode flag
            m_isChunkedMode = false;
            }
            else
            {
                // Original single-pass processing (not chunked)
                m_isChunkedMode = false;
                constructOctree();
                generateVertices();
                generateIndices();
                
                // Fill gaps between disconnected boundary edges.
                // This helps close holes caused by missing neighbor cells in the octree.
                // The function skips edges on the bbox boundary to avoid creating
                // incorrect geometry at domain boundaries.
                if (!m_mesh.indices.empty())
                {
                    fillBoundaryGaps(voxelSize * 1.5F);
                }
            }
        } // End of else block for non-hierarchical processing
        
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
        
        // Add margin to bounding box to ensure surface at boundaries is properly captured.
        // The margin should be at least 2 voxels at the finest level to allow proper
        // sign change detection at the surface boundary.
        Eigen::Vector3f const originalSize = bboxMax - bboxMin;
        float const maxExtent = originalSize.maxCoeff();
        std::uint32_t const depth = m_config.maxDepth;
        float const voxelSize = maxExtent / static_cast<float>(1U << depth);
        float const margin = 2.0f * voxelSize;
        
        bboxMin -= Eigen::Vector3f(margin, margin, margin);
        bboxMax += Eigen::Vector3f(margin, margin, margin);
        
        m_cachedBboxMin = bboxMin;
        m_cachedBboxMax = bboxMax;
        m_cachedBboxSize = bboxMax - bboxMin;
        m_octreeDepth = depth;
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
        
        std::cout << "Constructing Octree. Original BBox: [" << (bboxMin + Eigen::Vector3f(margin, margin, margin)).transpose() 
              << "] to [" << (bboxMax - Eigen::Vector3f(margin, margin, margin)).transpose() 
              << "], Padded BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose()
              << "], Margin: " << margin << " (voxelSize=" << voxelSize << ")"
              << ", Extents: " << m_cachedBboxSize.transpose()
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
            
            std::cout << "Octree construction complete. Nodes before halo: " << m_octreeNodeCount << std::endl;
            
            // Add halo nodes around surface-crossing cells to ensure all neighbors exist for quad generation.
            // This fixes holes in thin structures where boundary cells lack neighbors.
            std::uint32_t const maxCoord = m_gridResolution - 1;
            m_program->addHaloNodes(m_octreeBuffer, m_octreeNodeCount, maxCoord, m_config.maxDepth);
            
            std::cout << "Octree construction complete. Total nodes after halo: " << m_octreeNodeCount << std::endl;
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
        
        // Calculate voxel size and gradient epsilon based on octree depth
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;
        float const maxExtent = bboxSize.maxCoeff();
        float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
        // Use 10% of voxel size for gradient computation - balances detail vs noise
        float const gradientEpsilon = voxelSize * 0.1f;
        
        // 1. Count vertices (1-4 per cell based on discontinuity detection)
        m_countBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
        
        try {
            m_program->countVertices(*m_octreeBuffer, *m_countBuffer, numNodes,
                                    bboxMin, bboxMax, *primitives, m_config.isoValue, gradientEpsilon);
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
                m_config.isoValue,
                gradientEpsilon);
                
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
            
            // In chunked mode, disable boundary checks since chunk boundaries
            // are internal and we want to emit quads there
            std::uint32_t const disableBoundaryChecks = m_isChunkedMode ? 1U : 0U;

            // 1. Count quads per cell
            auto quadCountBuffer =
                context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
            m_program->countQuads(*m_octreeBuffer, *quadCountBuffer, numNodes, maxCoord, disableBoundaryChecks);

            // 2. CPU-side prefix sum for index offsets
            std::vector<int> quadCounts(numNodes);
            queue.enqueueReadBuffer(*quadCountBuffer, CL_TRUE, 0, numNodes * sizeof(int), quadCounts.data());

            // Debug: Count total quads and cells with quads
            int totalQuads = 0;
            int cellsWithQuads = 0;
            for (std::size_t i = 0U; i < numNodes; ++i)
            {
                totalQuads += quadCounts[i];
                if (quadCounts[i] > 0) cellsWithQuads++;
            }
            std::cout << "Quad counting: " << totalQuads << " quads from " << cellsWithQuads 
                      << " cells (of " << numNodes << " total nodes)" << std::endl;

            // Run diagnostic analysis to understand boundary hole causes
            auto diagnostics = m_program->runQuadDiagnostics(*m_octreeBuffer, numNodes, maxCoord);
            std::cout << "\n=== Boundary Hole Diagnostics (All 12 Edges) ===" << std::endl;
            
            // Edge axis names for better readability
            static char const * edgeNames[] = {
                "Edge  0 (X at y=0,z=0)",  // corners 0-1
                "Edge  1 (Y at x=1,z=0)",  // corners 1-3
                "Edge  2 (X at y=1,z=0)",  // corners 2-3
                "Edge  3 (Y at x=0,z=0)",  // corners 0-2
                "Edge  4 (X at y=0,z=1)",  // corners 4-5
                "Edge  5 (Y at x=1,z=1)",  // corners 5-7, owner
                "Edge  6 (X at y=1,z=1)",  // corners 6-7, owner
                "Edge  7 (Y at x=0,z=1)",  // corners 4-6
                "Edge  8 (Z at x=0,y=0)",  // corners 0-4
                "Edge  9 (Z at x=1,y=0)",  // corners 1-5
                "Edge 10 (Z at x=1,y=1)",  // corners 3-7, owner
                "Edge 11 (Z at x=0,y=1)"   // corners 2-6
            };
            
            int totalEmitted = 0;
            int totalSkipped = 0;
            for (int e = 0; e < 12; ++e)
            {
                int emitted = diagnostics.edgeEmitted[static_cast<std::size_t>(e)];
                int skipped = diagnostics.edgeSkipped[static_cast<std::size_t>(e)];
                if (emitted > 0 || skipped > 0)
                {
                    std::cout << edgeNames[e] << ": emitted=" << emitted << ", skipped=" << skipped << std::endl;
                }
                totalEmitted += emitted;
                totalSkipped += skipped;
            }
            std::cout << "Summary: " << totalEmitted << " edges emitted, " << totalSkipped << " edges skipped" << std::endl;
            std::cout << "=================================================\n" << std::endl;

            // Run discontinuity diagnostic to detect CSG-related gradient issues
            BBox paddedBbox;
            paddedBbox.extend(m_cachedBboxMin);
            paddedBbox.extend(m_cachedBboxMax);
            Eigen::Vector3f const bboxSize = m_cachedBboxMax - m_cachedBboxMin;
            float const maxExtent = bboxSize.maxCoeff();
            float const voxelSize = maxExtent / static_cast<float>(1U << m_config.maxDepth);
            float const gradientEpsilon = voxelSize * 0.1F;
            
            // Get primitives for discontinuity diagnostic
            auto primitives = m_core.getPrimitives();
            if (primitives)
            {
                auto discDiag = m_program->runDiscontinuityDiagnostics(
                    *m_octreeBuffer, numNodes, paddedBbox, *primitives, m_config.isoValue, gradientEpsilon);
                
                if (discDiag.cells2Components > 0 || discDiag.cells3Components > 0 || discDiag.cells4Components > 0)
                {
                    std::cout << "\n=== Gradient Discontinuity Analysis ===" << std::endl;
                    std::cout << "Cells analyzed: " << discDiag.totalCells << std::endl;
                    std::cout << "  1 component (smooth): " << discDiag.cells1Component 
                              << " (" << (100.0F * static_cast<float>(discDiag.cells1Component) / 
                                         static_cast<float>(discDiag.totalCells)) << "%)" << std::endl;
                    std::cout << "  2 components: " << discDiag.cells2Components 
                              << " (" << (100.0F * static_cast<float>(discDiag.cells2Components) / 
                                         static_cast<float>(discDiag.totalCells)) << "%)" << std::endl;
                    if (discDiag.cells3Components > 0)
                    {
                        std::cout << "  3 components: " << discDiag.cells3Components << std::endl;
                    }
                    if (discDiag.cells4Components > 0)
                    {
                        std::cout << "  4 components: " << discDiag.cells4Components << std::endl;
                    }
                    std::cout << "Average discontinuity score: " << discDiag.avgDiscontinuityScore << std::endl;
                    std::cout << "Severe discontinuities (>0.5): " << discDiag.severeDiscontinuities << std::endl;
                    std::cout << "========================================\n" << std::endl;
                }
            }

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
                *m_indexBuffer, numNodes, maxCoord, disableBoundaryChecks);

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
        
        // For DC, we need 2 extra cells at each internal boundary:
        // - Cell at boundary edge needs neighbor at +1 offset for quad generation
        // - So the cell at boundary (at +1 padding) needs its neighbor (at +2 padding)
        // The boundary cells will be duplicated in adjacent chunks, but clipping removes duplicates
        Eigen::Vector3f const boundaryPadding = globalVoxelSize * 2.0F;
        
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

    void ManifoldDualContouringGpu::fillBoundaryGaps(float searchRadius)
    {
        if (m_mesh.indices.size() < 3U)
        {
            return;
        }
        
        // Build edge-to-triangle map to find boundary edges
        // An edge is a boundary edge if it's used by only 1 triangle
        // IMPORTANT: We store DIRECTED edges to preserve winding order
        struct DirectedEdge
        {
            std::uint32_t v0;  // Start vertex (as in original triangle winding)
            std::uint32_t v1;  // End vertex (as in original triangle winding)
        };
        
        // For counting, we use undirected edge representation
        struct UndirectedEdgeHash
        {
            std::size_t operator()(DirectedEdge const & e) const
            {
                std::uint32_t minV = std::min(e.v0, e.v1);
                std::uint32_t maxV = std::max(e.v0, e.v1);
                return std::hash<std::uint64_t>{}(
                    (static_cast<std::uint64_t>(minV) << 32) | maxV);
            }
        };
        
        struct UndirectedEdgeEqual
        {
            bool operator()(DirectedEdge const & a, DirectedEdge const & b) const
            {
                std::uint32_t minA = std::min(a.v0, a.v1);
                std::uint32_t maxA = std::max(a.v0, a.v1);
                std::uint32_t minB = std::min(b.v0, b.v1);
                std::uint32_t maxB = std::max(b.v0, b.v1);
                return minA == minB && maxA == maxB;
            }
        };
        
        // Store edge usage count and the DIRECTED edge from the original triangle
        // Also store the third vertex of the triangle for normal computation
        struct EdgeInfo
        {
            std::size_t count = 0U;
            DirectedEdge directedEdge;  // Direction from the first triangle using this edge
            std::uint32_t thirdVertex;  // The third vertex of the triangle (for normal)
        };
        
        std::unordered_map<DirectedEdge, EdgeInfo, UndirectedEdgeHash, UndirectedEdgeEqual> edgeInfo;
        
        for (std::size_t t = 0U; t < m_mesh.indices.size(); t += 3U)
        {
            std::uint32_t const i0 = m_mesh.indices[t + 0U];
            std::uint32_t const i1 = m_mesh.indices[t + 1U];
            std::uint32_t const i2 = m_mesh.indices[t + 2U];
            
            // Store directed edges as they appear in the triangle (preserves winding)
            // Also store the opposite vertex for each edge (for normal computation)
            std::tuple<DirectedEdge, std::uint32_t> const edges[3] = {
                {{i0, i1}, i2},
                {{i1, i2}, i0},
                {{i2, i0}, i1}
            };
            for (auto const & [e, opposite] : edges)
            {
                auto & info = edgeInfo[e];
                if (info.count == 0U)
                {
                    info.directedEdge = e;  // Store the direction from first occurrence
                    info.thirdVertex = opposite;
                }
                info.count++;
            }
        }
        
        // Get bounding box for filtering - edges on bbox faces should not be filled
        // as they are legitimate open boundaries (surface clipped at bbox)
        Eigen::Vector3f bboxMin(std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max());
        Eigen::Vector3f bboxMax(std::numeric_limits<float>::lowest(),
                                std::numeric_limits<float>::lowest(),
                                std::numeric_limits<float>::lowest());
        for (auto const & pos : m_mesh.positions)
        {
            bboxMin = bboxMin.cwiseMin(pos);
            bboxMax = bboxMax.cwiseMax(pos);
        }
        float const bboxTolerance = searchRadius * 0.5F;  // Tolerance for bbox face detection
        
        // Helper to check if a point is on a bounding box face
        auto isOnBboxFace = [&bboxMin, &bboxMax, bboxTolerance](Eigen::Vector3f const & p) -> bool
        {
            return p.x() <= bboxMin.x() + bboxTolerance ||
                   p.x() >= bboxMax.x() - bboxTolerance ||
                   p.y() <= bboxMin.y() + bboxTolerance ||
                   p.y() >= bboxMax.y() - bboxTolerance ||
                   p.z() <= bboxMin.z() + bboxTolerance ||
                   p.z() >= bboxMax.z() - bboxTolerance;
        };
        
        // Collect boundary edges (used by only 1 triangle) with their original direction
        // Separate internal edges from bbox boundary edges for different handling
        struct BoundaryEdge
        {
            DirectedEdge edge;
            std::uint32_t thirdVertex;  // Third vertex of original triangle (for normal)
        };
        std::vector<BoundaryEdge> boundaryEdges;      // Internal boundary edges
        std::vector<BoundaryEdge> bboxBoundaryEdges;  // Edges on bbox faces (need capping)
        
        for (auto const & [edge, info] : edgeInfo)
        {
            if (info.count == 1U)
            {
                // Check if both vertices are on bbox faces
                Eigen::Vector3f const & p0 = m_mesh.positions[info.directedEdge.v0];
                Eigen::Vector3f const & p1 = m_mesh.positions[info.directedEdge.v1];
                
                if (isOnBboxFace(p0) && isOnBboxFace(p1))
                {
                    // Collect bbox boundary edges for capping (CSG intersection boundaries)
                    bboxBoundaryEdges.push_back({info.directedEdge, info.thirdVertex});
                }
                else
                {
                    boundaryEdges.push_back({info.directedEdge, info.thirdVertex});
                }
            }
        }
        
        if (boundaryEdges.empty() && bboxBoundaryEdges.empty())
        {
            std::cout << "  Gap filling: No boundary edges found - mesh is closed" << std::endl;
            return;
        }
        
        std::cout << "  Gap filling: Found " << boundaryEdges.size() << " internal boundary edges"
                  << ", " << bboxBoundaryEdges.size() << " on bbox boundary (will cap)" << std::endl;
        
        // Build spatial hash for edge midpoints
        float const searchRadiusSq = searchRadius * searchRadius;
        float const cellSize = searchRadius * 2.0F;
        float const invCellSize = 1.0F / cellSize;
        
        auto hashPos = [invCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
        {
            auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * invCellSize));
            std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
            std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
            std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
            return (hx << 42) | (hy << 21) | hz;
        };
        
        // Store edge midpoints with their indices
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> edgeSpatialHash;
        std::vector<Eigen::Vector3f> edgeMidpoints(boundaryEdges.size());
        
        for (std::size_t i = 0U; i < boundaryEdges.size(); ++i)
        {
            Eigen::Vector3f const & p0 = m_mesh.positions[boundaryEdges[i].edge.v0];
            Eigen::Vector3f const & p1 = m_mesh.positions[boundaryEdges[i].edge.v1];
            edgeMidpoints[i] = (p0 + p1) * 0.5F;
            edgeSpatialHash[hashPos(edgeMidpoints[i])].push_back(i);
        }
        
        // Find pairs of nearby boundary edges that can be stitched
        std::vector<bool> edgeUsed(boundaryEdges.size(), false);
        std::vector<std::uint32_t> newTriangles;
        
        for (std::size_t i = 0U; i < boundaryEdges.size(); ++i)
        {
            if (edgeUsed[i])
            {
                continue;
            }
            
            Eigen::Vector3f const & midI = edgeMidpoints[i];
            BoundaryEdge const & boundaryI = boundaryEdges[i];
            DirectedEdge const & edgeI = boundaryI.edge;
            Eigen::Vector3f const & pI0 = m_mesh.positions[edgeI.v0];
            Eigen::Vector3f const & pI1 = m_mesh.positions[edgeI.v1];
            float const edgeLenI = (pI1 - pI0).norm();
            
            // Compute normal of original triangle containing edge I
            Eigen::Vector3f const & pI2 = m_mesh.positions[boundaryI.thirdVertex];
            Eigen::Vector3f const normalI = (pI1 - pI0).cross(pI2 - pI0).normalized();
            
            // Search neighboring cells
            auto const ix = static_cast<std::int32_t>(std::floor(midI.x() * invCellSize));
            auto const iy = static_cast<std::int32_t>(std::floor(midI.y() * invCellSize));
            auto const iz = static_cast<std::int32_t>(std::floor(midI.z() * invCellSize));
            
            std::size_t bestMatch = std::numeric_limits<std::size_t>::max();
            float bestDistSq = searchRadiusSq;
            
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
                        
                        auto it = edgeSpatialHash.find(neighborHash);
                        if (it == edgeSpatialHash.end())
                        {
                            continue;
                        }
                        
                        for (std::size_t j : it->second)
                        {
                            if (j == i || edgeUsed[j])
                            {
                                continue;
                            }
                            
                            // Check if edges have similar length
                            BoundaryEdge const & boundaryJ = boundaryEdges[j];
                            DirectedEdge const & edgeJ = boundaryJ.edge;
                            Eigen::Vector3f const & pJ0 = m_mesh.positions[edgeJ.v0];
                            Eigen::Vector3f const & pJ1 = m_mesh.positions[edgeJ.v1];
                            float const edgeLenJ = (pJ1 - pJ0).norm();
                            
                            float const lenRatio = std::min(edgeLenI, edgeLenJ) / 
                                                   std::max(edgeLenI, edgeLenJ);
                            if (lenRatio < 0.5F)
                            {
                                continue;  // Edge lengths too different
                            }
                            
                            // Check if edges come from triangles with similar normals
                            // This avoids connecting edges from opposite sides of a thin sheet
                            Eigen::Vector3f const & pJ2 = m_mesh.positions[boundaryJ.thirdVertex];
                            Eigen::Vector3f const normalJ = (pJ1 - pJ0).cross(pJ2 - pJ0).normalized();
                            float const normalDot = normalI.dot(normalJ);
                            if (normalDot < 0.7F)  // Normals should point nearly the same direction
                            {
                                continue;  // Edges from opposite sides of surface
                            }
                            
                            float const distSq = (edgeMidpoints[j] - midI).squaredNorm();
                            if (distSq < bestDistSq)
                            {
                                bestDistSq = distSq;
                                bestMatch = j;
                            }
                        }
                    }
                }
            }
            
            if (bestMatch != std::numeric_limits<std::size_t>::max())
            {
                // Create bridge triangles between edges i and bestMatch
                BoundaryEdge const & boundaryJ = boundaryEdges[bestMatch];
                DirectedEdge const & edgeJ = boundaryJ.edge;
                
                // We have normal of original triangle (normalI) computed earlier.
                // Use distance to determine which vertices to pair.
                float const d00 = (m_mesh.positions[edgeI.v0] - m_mesh.positions[edgeJ.v0]).squaredNorm();
                float const d01 = (m_mesh.positions[edgeI.v0] - m_mesh.positions[edgeJ.v1]).squaredNorm();
                
                // Determine vertex pairing based on distance
                std::uint32_t const jNear0 = (d00 < d01) ? edgeJ.v0 : edgeJ.v1;  // Near I.v0
                std::uint32_t const jNear1 = (d00 < d01) ? edgeJ.v1 : edgeJ.v0;  // Near I.v1
                
                // Create bridge triangles with correct winding based on original normal
                // The quad is: I.v0 -- I.v1 -- jNear1 -- jNear0 (spatial order)
                // We need to determine if this should be CCW or CW based on normalI
                
                // Helper to add triangle with winding check
                auto addTriangleWithCorrectWinding = [this, &newTriangles, &normalI](
                    std::uint32_t a, std::uint32_t b, std::uint32_t c) -> bool
                {
                    // Skip if any two vertices are the same
                    if (a == b || b == c || c == a)
                    {
                        return false;
                    }
                    
                    Eigen::Vector3f const & pA = m_mesh.positions[a];
                    Eigen::Vector3f const & pB = m_mesh.positions[b];
                    Eigen::Vector3f const & pC = m_mesh.positions[c];
                    
                    // Check triangle area - skip if degenerate
                    Eigen::Vector3f const triNormal = (pB - pA).cross(pC - pA);
                    float const areaSq = triNormal.squaredNorm();
                    float constexpr minAreaSq = 1e-12F;
                    if (areaSq < minAreaSq)
                    {
                        return false;
                    }
                    
                    // Check if triangle normal aligns with original normal
                    // If not, reverse the winding
                    if (triNormal.dot(normalI) < 0.0F)
                    {
                        // Reverse winding: swap b and c
                        newTriangles.push_back(a);
                        newTriangles.push_back(c);
                        newTriangles.push_back(b);
                    }
                    else
                    {
                        newTriangles.push_back(a);
                        newTriangles.push_back(b);
                        newTriangles.push_back(c);
                    }
                    return true;
                };
                
                // Create the two bridge triangles for the quad
                // Quad: I.v0 -- I.v1 -- jNear1 -- jNear0
                addTriangleWithCorrectWinding(edgeI.v0, edgeI.v1, jNear1);
                addTriangleWithCorrectWinding(edgeI.v0, jNear1, jNear0);
                
                edgeUsed[i] = true;
                edgeUsed[bestMatch] = true;
            }
        }
        
        if (!newTriangles.empty())
        {
            std::size_t const bridgeTriCount = newTriangles.size() / 3U;
            m_mesh.indices.insert(m_mesh.indices.end(), 
                                  newTriangles.begin(), 
                                  newTriangles.end());
            
            std::size_t remainingBoundary = 0U;
            for (bool used : edgeUsed)
            {
                if (!used)
                {
                    ++remainingBoundary;
                }
            }
            
            std::cout << "  Gap filling: Created " << bridgeTriCount 
                      << " bridge triangles, " << remainingBoundary 
                      << " boundary edges remain" << std::endl;
        }
        else if (!boundaryEdges.empty())
        {
            std::cout << "  Gap filling: No matching edge pairs found" << std::endl;
        }
        
        // ========================================================================
        // Phase 2: Cap bbox boundary edges (CSG intersection boundaries)
        // These edges form closed loops on each bbox face that need triangulation
        // ========================================================================
        if (!bboxBoundaryEdges.empty())
        {
            std::vector<std::uint32_t> capTriangles;
            
            // Build adjacency map for bbox boundary edges: vertex -> list of edges starting there
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> vertexToEdges;
            for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
            {
                vertexToEdges[bboxBoundaryEdges[i].edge.v0].push_back(i);
            }
            
            // Track which edges have been used in loops
            std::vector<bool> bboxEdgeUsed(bboxBoundaryEdges.size(), false);
            std::size_t loopCount = 0U;
            std::size_t cappedEdges = 0U;
            
            // Find and triangulate closed loops
            for (std::size_t startIdx = 0U; startIdx < bboxBoundaryEdges.size(); ++startIdx)
            {
                if (bboxEdgeUsed[startIdx])
                {
                    continue;
                }
                
                // Try to build a closed loop starting from this edge
                // Don't mark edges as used until we confirm the loop is closed
                std::vector<std::size_t> loopEdgeIndices;  // Edge indices in the loop
                std::vector<std::uint32_t> loopVertices;   // Vertex sequence
                std::set<std::size_t> visitedEdges;        // Edges in current attempt
                
                std::size_t currentIdx = startIdx;
                std::uint32_t startVertex = bboxBoundaryEdges[startIdx].edge.v0;
                
                bool foundLoop = false;
                std::size_t const maxLoopSize = 1000U;  // Prevent infinite loops
                
                while (loopEdgeIndices.size() < maxLoopSize)
                {
                    if (bboxEdgeUsed[currentIdx] || visitedEdges.count(currentIdx) > 0)
                    {
                        break;  // Already used or visited in this attempt
                    }
                    
                    BoundaryEdge const & edge = bboxBoundaryEdges[currentIdx];
                    loopEdgeIndices.push_back(currentIdx);
                    loopVertices.push_back(edge.edge.v0);
                    visitedEdges.insert(currentIdx);
                    
                    std::uint32_t nextVertex = edge.edge.v1;
                    
                    // Check if we closed the loop
                    if (nextVertex == startVertex && loopVertices.size() >= 3U)
                    {
                        foundLoop = true;
                        break;
                    }
                    
                    // Find next edge in the chain
                    auto it = vertexToEdges.find(nextVertex);
                    if (it == vertexToEdges.end() || it->second.empty())
                    {
                        break;  // Dead end
                    }
                    
                    // Find an unused edge from this vertex
                    bool foundNext = false;
                    for (std::size_t nextIdx : it->second)
                    {
                        if (!bboxEdgeUsed[nextIdx] && visitedEdges.count(nextIdx) == 0)
                        {
                            currentIdx = nextIdx;
                            foundNext = true;
                            break;
                        }
                    }
                    
                    if (!foundNext)
                    {
                        break;  // No more edges
                    }
                }
                
                // If we found a closed loop with at least 3 vertices, triangulate it
                if (foundLoop && loopVertices.size() >= 3U)
                {
                    // Mark all edges in this loop as used
                    for (std::size_t edgeIdx : loopEdgeIndices)
                    {
                        bboxEdgeUsed[edgeIdx] = true;
                    }
                    
                    ++loopCount;
                    cappedEdges += loopVertices.size();
                    
                    // Get the average normal from original triangles (for winding)
                    Eigen::Vector3f avgNormal = Eigen::Vector3f::Zero();
                    for (std::size_t i = 0U; i < loopVertices.size(); ++i)
                    {
                        std::uint32_t v0 = loopVertices[i];
                        std::uint32_t v1 = loopVertices[(i + 1U) % loopVertices.size()];
                        
                        // Find the original edge to get its third vertex
                        for (auto const & be : bboxBoundaryEdges)
                        {
                            if (be.edge.v0 == v0 && be.edge.v1 == v1)
                            {
                                Eigen::Vector3f const & p0 = m_mesh.positions[be.edge.v0];
                                Eigen::Vector3f const & p1 = m_mesh.positions[be.edge.v1];
                                Eigen::Vector3f const & p2 = m_mesh.positions[be.thirdVertex];
                                avgNormal += (p1 - p0).cross(p2 - p0).normalized();
                                break;
                            }
                        }
                    }
                    if (avgNormal.squaredNorm() > 0.0F)
                    {
                        avgNormal.normalize();
                    }
                    
                    // Simple fan triangulation from first vertex
                    // Works well for convex-ish loops, may need ear-clipping for complex ones
                    std::uint32_t const anchor = loopVertices[0];
                    for (std::size_t i = 1U; i + 1U < loopVertices.size(); ++i)
                    {
                        std::uint32_t v1 = loopVertices[i];
                        std::uint32_t v2 = loopVertices[i + 1U];
                        
                        // Skip degenerate triangles
                        if (anchor == v1 || v1 == v2 || v2 == anchor)
                        {
                            continue;
                        }
                        
                        // Check winding and emit triangle
                        Eigen::Vector3f const & p0 = m_mesh.positions[anchor];
                        Eigen::Vector3f const & p1 = m_mesh.positions[v1];
                        Eigen::Vector3f const & p2 = m_mesh.positions[v2];
                        
                        Eigen::Vector3f triNormal = (p1 - p0).cross(p2 - p0);
                        if (triNormal.squaredNorm() < 1e-12F)
                        {
                            continue;  // Degenerate
                        }
                        
                        // Emit with correct winding based on average normal
                        if (triNormal.dot(avgNormal) >= 0.0F)
                        {
                            capTriangles.push_back(anchor);
                            capTriangles.push_back(v1);
                            capTriangles.push_back(v2);
                        }
                        else
                        {
                            capTriangles.push_back(anchor);
                            capTriangles.push_back(v2);
                            capTriangles.push_back(v1);
                        }
                    }
                }
            }
            
            // Add cap triangles to mesh
            if (!capTriangles.empty())
            {
                std::size_t const capTriCount = capTriangles.size() / 3U;
                m_mesh.indices.insert(m_mesh.indices.end(),
                                      capTriangles.begin(),
                                      capTriangles.end());
                
                std::size_t uncappedEdges = 0U;
                for (bool used : bboxEdgeUsed)
                {
                    if (!used)
                    {
                        ++uncappedEdges;
                    }
                }
                
                std::cout << "  Bbox capping: Created " << capTriCount
                          << " cap triangles from " << loopCount << " loops"
                          << " (capped " << cappedEdges << " edges"
                          << ", " << uncappedEdges << " remain)" << std::endl;
            }
            else
            {
                std::cout << "  Bbox capping: No closed loops found in " 
                          << bboxBoundaryEdges.size() << " bbox boundary edges" << std::endl;
            }
            
            // ----------------------------------------------------------------
            // Phase 2b: Stitch remaining uncapped bbox boundary edges
            // Use spatial hashing to find nearby edges and create bridge triangles
            // ----------------------------------------------------------------
            std::size_t uncappedCount = 0U;
            for (bool used : bboxEdgeUsed)
            {
                if (!used)
                {
                    ++uncappedCount;
                }
            }
            
            if (uncappedCount > 0U)
            {
                // Build spatial hash for uncapped bbox edge midpoints
                float const bboxStitchRadius = searchRadius * 2.0F;  // Slightly larger radius
                float const bboxStitchRadiusSq = bboxStitchRadius * bboxStitchRadius;
                float const bboxCellSize = bboxStitchRadius * 2.0F;
                float const bboxInvCellSize = 1.0F / bboxCellSize;
                
                std::unordered_map<std::uint64_t, std::vector<std::size_t>> bboxEdgeSpatialHash;
                std::vector<Eigen::Vector3f> bboxEdgeMidpoints(bboxBoundaryEdges.size());
                
                auto bboxHashPos = [bboxInvCellSize](Eigen::Vector3f const & pos) -> std::uint64_t
                {
                    auto const ix = static_cast<std::int32_t>(std::floor(pos.x() * bboxInvCellSize));
                    auto const iy = static_cast<std::int32_t>(std::floor(pos.y() * bboxInvCellSize));
                    auto const iz = static_cast<std::int32_t>(std::floor(pos.z() * bboxInvCellSize));
                    std::uint64_t const hx = static_cast<std::uint64_t>(ix) & 0x1FFFFF;
                    std::uint64_t const hy = static_cast<std::uint64_t>(iy) & 0x1FFFFF;
                    std::uint64_t const hz = static_cast<std::uint64_t>(iz) & 0x1FFFFF;
                    return (hx << 42) | (hy << 21) | hz;
                };
                
                for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
                {
                    if (bboxEdgeUsed[i])
                    {
                        continue;
                    }
                    Eigen::Vector3f const & p0 = m_mesh.positions[bboxBoundaryEdges[i].edge.v0];
                    Eigen::Vector3f const & p1 = m_mesh.positions[bboxBoundaryEdges[i].edge.v1];
                    bboxEdgeMidpoints[i] = (p0 + p1) * 0.5F;
                    bboxEdgeSpatialHash[bboxHashPos(bboxEdgeMidpoints[i])].push_back(i);
                }
                
                std::vector<std::uint32_t> stitchTriangles;
                
                for (std::size_t i = 0U; i < bboxBoundaryEdges.size(); ++i)
                {
                    if (bboxEdgeUsed[i])
                    {
                        continue;
                    }
                    
                    Eigen::Vector3f const & midI = bboxEdgeMidpoints[i];
                    BoundaryEdge const & boundaryI = bboxBoundaryEdges[i];
                    DirectedEdge const & edgeI = boundaryI.edge;
                    Eigen::Vector3f const & pI0 = m_mesh.positions[edgeI.v0];
                    Eigen::Vector3f const & pI1 = m_mesh.positions[edgeI.v1];
                    float const edgeLenI = (pI1 - pI0).norm();
                    
                    // Get normal from original triangle
                    Eigen::Vector3f const & pI2 = m_mesh.positions[boundaryI.thirdVertex];
                    Eigen::Vector3f const normalI = (pI1 - pI0).cross(pI2 - pI0).normalized();
                    
                    // Search neighboring cells
                    auto const ix = static_cast<std::int32_t>(std::floor(midI.x() * bboxInvCellSize));
                    auto const iy = static_cast<std::int32_t>(std::floor(midI.y() * bboxInvCellSize));
                    auto const iz = static_cast<std::int32_t>(std::floor(midI.z() * bboxInvCellSize));
                    
                    std::size_t bestMatch = std::numeric_limits<std::size_t>::max();
                    float bestDistSq = bboxStitchRadiusSq;
                    
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
                                
                                auto it = bboxEdgeSpatialHash.find(neighborHash);
                                if (it == bboxEdgeSpatialHash.end())
                                {
                                    continue;
                                }
                                
                                for (std::size_t j : it->second)
                                {
                                    if (j == i || bboxEdgeUsed[j])
                                    {
                                        continue;
                                    }
                                    
                                    // Check edge length similarity
                                    BoundaryEdge const & boundaryJ = bboxBoundaryEdges[j];
                                    DirectedEdge const & edgeJ = boundaryJ.edge;
                                    Eigen::Vector3f const & pJ0 = m_mesh.positions[edgeJ.v0];
                                    Eigen::Vector3f const & pJ1 = m_mesh.positions[edgeJ.v1];
                                    float const edgeLenJ = (pJ1 - pJ0).norm();
                                    
                                    float const lenRatio = std::min(edgeLenI, edgeLenJ) / 
                                                           std::max(edgeLenI, edgeLenJ);
                                    if (lenRatio < 0.3F)
                                    {
                                        continue;  // Edge lengths too different
                                    }
                                    
                                    // Check normal compatibility
                                    Eigen::Vector3f const & pJ2 = m_mesh.positions[boundaryJ.thirdVertex];
                                    Eigen::Vector3f const normalJ = (pJ1 - pJ0).cross(pJ2 - pJ0).normalized();
                                    float const normalDot = normalI.dot(normalJ);
                                    if (normalDot < 0.5F)  // More lenient for bbox edges
                                    {
                                        continue;
                                    }
                                    
                                    float const distSq = (bboxEdgeMidpoints[j] - midI).squaredNorm();
                                    if (distSq < bestDistSq)
                                    {
                                        bestDistSq = distSq;
                                        bestMatch = j;
                                    }
                                }
                            }
                        }
                    }
                    
                    if (bestMatch != std::numeric_limits<std::size_t>::max())
                    {
                        BoundaryEdge const & boundaryJ = bboxBoundaryEdges[bestMatch];
                        DirectedEdge const & edgeJ = boundaryJ.edge;
                        
                        // Determine vertex pairing
                        float const d00 = (pI0 - m_mesh.positions[edgeJ.v0]).squaredNorm();
                        float const d01 = (pI0 - m_mesh.positions[edgeJ.v1]).squaredNorm();
                        
                        std::uint32_t const jNear0 = (d00 < d01) ? edgeJ.v0 : edgeJ.v1;
                        std::uint32_t const jNear1 = (d00 < d01) ? edgeJ.v1 : edgeJ.v0;
                        
                        // Create bridge triangles with winding check
                        auto addTriWithWinding = [this, &stitchTriangles, &normalI](
                            std::uint32_t a, std::uint32_t b, std::uint32_t c) -> bool
                        {
                            if (a == b || b == c || c == a)
                            {
                                return false;
                            }
                            
                            Eigen::Vector3f const & pA = m_mesh.positions[a];
                            Eigen::Vector3f const & pB = m_mesh.positions[b];
                            Eigen::Vector3f const & pC = m_mesh.positions[c];
                            
                            Eigen::Vector3f const triNormal = (pB - pA).cross(pC - pA);
                            if (triNormal.squaredNorm() < 1e-12F)
                            {
                                return false;
                            }
                            
                            if (triNormal.dot(normalI) < 0.0F)
                            {
                                stitchTriangles.push_back(a);
                                stitchTriangles.push_back(c);
                                stitchTriangles.push_back(b);
                            }
                            else
                            {
                                stitchTriangles.push_back(a);
                                stitchTriangles.push_back(b);
                                stitchTriangles.push_back(c);
                            }
                            return true;
                        };
                        
                        addTriWithWinding(edgeI.v0, edgeI.v1, jNear1);
                        addTriWithWinding(edgeI.v0, jNear1, jNear0);
                        
                        bboxEdgeUsed[i] = true;
                        bboxEdgeUsed[bestMatch] = true;
                    }
                }
                
                if (!stitchTriangles.empty())
                {
                    std::size_t const stitchTriCount = stitchTriangles.size() / 3U;
                    m_mesh.indices.insert(m_mesh.indices.end(),
                                          stitchTriangles.begin(),
                                          stitchTriangles.end());
                    
                    std::size_t finalUncapped = 0U;
                    for (bool used : bboxEdgeUsed)
                    {
                        if (!used)
                        {
                            ++finalUncapped;
                        }
                    }
                    
                    std::cout << "  Bbox stitching: Created " << stitchTriCount
                              << " stitch triangles, " << finalUncapped 
                              << " bbox edges remain uncapped" << std::endl;
                }
            }
        }
    }

    void ManifoldDualContouringGpu::generateMeshHierarchical()
    {
        std::cout << "Using hierarchical octree approach for watertight mesh generation" << std::endl;

        // Ensure SDF is precomputed for the bounding box
        if (m_cachedBoundingBox.has_value())
        {
            m_core.precomputeSdfForBBox(*m_cachedBoundingBox);
        }

        // Create or reuse the hierarchical octree
        if (!m_hierarchicalOctree)
        {
            m_hierarchicalOctree = std::make_unique<GlobalMortonOctree>(m_core);
        }

        // Configure the octree
        GlobalMortonOctreeConfig octreeConfig;
        octreeConfig.initialDepth = m_config.initialDepth;
        octreeConfig.maxDepth = m_config.maxDepth;
        octreeConfig.isoValue = m_config.isoValue;
        octreeConfig.minFeatureSize = m_config.minFeatureSize;
        octreeConfig.enableAdaptiveRefinement = m_config.enableAdaptiveRefinement;
        octreeConfig.curvatureThreshold = m_config.curvatureThreshold;
        octreeConfig.refinementPasses = m_config.refinementPasses;

        // Build the octree
        m_hierarchicalOctree->build(octreeConfig);

        // Extract mesh
        m_hierarchicalOctree->extractMesh(m_mesh.positions, m_mesh.normals, m_mesh.indices);

        // Report statistics
        auto const& stats = m_hierarchicalOctree->getStats();
        std::cout << "Hierarchical mesh generation complete:" << std::endl;
        std::cout << "  Vertices: " << m_mesh.positions.size() << std::endl;
        std::cout << "  Triangles: " << m_mesh.indices.size() / 3U << std::endl;
        std::cout << "  Boundary edges: " << stats.boundaryEdges << std::endl;
        std::cout << "  Non-manifold edges: " << stats.nonManifoldEdges << std::endl;
        
        if (stats.boundaryEdges > 0U)
        {
            std::cout << "  WARNING: Mesh is not watertight!" << std::endl;
        }
        if (stats.nonManifoldEdges > 0U)
        {
            std::cout << "  WARNING: Mesh has non-manifold edges!" << std::endl;
        }
    }
}
