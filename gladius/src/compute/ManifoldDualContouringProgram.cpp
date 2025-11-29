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
        std::uint32_t initialDepth,
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
        rootNodes[0].depth = 0;
        std::memset(rootNodes[0].padding, 0, sizeof(rootNodes[0].padding));

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
                               depth,
                               maxDepth,
                               initialDepth,
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
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        float isoValue)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        cl::NDRange global(nodeCount);

        m_programFront->run("count_vertices",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           countBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           PAYLOAD_ARGUMENTS,
                           isoValue);
    }

    void ManifoldDualContouringProgram::generateVertices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & offsetBuffer,
        cl::Buffer & vertexBuffer,
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
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
                           PAYLOAD_ARGUMENTS,
                           isoValue);
    }

    void ManifoldDualContouringProgram::countQuads(
        cl::Buffer const & octreeBuffer,
        cl::Buffer & quadCountBuffer,
        std::size_t nodeCount,
        std::uint32_t maxCoord)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl::NDRange global(nodeCount);

        m_programFront->run("count_quads",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           quadCountBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord));
    }

    void ManifoldDualContouringProgram::generateIndices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & vertexOffsetBuffer,
        cl::Buffer const & indexOffsetBuffer,
        cl::Buffer & indexBuffer,
        std::size_t nodeCount,
        std::uint32_t maxCoord)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl::NDRange global(nodeCount);

        m_programFront->run("emit_indices",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           vertexOffsetBuffer,
                           indexOffsetBuffer,
                           indexBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord));
    }

    void ManifoldDualContouringProgram::sortOctreeByMorton(
        std::unique_ptr<cl::Buffer> & octreeBuffer,
        std::size_t nodeCount)
    {
        if (nodeCount <= 1)
        {
            return;
        }

        // Read octree to CPU, sort by Morton code, write back
        // For production, consider GPU-based radix sort
        std::vector<OctreeNode> nodes(nodeCount);
        m_ComputeContext->GetQueue().enqueueReadBuffer(
            *octreeBuffer, CL_TRUE, 0, nodeCount * sizeof(OctreeNode), nodes.data());

        std::sort(nodes.begin(), nodes.end(),
                  [](OctreeNode const & a, OctreeNode const & b)
                  { return a.mortonCode < b.mortonCode; });

        m_ComputeContext->GetQueue().enqueueWriteBuffer(
            *octreeBuffer, CL_TRUE, 0, nodeCount * sizeof(OctreeNode), nodes.data());
    }

    void ManifoldDualContouringProgram::addHaloNodes(
        std::unique_ptr<cl::Buffer> & octreeBuffer,
        std::size_t & nodeCount,
        std::uint32_t maxCoord,
        std::uint8_t depth)
    {
        if (nodeCount == 0)
        {
            return;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto & queue = m_ComputeContext->GetQueue();

        // First, sort the octree so binary search works
        sortOctreeByMorton(octreeBuffer, nodeCount);

        // 1. Count halo neighbors needed per surface cell
        auto haloCountBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_WRITE,
            nodeCount * sizeof(int));

        cl::NDRange global(nodeCount);
        m_programFront->run("count_halo_neighbors",
                           cl::NullRange,
                           global,
                           *octreeBuffer,
                           *haloCountBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord),
                           static_cast<cl_uchar>(depth));

        // 2. CPU-side prefix sum for halo offsets
        std::vector<int> haloCounts(nodeCount);
        queue.enqueueReadBuffer(*haloCountBuffer, CL_TRUE, 0, nodeCount * sizeof(int), haloCounts.data());

        std::vector<int> haloOffsets(nodeCount);
        int totalHaloNodes = 0;
        for (std::size_t i = 0; i < nodeCount; ++i)
        {
            haloOffsets[i] = totalHaloNodes;
            totalHaloNodes += haloCounts[i];
        }

        if (totalHaloNodes == 0)
        {
            // No halo nodes needed - mesh should already be watertight
            return;
        }

        std::cout << "Adding " << totalHaloNodes << " halo nodes for watertight mesh" << std::endl;

        auto haloOffsetBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
            nodeCount * sizeof(int),
            haloOffsets.data());

        // 3. Emit halo nodes
        auto haloNodesBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_WRITE_ONLY,
            static_cast<std::size_t>(totalHaloNodes) * sizeof(OctreeNode));

        m_programFront->run("emit_halo_neighbors",
                           cl::NullRange,
                           global,
                           *octreeBuffer,
                           *haloOffsetBuffer,
                           *haloNodesBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord),
                           static_cast<cl_uchar>(depth));

        // 4. Read halo nodes, deduplicate, merge with original octree
        std::vector<OctreeNode> haloNodes(static_cast<std::size_t>(totalHaloNodes));
        queue.enqueueReadBuffer(*haloNodesBuffer, CL_TRUE, 0,
                               static_cast<std::size_t>(totalHaloNodes) * sizeof(OctreeNode),
                               haloNodes.data());

        // Deduplicate halo nodes, MERGING edgeMasks for nodes at the same position
        // Multiple surface cells may contribute edges to the same halo node
        std::sort(haloNodes.begin(), haloNodes.end(),
                 [](OctreeNode const & a, OctreeNode const & b)
                 { return a.mortonCode < b.mortonCode; });

        // Custom deduplication that merges edgeMask and internalMask
        if (!haloNodes.empty())
        {
            auto writeIt = haloNodes.begin();
            for (auto readIt = haloNodes.begin() + 1; readIt != haloNodes.end(); ++readIt)
            {
                if (readIt->mortonCode == writeIt->mortonCode)
                {
                    // Merge edge masks (OR them together)
                    writeIt->edgeMask |= readIt->edgeMask;
                    writeIt->internalMask |= readIt->internalMask;
                }
                else
                {
                    // Move to next unique position
                    ++writeIt;
                    *writeIt = *readIt;
                }
            }
            haloNodes.erase(writeIt + 1, haloNodes.end());
        }

        if (haloNodes.empty())
        {
            return;
        }

        std::cout << "After deduplication: " << haloNodes.size() << " unique halo nodes" << std::endl;

        // 5. Read original nodes
        std::vector<OctreeNode> originalNodes(nodeCount);
        queue.enqueueReadBuffer(*octreeBuffer, CL_TRUE, 0,
                               nodeCount * sizeof(OctreeNode),
                               originalNodes.data());

        // 6. Merge and sort
        originalNodes.insert(originalNodes.end(), haloNodes.begin(), haloNodes.end());
        std::sort(originalNodes.begin(), originalNodes.end(),
                 [](OctreeNode const & a, OctreeNode const & b)
                 { return a.mortonCode < b.mortonCode; });

        // 7. Write back to new buffer
        std::size_t const newNodeCount = originalNodes.size();
        octreeBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            newNodeCount * sizeof(OctreeNode),
            originalNodes.data());

        nodeCount = newNodeCount;
    }
}
