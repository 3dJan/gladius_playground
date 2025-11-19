#include "ManifoldDualContouringGpu.h"
#include <iostream>

namespace gladius::compute
{
    ManifoldDualContouringGpu::ManifoldDualContouringGpu(ComputeCore& core)
        : m_core(core)
    {
        loadKernels();
    }

    void ManifoldDualContouringGpu::loadKernels()
    {
        auto context = m_core.getComputeContext();
        m_program = std::make_unique<CLProgram>(context);
        
        // Assuming the file is in the resource path or current directory
        // The build system should ensure this file is available to the runtime
        std::vector<std::string> sources = {"kernel/manifold_dual_contouring.cl"};
        
        BuildCallBack callback = []() {
            // Compilation finished
        };
        
        try {
            m_program->loadAndCompileSource(sources, "", callback);
            m_program->finishCompilation();
        } catch (std::exception& e) {
            std::cerr << "Failed to compile Manifold Dual Contouring kernels: " << e.what() << std::endl;
        }
    }

    void ManifoldDualContouringGpu::generateMesh()
    {
        if (!m_program || !m_program->isValid()) {
            std::cerr << "Program not valid, cannot generate mesh" << std::endl;
            return;
        }

        constructOctree();
        generateVertices();
        generateIndices();
    }

    void ManifoldDualContouringGpu::constructOctree()
    {
        // Placeholder: Create dummy octree data
        // In a real implementation, this would be a compute kernel that builds the octree
        // from the SDF.
        
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

        auto context = m_core.getComputeContext();
        size_t numNodes = 1000; // Example size
        size_t nodeSize = sizeof(OctreeNode); 
        
        m_octreeBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * nodeSize);
        
        // Initialize with zeros or test data if needed
        // cl::enqueueFillBuffer(context->GetQueue(), *m_octreeBuffer, 0, 0, numNodes * nodeSize);
    }

    void ManifoldDualContouringGpu::generateVertices()
    {
        auto context = m_core.getComputeContext();
        auto& queue = context->GetQueue();
        
        size_t numNodes = 1000; // Should match octree size
        
        // 1. Count vertices
        m_countBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, numNodes * sizeof(int));
        
        cl::NDRange global(numNodes);
        // Use a safe local size, or query device for max work group size
        cl::NDRange local(64); 
        
        try {
            m_program->run(queue, "count_vertices", cl::NullRange, global, local, 
                *m_octreeBuffer, *m_countBuffer, (int)numNodes);
        } catch (std::exception& e) {
            std::cerr << "Error running count_vertices: " << e.what() << std::endl;
            return;
        }
            
        // 2. Scan (Prefix Sum)
        // For this initial implementation, we perform the scan on the CPU.
        // This involves reading back the counts, computing offsets, and writing them back.
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
        
        if (totalVertices == 0) {
            return;
        }
        
        m_offsetBuffer = context->createBufferChecked(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, 
            numNodes * sizeof(int), offsets.data());
            
        // 3. Emit Vertices
        // Vertex struct is float4 position + float4 normal = 32 bytes
        m_vertexBuffer = context->createBufferChecked(CL_MEM_READ_WRITE, totalVertices * 32); 
        
        try {
            m_program->run(queue, "emit_vertices", cl::NullRange, global, local,
                *m_octreeBuffer, *m_offsetBuffer, *m_vertexBuffer, (int)numNodes);
        } catch (std::exception& e) {
            std::cerr << "Error running emit_vertices: " << e.what() << std::endl;
        }
    }

    void ManifoldDualContouringGpu::generateIndices()
    {
        // Placeholder for index generation
        // This would follow a similar pattern: count indices per cell/edge, scan, emit.
    }
}
