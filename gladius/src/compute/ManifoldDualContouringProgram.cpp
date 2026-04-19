#include "ManifoldDualContouringProgram.h"

#include "../Primitives.h"
#include "../exceptions.h"
#include "ManifoldDualContouringGpu.h"

#include <algorithm>

namespace gladius::compute
{
    // Match the payload structure from other DC programs
    #define PAYLOAD_ARGUMENTS                                                                          \
        m_resources->getBuildArea(), primitives.primitives.getBuffer(),                                 \
          static_cast<cl_uint>(primitives.primitives.getSize()), primitives.data.getBuffer(),          \
          static_cast<cl_uint>(primitives.data.getSize()), m_resources->getRenderingSettings(),         \
          m_resources->getPrecompSdfBuffer().getBuffer(), m_resources->getParameterBuffer().getBuffer(), \
          m_resources->getCommandBuffer().getBuffer(),                                                  \
          static_cast<cl_int>(m_resources->getCommandBuffer().getData().size()),                        \
          m_resources->getPreCompSdfBBox()

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
        float isoValue,
        float gradientEpsilon)
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
                           isoValue,
                           gradientEpsilon);
    }

    void ManifoldDualContouringProgram::generateVertices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & offsetBuffer,
        cl::Buffer & vertexBuffer,
        cl::Buffer & edgeComponentBuffer,
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        float isoValue,
        float gradientEpsilon)
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
                           edgeComponentBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           PAYLOAD_ARGUMENTS,
                           isoValue,
                           gradientEpsilon);
    }

    void ManifoldDualContouringProgram::countQuads(
        cl::Buffer const & octreeBuffer,
        cl::Buffer & quadCountBuffer,
        std::size_t nodeCount,
        std::uint32_t maxCoord,
        std::uint32_t disableBoundaryChecks)
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
                           static_cast<cl_uint>(maxCoord),
                           static_cast<cl_uint>(disableBoundaryChecks));
    }

    void ManifoldDualContouringProgram::generateIndices(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & vertexOffsetBuffer,
        cl::Buffer const & edgeComponentBuffer,
        cl::Buffer const & indexOffsetBuffer,
        cl::Buffer & indexBuffer,
        std::size_t nodeCount,
        std::uint32_t maxCoord,
        std::uint32_t disableBoundaryChecks)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl::NDRange global(nodeCount);

        m_programFront->run("emit_indices",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           vertexOffsetBuffer,
                           edgeComponentBuffer,
                           indexOffsetBuffer,
                           indexBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord),
                           static_cast<cl_uint>(disableBoundaryChecks));
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
        std::uint8_t depth,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        float isoValue)
    {
        if (nodeCount == 0)
        {
            return;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto & queue = m_ComputeContext->GetQueue();

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        // Iterative halo completion.
        // Rationale: the adaptive octree construction can miss some cells required for watertight
        // quad emission. We first create missing neighbor cells as halos, then recompute their
        // edge/internal masks on the GPU and promote halos that actually contain the surface.
        // Any newly promoted surface cells may require additional halos, hence the loop.

        constexpr std::size_t MAX_ITERATIONS = 8;
        std::size_t iteration = 0;
        std::size_t totalNodesAdded = 0;

        while (iteration < MAX_ITERATIONS)
        {
            std::size_t const nodeCountAtStart = nodeCount;

            // Sort the octree so binary search works.
            sortOctreeByMorton(octreeBuffer, nodeCount);

            // 1. Count halo neighbors needed per cell
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
                // Converged: all required neighbors are present.
                break;
            }

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

            std::sort(haloNodes.begin(), haloNodes.end(),
                     [](OctreeNode const & a, OctreeNode const & b)
                     { return a.mortonCode < b.mortonCode; });
            haloNodes.erase(std::unique(haloNodes.begin(), haloNodes.end(),
                                       [](OctreeNode const & a, OctreeNode const & b)
                                       { return a.mortonCode == b.mortonCode; }),
                           haloNodes.end());

            if (haloNodes.empty())
            {
                // All halos were duplicates of existing nodes.
                break;
            }

            std::vector<OctreeNode> originalNodes(nodeCount);
            queue.enqueueReadBuffer(*octreeBuffer, CL_TRUE, 0,
                                   nodeCount * sizeof(OctreeNode),
                                   originalNodes.data());

            originalNodes.insert(originalNodes.end(), haloNodes.begin(), haloNodes.end());
            std::sort(originalNodes.begin(), originalNodes.end(),
                     [](OctreeNode const & a, OctreeNode const & b)
                     { return a.mortonCode < b.mortonCode; });

            std::vector<OctreeNode> mergedNodes;
            mergedNodes.reserve(originalNodes.size());
            for (auto const & node : originalNodes)
            {
                if (mergedNodes.empty() || mergedNodes.back().mortonCode != node.mortonCode)
                {
                    mergedNodes.push_back(node);
                    continue;
                }

                OctreeNode & dst = mergedNodes.back();
                dst.edgeMask |= node.edgeMask;
                dst.internalMask |= node.internalMask;
                dst.depth = std::max(dst.depth, node.depth);

                bool const dstIsHalo = (dst.padding[0] == 1);
                bool const srcIsHalo = (node.padding[0] == 1);
                dst.padding[0] = (dstIsHalo && srcIsHalo) ? 1 : 0;
                if (dst.edgeMask != 0)
                {
                    dst.padding[0] = 0;
                }
                std::memset(&dst.padding[1], 0, sizeof(dst.padding) - 1);
            }

            std::size_t const newNodeCount = mergedNodes.size();
            octreeBuffer = std::make_unique<cl::Buffer>(
                m_ComputeContext->GetContext(),
                CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                newNodeCount * sizeof(OctreeNode),
                mergedNodes.data());
            nodeCount = newNodeCount;

            // 5. Recompute edge/internal masks for any halo nodes and promote halos that
            // actually contain the surface (kernel clears padding[0]).
            {
                cl::NDRange globalRecompute(nodeCount);
                m_programFront->run("recompute_halo_edge_masks",
                                   cl::NullRange,
                                   globalRecompute,
                                   *octreeBuffer,
                                   static_cast<int>(nodeCount),
                                   clBboxMin,
                                   clBboxMax,
                                   PAYLOAD_ARGUMENTS,
                                   isoValue);
            }

            if (nodeCount > nodeCountAtStart)
            {
                totalNodesAdded += (nodeCount - nodeCountAtStart);
            }

            ++iteration;

            // If nodeCount didn't grow, a subsequent iteration should also yield zero halos.
            if (nodeCount == nodeCountAtStart)
            {
                break;
            }
        }

        std::cout << "Halo generation complete: " << totalNodesAdded << " nodes added in " << iteration << " iterations" << std::endl;
    }

    ManifoldDualContouringProgram::DiagnosticCounters ManifoldDualContouringProgram::runQuadDiagnostics(
        cl::Buffer const & octreeBuffer,
        std::size_t nodeCount,
        std::uint32_t maxCoord)
    {
        DiagnosticCounters result{};
        
        if (nodeCount == 0)
        {
            return result;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto & queue = m_ComputeContext->GetQueue();

        // Allocate buffer for 24 diagnostic counters (2 per edge)
        constexpr std::size_t numCounters = 24;
        auto diagnosticBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_WRITE,
            numCounters * sizeof(int));

        // Zero-initialize the counters
        std::vector<int> zeros(numCounters, 0);
        queue.enqueueWriteBuffer(*diagnosticBuffer, CL_TRUE, 0, numCounters * sizeof(int), zeros.data());

        cl::NDRange global(nodeCount);
        m_programFront->run("count_quads_diagnostic",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           *diagnosticBuffer,
                           static_cast<int>(nodeCount),
                           static_cast<cl_uint>(maxCoord));

        // Read back results
        std::vector<int> counters(numCounters);
        queue.enqueueReadBuffer(*diagnosticBuffer, CL_TRUE, 0, numCounters * sizeof(int), counters.data());

        // Unpack into result struct
        for (int e = 0; e < 12; ++e)
        {
            result.edgeEmitted[static_cast<std::size_t>(e)] = counters[static_cast<std::size_t>(e * 2)];
            result.edgeSkipped[static_cast<std::size_t>(e)] = counters[static_cast<std::size_t>(e * 2 + 1)];
        }

        return result;
    }

    ManifoldDualContouringProgram::DiscontinuityCounters ManifoldDualContouringProgram::runDiscontinuityDiagnostics(
        cl::Buffer const & octreeBuffer,
        std::size_t nodeCount,
        BBox const & paddedBbox,
        Primitives const & primitives,
        float isoValue,
        float gradientEpsilon)
    {
        DiscontinuityCounters result{};
        
        if (nodeCount == 0)
        {
            return result;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto & queue = m_ComputeContext->GetQueue();

        // Allocate buffer for 8 diagnostic counters
        constexpr std::size_t numCounters = 8;
        auto diagnosticBuffer = std::make_unique<cl::Buffer>(
            m_ComputeContext->GetContext(),
            CL_MEM_READ_WRITE,
            numCounters * sizeof(int));

        // Zero-initialize the counters
        std::vector<int> zeros(numCounters, 0);
        queue.enqueueWriteBuffer(*diagnosticBuffer, CL_TRUE, 0, numCounters * sizeof(int), zeros.data());

        // Convert bbox to float3 for kernel
        auto const bboxMin = paddedBbox.getMin();
        auto const bboxMax = paddedBbox.getMax();
        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        cl::NDRange global(nodeCount);
        m_programFront->run("count_discontinuities_diagnostic",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           *diagnosticBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           PAYLOAD_ARGUMENTS,
                           isoValue,
                           gradientEpsilon);

        // Read back results
        std::vector<int> counters(numCounters);
        queue.enqueueReadBuffer(*diagnosticBuffer, CL_TRUE, 0, numCounters * sizeof(int), counters.data());

        // Unpack into result struct
        result.cells1Component = counters[0];
        result.cells2Components = counters[1];
        result.cells3Components = counters[2];
        result.cells4Components = counters[3];
        result.totalCells = counters[4];
        result.avgDiscontinuityScore = (result.totalCells > 0) 
            ? static_cast<float>(counters[5]) / (1000.0F * static_cast<float>(result.totalCells))
            : 0.0F;
        result.severeDiscontinuities = counters[6];

        return result;
    }

    void ManifoldDualContouringProgram::constructOctreeWithThicknessField(
        std::unique_ptr<cl::Buffer> & octreeBuffer,
        std::size_t & nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        std::uint32_t initialDepth,
        std::uint32_t maxDepth,
        Primitives const & primitives,
        cl::Buffer const & outerThicknessField,
        cl::Buffer const & innerThicknessField,
        int thicknessFieldResolution,
        Eigen::Matrix4f const & worldToThicknessField,
        bool isInnermostLayer)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        std::vector<OctreeNode> rootNodes(1);
        rootNodes[0].mortonCode = 0;
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

        // Pack matrix as float16 (row-major)
        cl_float16 clWorldToGrid;
        for (int i = 0; i < 16; ++i)
        {
            clWorldToGrid.s[i] = worldToThicknessField.data()[i];
        }

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

            m_programFront->run("construct_octree_level_with_thickness",
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
                               outerThicknessField,
                               innerThicknessField,
                               thicknessFieldResolution,
                               clWorldToGrid,
                               static_cast<int>(isInnermostLayer ? 1 : 0));

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

    void ManifoldDualContouringProgram::countVerticesWithThicknessField(
        cl::Buffer const & octreeBuffer,
        cl::Buffer & countBuffer,
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        cl::Buffer const & outerThicknessField,
        cl::Buffer const & innerThicknessField,
        int thicknessFieldResolution,
        Eigen::Matrix4f const & worldToThicknessField,
        bool isInnermostLayer,
        float gradientEpsilon)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        cl_float16 clWorldToGrid;
        for (int i = 0; i < 16; ++i)
        {
            clWorldToGrid.s[i] = worldToThicknessField.data()[i];
        }

        cl::NDRange global(nodeCount);

        m_programFront->run("count_vertices_with_thickness",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           countBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           PAYLOAD_ARGUMENTS,
                           outerThicknessField,
                           innerThicknessField,
                           thicknessFieldResolution,
                           clWorldToGrid,
                           static_cast<int>(isInnermostLayer ? 1 : 0),
                           gradientEpsilon);
    }

    void ManifoldDualContouringProgram::generateVerticesWithThicknessField(
        cl::Buffer const & octreeBuffer,
        cl::Buffer const & offsetBuffer,
        cl::Buffer & vertexBuffer,
        cl::Buffer & edgeComponentBuffer,
        std::size_t nodeCount,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        cl::Buffer const & outerThicknessField,
        cl::Buffer const & innerThicknessField,
        int thicknessFieldResolution,
        Eigen::Matrix4f const & worldToThicknessField,
        bool isInnermostLayer,
        float gradientEpsilon)
    {
        ensureCompiled();
        swapProgramsIfNeeded();

        cl_float3 clBboxMin = {{bboxMin.x(), bboxMin.y(), bboxMin.z()}};
        cl_float3 clBboxMax = {{bboxMax.x(), bboxMax.y(), bboxMax.z()}};

        cl_float16 clWorldToGrid;
        for (int i = 0; i < 16; ++i)
        {
            clWorldToGrid.s[i] = worldToThicknessField.data()[i];
        }

        cl::NDRange global(nodeCount);

        m_programFront->run("emit_vertices_with_thickness",
                           cl::NullRange,
                           global,
                           octreeBuffer,
                           offsetBuffer,
                           vertexBuffer,
                           edgeComponentBuffer,
                           static_cast<int>(nodeCount),
                           clBboxMin,
                           clBboxMax,
                           PAYLOAD_ARGUMENTS,
                           outerThicknessField,
                           innerThicknessField,
                           thicknessFieldResolution,
                           clWorldToGrid,
                           static_cast<int>(isInnermostLayer ? 1 : 0),
                           gradientEpsilon);
    }

    void ManifoldDualContouringProgram::addHaloNodesWithThicknessField(
        std::unique_ptr<cl::Buffer> & octreeBuffer,
        std::size_t & nodeCount,
        std::uint32_t maxCoord,
        std::uint8_t depth,
        Eigen::Vector3f const & bboxMin,
        Eigen::Vector3f const & bboxMax,
        Primitives const & primitives,
        cl::Buffer const & outerThicknessField,
        cl::Buffer const & innerThicknessField,
        int thicknessFieldResolution,
        Eigen::Matrix4f const & worldToThicknessField,
        bool isInnermostLayer)
    {
        // For now, use the standard halo node generation with isoValue=0
        // The halo nodes just need to exist for watertight quad emission;
        // their exact positions will be computed with thickness in emit_vertices_with_thickness.
        addHaloNodes(octreeBuffer, nodeCount, maxCoord, depth, bboxMin, bboxMax, primitives, 0.0F);
    }

    std::vector<float> ManifoldDualContouringProgram::evaluateSdfBatch(
        std::vector<Eigen::Vector3f> const & positions,
        Primitives const & primitives,
        float isoValue)
    {
        std::vector<float> results(positions.size(), 0.0F);
        if (positions.empty())
        {
            return results;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto const posCount = static_cast<cl_uint>(positions.size());

        // Pack positions into flat float array
        std::vector<float> posFlat(positions.size() * 3U);
        for (std::size_t i = 0U; i < positions.size(); ++i)
        {
            posFlat[i * 3U + 0U] = positions[i].x();
            posFlat[i * 3U + 1U] = positions[i].y();
            posFlat[i * 3U + 2U] = positions[i].z();
        }

        auto & ctx = m_ComputeContext->GetContext();
        cl::Buffer posBuf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          posFlat.size() * sizeof(float), posFlat.data());
        cl::Buffer sdfBuf(ctx, CL_MEM_WRITE_ONLY,
                          results.size() * sizeof(float));

        cl::NDRange global(positions.size());

        m_programFront->run("evaluate_sdf_batch",
                           cl::NullRange,
                           global,
                           posBuf,
                           sdfBuf,
                           posCount,
                           isoValue,
                           PAYLOAD_ARGUMENTS);

        m_ComputeContext->GetQueue().enqueueReadBuffer(
            sdfBuf, CL_TRUE, 0, results.size() * sizeof(float), results.data());

        return results;
    }

    void ManifoldDualContouringProgram::evaluateSdfGradientBatch(
        std::vector<Eigen::Vector3f> const & positions,
        Primitives const & primitives,
        float isoValue,
        float epsilon,
        std::vector<float> & outSdfValues,
        std::vector<Eigen::Vector3f> & outGradients)
    {
        outSdfValues.assign(positions.size(), 0.0F);
        outGradients.assign(positions.size(), Eigen::Vector3f::Zero());
        if (positions.empty())
        {
            return;
        }

        ensureCompiled();
        swapProgramsIfNeeded();

        auto const posCount = static_cast<cl_uint>(positions.size());

        // Pack positions into flat float array
        std::vector<float> posFlat(positions.size() * 3U);
        for (std::size_t i = 0U; i < positions.size(); ++i)
        {
            posFlat[i * 3U + 0U] = positions[i].x();
            posFlat[i * 3U + 1U] = positions[i].y();
            posFlat[i * 3U + 2U] = positions[i].z();
        }

        auto & ctx = m_ComputeContext->GetContext();
        cl::Buffer posBuf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          posFlat.size() * sizeof(float), posFlat.data());
        cl::Buffer sdfBuf(ctx, CL_MEM_WRITE_ONLY,
                          positions.size() * sizeof(float));
        cl::Buffer gradBuf(ctx, CL_MEM_WRITE_ONLY,
                           positions.size() * 3U * sizeof(float));

        cl::NDRange global(positions.size());

        m_programFront->run("evaluate_sdf_gradient_batch",
                           cl::NullRange,
                           global,
                           posBuf,
                           sdfBuf,
                           gradBuf,
                           posCount,
                           isoValue,
                           epsilon,
                           PAYLOAD_ARGUMENTS);

        auto & queue = m_ComputeContext->GetQueue();
        queue.enqueueReadBuffer(
            sdfBuf, CL_TRUE, 0, positions.size() * sizeof(float), outSdfValues.data());

        std::vector<float> gradFlat(positions.size() * 3U);
        queue.enqueueReadBuffer(
            gradBuf, CL_TRUE, 0, gradFlat.size() * sizeof(float), gradFlat.data());

        for (std::size_t i = 0U; i < positions.size(); ++i)
        {
            outGradients[i] = Eigen::Vector3f(gradFlat[i * 3U + 0U],
                                               gradFlat[i * 3U + 1U],
                                               gradFlat[i * 3U + 2U]);
        }
    }
}
