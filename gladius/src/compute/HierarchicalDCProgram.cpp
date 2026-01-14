#include "compute/HierarchicalDCProgram.h"

#include "BBox.h"
#include "Primitives.h"
#include "exceptions.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace gladius
{

#define PAYLOAD_ARGUMENTS                                                                          \
    m_resources->getBuildArea(), primitives.primitives.getBuffer(),                                 \
      static_cast<cl_uint>(primitives.primitives.getSize()), primitives.data.getBuffer(),          \
      static_cast<cl_uint>(primitives.data.getSize()), m_resources->getRenderingSettings(),         \
      m_resources->getPrecompSdfBuffer().getBuffer(), m_resources->getParameterBuffer().getBuffer(), \
      m_resources->getCommandBuffer().getBuffer(),                                                  \
      static_cast<cl_int>(m_resources->getCommandBuffer().getData().size()),                        \
      m_resources->getPreCompSdfBBox()
    HierarchicalDCProgram::HierarchicalDCProgram(SharedComputeContext context,
                                                 SharedResources const & resources)
        : ProgramBase(std::move(context), resources)
    {
        // Add hierarchical DC specific kernel (base class already has headers and sdf.cl)
        m_sourceFiles.push_back("hierarchical_dc.cl");
    }

    void HierarchicalDCProgram::ensureCompiled()
    {
        if (!isValid())
        {
            recompileBlocking();
            waitForCompilation();
        }

        if (!isValid())
        {
            throw std::runtime_error("Hierarchical dual contouring program failed to compile");
        }
    }

    void HierarchicalDCProgram::evaluateOctreeLevel(
      std::vector<Eigen::Vector3f> const & nodeBoundsMin,
      std::vector<Eigen::Vector3f> const & nodeBoundsMax,
      std::vector<float> & outCornerValues,
      Primitives const & primitives,
      float isoValue)
    {
        ensureCompiled();

        if (nodeBoundsMin.empty() || nodeBoundsMax.empty())
        {
            outCornerValues.clear();
            return;
        }

        if (nodeBoundsMin.size() != nodeBoundsMax.size())
        {
            throw std::runtime_error("Node bounds arrays must have equal size");
        }

        swapProgramsIfNeeded();

        auto const nodeCount = static_cast<cl_uint>(nodeBoundsMin.size());
        outCornerValues.resize(nodeCount * 8U);

        std::vector<cl_float> flatBoundsMin;
        std::vector<cl_float> flatBoundsMax;
        flatBoundsMin.reserve(nodeCount * 3U);
        flatBoundsMax.reserve(nodeCount * 3U);

        for (std::size_t i = 0U; i < nodeCount; ++i)
        {
            flatBoundsMin.push_back(nodeBoundsMin[i].x());
            flatBoundsMin.push_back(nodeBoundsMin[i].y());
            flatBoundsMin.push_back(nodeBoundsMin[i].z());

            flatBoundsMax.push_back(nodeBoundsMax[i].x());
            flatBoundsMax.push_back(nodeBoundsMax[i].y());
            flatBoundsMax.push_back(nodeBoundsMax[i].z());
        }

        cl::Buffer minBoundsBuffer(m_ComputeContext->GetContext(),
                                   CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                   flatBoundsMin.size() * sizeof(cl_float),
                                   flatBoundsMin.data());

        cl::Buffer maxBoundsBuffer(m_ComputeContext->GetContext(),
                                   CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                   flatBoundsMax.size() * sizeof(cl_float),
                                   flatBoundsMax.data());

        cl::Buffer cornerValuesBuffer(m_ComputeContext->GetContext(),
                                      CL_MEM_WRITE_ONLY,
                                      outCornerValues.size() * sizeof(cl_float));

        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        m_programFront->run("evaluateOctreeLevel",
                            cl::NullRange,
                            cl::NDRange(nodeCount),
                            minBoundsBuffer,
                            maxBoundsBuffer,
                            cornerValuesBuffer,
                            nodeCount,
                            PAYLOAD_ARGUMENTS,
                            isoValue);

        m_ComputeContext->GetQueue().enqueueReadBuffer(cornerValuesBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outCornerValues.size() * sizeof(cl_float),
                                                       outCornerValues.data());
    }

    void HierarchicalDCProgram::detectIntersections(
      std::vector<float> const & cornerValues,
      std::vector<std::uint8_t> & outSubdivisionFlags)
    {
        ensureCompiled();

        if (cornerValues.empty())
        {
            outSubdivisionFlags.clear();
            return;
        }

        if (cornerValues.size() % 8U != 0U)
        {
            throw std::runtime_error("Corner values size must be multiple of 8");
        }

        swapProgramsIfNeeded();

        auto const nodeCount = static_cast<cl_uint>(cornerValues.size() / 8U);
        outSubdivisionFlags.resize(nodeCount);

        cl::Buffer cornerValuesBuffer(m_ComputeContext->GetContext(),
                                      CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      cornerValues.size() * sizeof(cl_float),
                                      const_cast<void*>(static_cast<void const*>(cornerValues.data())));

        cl::Buffer subdivisionFlagsBuffer(m_ComputeContext->GetContext(),
                                          CL_MEM_WRITE_ONLY,
                                          outSubdivisionFlags.size() * sizeof(cl_uchar));

        m_programFront->run("detectIntersections",
                            cl::NullRange,
                            cl::NDRange(nodeCount),
                            cornerValuesBuffer,
                            subdivisionFlagsBuffer,
                            nodeCount);

        m_ComputeContext->GetQueue().enqueueReadBuffer(subdivisionFlagsBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outSubdivisionFlags.size() * sizeof(cl_uchar),
                                                       outSubdivisionFlags.data());
    }

    void HierarchicalDCProgram::estimateCurvature(
      std::vector<Eigen::Vector3f> const & leafCenters,
      std::vector<float> & outCurvatureMetrics,
      Primitives const & primitives,
      float gradientEpsilon)
    {
        ensureCompiled();

        if (leafCenters.empty())
        {
            outCurvatureMetrics.clear();
            return;
        }

        swapProgramsIfNeeded();

        auto const leafCount = static_cast<cl_uint>(leafCenters.size());
        outCurvatureMetrics.resize(leafCount);

        std::vector<cl_float> flatCenters;
        flatCenters.reserve(leafCount * 3U);

        for (auto const & center : leafCenters)
        {
            flatCenters.push_back(center.x());
            flatCenters.push_back(center.y());
            flatCenters.push_back(center.z());
        }

        cl::Buffer centersBuffer(m_ComputeContext->GetContext(),
                                 CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 flatCenters.size() * sizeof(cl_float),
                                 flatCenters.data());

        cl::Buffer curvatureBuffer(m_ComputeContext->GetContext(),
                                   CL_MEM_WRITE_ONLY,
                                   outCurvatureMetrics.size() * sizeof(cl_float));

        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        m_programFront->run("estimateCurvature",
                            cl::NullRange,
                            cl::NDRange(leafCount),
                            centersBuffer,
                            curvatureBuffer,
                            leafCount,
                            PAYLOAD_ARGUMENTS,
                            gradientEpsilon);

        m_ComputeContext->GetQueue().enqueueReadBuffer(curvatureBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outCurvatureMetrics.size() * sizeof(cl_float),
                                                       outCurvatureMetrics.data());
    }

    void HierarchicalDCProgram::batchGradients(std::vector<Eigen::Vector3f> const & positions,
                                               std::vector<Eigen::Vector3f> & outGradients,
                                               Primitives const & primitives,
                                               float gradientEpsilon)
    {
        ensureCompiled();

        if (positions.empty())
        {
            outGradients.clear();
            return;
        }

        swapProgramsIfNeeded();

        std::vector<cl_float4> clPositions;
        clPositions.reserve(positions.size());
        for (auto const & pos : positions)
        {
            clPositions.push_back({pos.x(), pos.y(), pos.z(), 0.0F});
        }

        outGradients.resize(positions.size());

        auto const count = static_cast<cl_uint>(positions.size());

        cl::Buffer positionBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  clPositions.size() * sizeof(cl_float4),
                                  clPositions.data());

        cl::Buffer gradientBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_WRITE_ONLY,
                                  positions.size() * sizeof(cl_float4));

        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        m_programFront->run("batchGradients",
                            cl::NullRange,
                            cl::NDRange(count),
                            positionBuffer,
                            gradientBuffer,
                            count,
                            PAYLOAD_ARGUMENTS,
                            gradientEpsilon);

        std::vector<cl_float4> clGradients(positions.size());

        m_ComputeContext->GetQueue().enqueueReadBuffer(gradientBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       clGradients.size() * sizeof(cl_float4),
                                                       clGradients.data());

        for (std::size_t i = 0U; i < clGradients.size(); ++i)
        {
            outGradients[i] =
              Eigen::Vector3f{clGradients[i].s[0], clGradients[i].s[1], clGradients[i].s[2]};
        }
    }

    void HierarchicalDCProgram::refineZeroCrossings(
      std::vector<Eigen::Vector3f> const & edgeStarts,
      std::vector<Eigen::Vector3f> const & edgeEnds,
      std::vector<float> const & startValues,
      std::vector<float> const & endValues,
      std::vector<Eigen::Vector3f> & outPositions,
      Primitives const & primitives,
      float isoValue,
      std::uint32_t maxIterations,
      float tolerance)
    {
        ensureCompiled();

        if (edgeStarts.empty())
        {
            outPositions.clear();
            return;
        }

        if (edgeStarts.size() != edgeEnds.size() ||
            edgeStarts.size() != startValues.size() ||
            edgeStarts.size() != endValues.size())
        {
            throw std::runtime_error("Edge arrays must have equal size for zero-crossing refinement");
        }

        swapProgramsIfNeeded();

        auto const count = static_cast<cl_uint>(edgeStarts.size());
        outPositions.resize(edgeStarts.size());

        std::vector<cl_float4> clStarts;
        std::vector<cl_float4> clEnds;
        clStarts.reserve(edgeStarts.size());
        clEnds.reserve(edgeEnds.size());

        for (std::size_t i = 0U; i < edgeStarts.size(); ++i)
        {
            clStarts.push_back({edgeStarts[i].x(), edgeStarts[i].y(), edgeStarts[i].z(), 0.0F});
            clEnds.push_back({edgeEnds[i].x(), edgeEnds[i].y(), edgeEnds[i].z(), 0.0F});
        }

        cl::Buffer startBuffer(m_ComputeContext->GetContext(),
                               CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                               clStarts.size() * sizeof(cl_float4),
                               clStarts.data());

        cl::Buffer endBuffer(m_ComputeContext->GetContext(),
                             CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             clEnds.size() * sizeof(cl_float4),
                             clEnds.data());

        std::vector<cl_float> clStartValues(startValues.begin(), startValues.end());
        std::vector<cl_float> clEndValues(endValues.begin(), endValues.end());

        cl::Buffer startValueBuffer(m_ComputeContext->GetContext(),
                                    CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    clStartValues.size() * sizeof(cl_float),
                                    clStartValues.data());

        cl::Buffer endValueBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  clEndValues.size() * sizeof(cl_float),
                                  clEndValues.data());

        cl::Buffer refinedBuffer(m_ComputeContext->GetContext(),
                                 CL_MEM_WRITE_ONLY,
                                 edgeStarts.size() * sizeof(cl_float4));

        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        m_programFront->run("refineZeroCrossings",
                            cl::NullRange,
                            cl::NDRange(count),
                            startBuffer,
                            endBuffer,
                            startValueBuffer,
                            endValueBuffer,
                            refinedBuffer,
                            count,
                            PAYLOAD_ARGUMENTS,
                            static_cast<cl_uint>(maxIterations),
                            tolerance,
                            isoValue);

        std::vector<cl_float4> clRefined(edgeStarts.size());
        m_ComputeContext->GetQueue().enqueueReadBuffer(refinedBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       clRefined.size() * sizeof(cl_float4),
                                                       clRefined.data());

        for (std::size_t i = 0U; i < clRefined.size(); ++i)
        {
            outPositions[i] = Eigen::Vector3f{clRefined[i].s[0], clRefined[i].s[1], clRefined[i].s[2]};
        }
    }
}
