#include "DualContouringSamplingProgram.h"

#include "Primitives.h"
#include "exceptions.h"

#include <algorithm>

namespace gladius
{
    DualContouringSamplingProgram::DualContouringSamplingProgram(SharedComputeContext context,
                                                                 SharedResources const & resources)
        : ProgramBase(std::move(context), resources)
    {
        m_sourceFiles = {"kernel/sdf.cl", "kernel/dual_contouring_sampling.cl"};
    }

    void DualContouringSamplingProgram::ensureCompiled()
    {
        if (!isValid())
        {
            recompileBlocking();
            waitForCompilation();
        }

        if (!isValid())
        {
            throw std::runtime_error("Dual contouring sampling program failed to compile");
        }
    }

    void DualContouringSamplingProgram::sampleCorners(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<float> & outValues,
      Primitives const & primitives,
      float isoValue)
    {
        ensureCompiled();

        if (positions.empty())
        {
            outValues.clear();
            return;
        }

        swapProgramsIfNeeded();

        // Prepare input buffer (convert to cl_float4)
        std::vector<cl_float4> clPositions;
        clPositions.reserve(positions.size());
        for (auto const & pos : positions)
        {
            clPositions.push_back({pos.x(), pos.y(), pos.z(), 0.0F});
        }

        outValues.resize(positions.size());

        auto const count = static_cast<cl_uint>(positions.size());

        // Create OpenCL buffers
        cl::Buffer positionBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  clPositions.size() * sizeof(cl_float4),
                                  clPositions.data());

        cl::Buffer valueBuffer(m_ComputeContext->GetContext(),
                              CL_MEM_WRITE_ONLY,
                              outValues.size() * sizeof(cl_float));

        // Get primitive buffers
        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        // Run kernel
        m_programFront->run("sampleCorners",
                           cl::NDRange(count),
                           cl::NullRange,
                           positionBuffer,
                           valueBuffer,
                           count,
                           primitivesBuffer.getBuffer(),
                           primitivesCount,
                           dataBuffer.getBuffer(),
                           dataCount,
                           isoValue);

        // Read results
        m_ComputeContext->GetQueue().enqueueReadBuffer(valueBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outValues.size() * sizeof(cl_float),
                                                       outValues.data());
    }

    void DualContouringSamplingProgram::sampleHermite(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<float> & outValues,
      std::vector<Eigen::Vector3f> & outGradients,
      Primitives const & primitives,
      float isoValue,
      float gradientEpsilon)
    {
        ensureCompiled();

        if (positions.empty())
        {
            outValues.clear();
            outGradients.clear();
            return;
        }

        swapProgramsIfNeeded();

        // Prepare input buffer
        std::vector<cl_float4> clPositions;
        clPositions.reserve(positions.size());
        for (auto const & pos : positions)
        {
            clPositions.push_back({pos.x(), pos.y(), pos.z(), 0.0F});
        }

        outValues.resize(positions.size());
        outGradients.resize(positions.size());

        auto const count = static_cast<cl_uint>(positions.size());

        // Create OpenCL buffers
        cl::Buffer positionBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  clPositions.size() * sizeof(cl_float4),
                                  clPositions.data());

        cl::Buffer valueBuffer(m_ComputeContext->GetContext(),
                              CL_MEM_WRITE_ONLY,
                              outValues.size() * sizeof(cl_float));

        cl::Buffer gradientBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_WRITE_ONLY,
                                  positions.size() * sizeof(cl_float4));

        // Get primitive buffers
        auto const & primitivesBuffer = primitives.primitives;
        auto const & dataBuffer = primitives.data;

        auto const primitivesCount = static_cast<cl_uint>(primitivesBuffer.getSize());
        auto const dataCount = static_cast<cl_uint>(dataBuffer.getSize());

        // Run kernel
        m_programFront->run("sampleHermite",
                           cl::NDRange(count),
                           cl::NullRange,
                           positionBuffer,
                           valueBuffer,
                           gradientBuffer,
                           count,
                           gradientEpsilon,
                           primitivesBuffer.getBuffer(),
                           primitivesCount,
                           dataBuffer.getBuffer(),
                           dataCount,
                           isoValue);

        // Read results
        std::vector<cl_float4> clGradients(positions.size());

        m_ComputeContext->GetQueue().enqueueReadBuffer(valueBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outValues.size() * sizeof(cl_float),
                                                       outValues.data());

        m_ComputeContext->GetQueue().enqueueReadBuffer(gradientBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       clGradients.size() * sizeof(cl_float4),
                                                       clGradients.data());

        // Convert back to Eigen
        for (std::size_t i = 0U; i < clGradients.size(); ++i)
        {
            outGradients[i] = Eigen::Vector3f{clGradients[i].s[0], clGradients[i].s[1], clGradients[i].s[2]};
        }
    }
}
