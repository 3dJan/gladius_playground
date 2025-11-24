#include "ManifoldDualContouringGpu.h"
#include "ManifoldDualContouringProgram.h"
#include "../Primitives.h"

#include <iostream>

namespace gladius::compute
{
    namespace
    {
        struct GpuVertex
        {
            cl_float4 position;
            cl_float4 normal;
        };

        struct OctreeNode
        {
            cl_ulong mortonCode;
            cl_uint edgeMask;
            cl_uint internalMask;
            cl_uint vertexStartIndex;
            cl_uchar vertexCount;
            cl_uchar padding[3];
        };
        static_assert(sizeof(OctreeNode) == 24, "OctreeNode size mismatch");
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
        Eigen::Vector3f size = bboxMax - bboxMin;
        float rootSize = std::max({size.x(), size.y(), size.z()});
        
        std::cout << "Constructing Octree. BBox: [" << bboxMin.transpose() << "] to [" 
                  << bboxMax.transpose() << "], Root size: " << rootSize << std::endl;
        
        try {
            m_program->constructOctree(
                m_octreeBuffer,
                m_octreeNodeCount,
                bboxMin,
                bboxMax,
                rootSize,
                m_config.maxDepth,
                *primitives,
                m_config.isoValue);
            
            std::cout << "Octree construction complete. Total nodes: " << m_octreeNodeCount << std::endl;
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
        
        // Get bounding box
        auto bbox = m_core.getBoundingBox();
        if (!bbox.has_value())
        {
            std::cerr << "No bounding box available" << std::endl;
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
        Eigen::Vector3f size = bboxMax - bboxMin;
        float rootSize = std::max({size.x(), size.y(), size.z()});
        
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
                rootSize,
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
        // For now, implement a simple triangle strip from vertices
        // This is a placeholder - proper dual contouring topology will be implemented later
        m_mesh.indices.clear();
        
        if (m_mesh.positions.size() < 3U)
        {
            return;
        }
        
        // Simple approach: Create triangles from consecutive vertex triplets
        // This won't be manifold, but allows testing the pipeline
        for (std::size_t i = 0U; i + 2U < m_mesh.positions.size(); i += 3U)
        {
            m_mesh.indices.push_back(static_cast<std::uint32_t>(i));
            m_mesh.indices.push_back(static_cast<std::uint32_t>(i + 1U));
            m_mesh.indices.push_back(static_cast<std::uint32_t>(i + 2U));
        }
        
        std::cout << "Generated " << (m_mesh.indices.size() / 3U) << " triangles (simple mode)" << std::endl;
    }
}
