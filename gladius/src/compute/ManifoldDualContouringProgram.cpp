#include "ManifoldDualContouringProgram.h"

#include "../Primitives.h"
#include "../exceptions.h"
#include "ManifoldDualContouringGpu.h"

#include <algorithm>

namespace gladius::compute
{
    // Match the payload structure from other DC programs
    #define PAYLOAD_ARGUMENTS                                                                          \
        m_resoures->getBuildArea(), primitives.primitives.getBuffer(),                                 \
          static_cast<cl_uint>(primitives.primitives.getSize()), primitives.data.getBuffer(),          \
          static_cast<cl_uint>(primitives.data.getSize()), m_resoures->getRenderingSettings(),         \
          m_resoures->getPrecompSdfBuffer().getBuffer(), m_resoures->getParameterBuffer().getBuffer(), \
          m_resoures->getCommandBuffer().getBuffer(),                                                  \
          static_cast<cl_int>(m_resoures->getCommandBuffer().getData().size()),                        \
          m_resoures->getPreCompSdfBBox()

    ManifoldDualContouringProgram::ManifoldDualContouringProgram(SharedComputeContext context,
                                                                 SharedResources const & resources)
        : ProgramBase(std::move(context), resources)
    {
        m_sourceFiles.push_back("manifold_dual_contouring.cl");
    }

    void ManifoldDualContouringProgram::ensureCompiled()
    {
        if (!isValid())
        {
            recompileBlocking();
            waitForCompilation();
        }

        if (!isValid())
        {
            throw std::runtime_error("Manifold dual contouring program failed to compile");
        }
    }

    void ManifoldDualContouringProgram::constructOctree(
        std::unique_ptr<cl::Buffer> & octreeBuffer,
        std::size_t & nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        float rootSize,
        std::uint32_t maxDepth,
        Primitives const & primitives,
        float isoValue)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        // Start with root node
        std::vector<OctreeNode> rootNodes(1);
        rootNodes[0].mortonCode = 0; // Root node at depth 0, position (0,0,0)
        rootNodes[0].edgeMask = 0xFFF;
        rootNodes[0].internalMask = 0;
        rootNodes[0].vertexStartIndex = 0;
        rootNodes[0].vertexCount = 0;

        auto currentBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            rootNodes.size() * sizeof(OctreeNode),
            rootNodes.data());

        std::size_t currentNodeCount = 1;

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        // Iteratively subdivide
        for (std::uint32_t depth = 0; depth < maxDepth; ++depth)
        {
            std::size_t maxChildren = currentNodeCount * 8;

            auto nextBuffer = std::make_unique<cl::Buffer>(
                m_ComputeContext->GetContext(),
                CL_MEM_READ_WRITE,
                maxChildren * sizeof(OctreeNode));

            auto countBuffer = std::make_unique<cl::Buffer>(
                m_ComputeContext->GetContext(),
                CL_MEM_READ_WRITE,
                sizeof(int));

            int zero = 0;
            m_ComputeContext->GetQueue().enqueueWriteBuffer(*countBuffer, CL_TRUE, 0, sizeof(int), &zero);

            cl::NDRange global(currentNodeCount);

            m_programFront->run("construct_octree_level",
                               cl::NullRange,
                               global,
                               *currentBuffer,
                               *nextBuffer,
                               *countBuffer,
                               static_cast<int>(currentNodeCount),
                               clBboxMin,
                               clBboxMax,
                               rootSize,
                               depth,
                               maxDepth,
                               PAYLOAD_ARGUMENTS,
                               isoValue);

            // Read back count
            m_ComputeContext->GetQueue().enqueueReadBuffer(*countBuffer, CL_TRUE, 0, sizeof(int), &currentNodeCount);

            if (currentNodeCount == 0)
            {
                break;
            }

            currentBuffer = std::move(nextBuffer);
        }

        octreeBuffer = std::move(currentBuffer);
        nodeCount = currentNodeCount;
    }

    void ManifoldDualContouringProgram::countVertices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer & countBuffer,
        std::size_t nodeCount)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl::NDRange global(nodeCount);

        m_programFront->run("count_vertices",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           countBuffer,
                           static_cast<int>(nodeCount));
    }

    void ManifoldDualContouringProgram::generateVertices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & offsetBuffer,
        cl::Buffer & vertexBuffer,
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        float rootSize,
        Primitives const & primitives,
        float isoValue)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        cl::NDRange global(nodeCount);

        m_programFront->run("emit_vertices",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           offsetBuffer,
                           vertexBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           rootSize,
                           PAYLOAD_ARGUMENTS,
                           isoValue);
    }
}
