#pragma once

#include "../ComputeContext.h"
#include "ComputeCore.h"
#include "../types.h"
#include "../CLProgram.h"

#include <vector>
#include <memory>

namespace gladius::compute
{
    class ManifoldDualContouringGpu
    {
    public:
        explicit ManifoldDualContouringGpu(ComputeCore& core);
        
        void generateMesh();
        
        cl::Buffer* getVertexBuffer() const { return m_vertexBuffer.get(); }
        cl::Buffer* getIndexBuffer() const { return m_indexBuffer.get(); }
        size_t getVertexCount() const { return m_vertexBuffer ? m_vertexBuffer->getInfo<CL_MEM_SIZE>() / 32 : 0; }
        
    private:
        ComputeCore& m_core;
        std::unique_ptr<CLProgram> m_program;
        
        // Buffers
        std::unique_ptr<cl::Buffer> m_octreeBuffer;
        std::unique_ptr<cl::Buffer> m_vertexBuffer;
        std::unique_ptr<cl::Buffer> m_indexBuffer;
        std::unique_ptr<cl::Buffer> m_countBuffer;
        std::unique_ptr<cl::Buffer> m_offsetBuffer;
        
        void loadKernels();
        void constructOctree();
        void generateVertices();
        void generateIndices();
    };
}
