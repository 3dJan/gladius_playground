#include "DualContouringSamplingProgram.h"

#include "Primitives.h"
#include "exceptions.h"

#include <algorithm>

namespace gladius
{
    // Match the payload structure from HierarchicalDCProgram
    #define PAYLOAD_ARGUMENTS                                                                          \
        m_resources->getBuildArea(), primitives.primitives.getBuffer(),                                 \
          static_cast<cl_uint>(primitives.primitives.getSize()), primitives.data.getBuffer(),          \
          static_cast<cl_uint>(primitives.data.getSize()), m_resources->getRenderingSettings(),         \
          m_resources->getPrecompSdfBuffer().getBuffer(), m_resources->getParameterBuffer().getBuffer(), \
          m_resources->getCommandBuffer().getBuffer(),                                                  \
          static_cast<cl_int>(m_resources->getCommandBuffer().getData().size()),                        \
          m_resources->getPreCompSdfBBox()

    DualContouringSamplingProgram::DualContouringSamplingProgram(SharedComputeContext context,
                                                                 SharedResources const & resources)
        : ProgramBase(std::move(context), resources)
    {
        // Add dual contouring sampling specific kernel (base class already has headers and sdf.cl)
        m_sourceFiles.push_back("dual_contouring_sampling.cl");
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

        // Run kernel with full payload
        m_programFront->run("sampleCorners",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           valueBuffer,
                           count,
                           PAYLOAD_ARGUMENTS,
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
        // Kernel signature: sampleHermite(positions, values, gradients, count, PAYLOAD_ARGS, isoValue, epsilon)
        m_programFront->run("sampleHermite",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           valueBuffer,
                           gradientBuffer,
                           count,
                           PAYLOAD_ARGUMENTS,
                           isoValue,
                           gradientEpsilon);

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

    void DualContouringSamplingProgram::sampleColors(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<Eigen::Vector3f> & outColors,
      Primitives const & primitives)
    {
        ensureCompiled();

        if (positions.empty())
        {
            outColors.clear();
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

        outColors.resize(positions.size());

        auto const count = static_cast<cl_uint>(positions.size());

        // Create OpenCL buffers
        cl::Buffer positionBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  clPositions.size() * sizeof(cl_float4),
                                  clPositions.data());

        cl::Buffer colorBuffer(m_ComputeContext->GetContext(),
                              CL_MEM_WRITE_ONLY,
                              positions.size() * sizeof(cl_float4));

        // Run kernel
        m_programFront->run("sampleColors",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           colorBuffer,
                           count,
                           PAYLOAD_ARGUMENTS);

        // Read results
        std::vector<cl_float4> clColors(positions.size());

        m_ComputeContext->GetQueue().enqueueReadBuffer(colorBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       clColors.size() * sizeof(cl_float4),
                                                       clColors.data());

        // Convert back to Eigen
        for (std::size_t i = 0U; i < clColors.size(); ++i)
        {
            outColors[i] = Eigen::Vector3f{clColors[i].s[0], clColors[i].s[1], clColors[i].s[2]};
        }
    }

    void DualContouringSamplingProgram::sampleCornersVariableThickness(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<float> & outValues,
      Primitives const & primitives,
      float baseIsoValue,
      std::vector<float> const & thicknessLUT,
      int lutResolution)
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

        cl::Buffer lutBuffer(m_ComputeContext->GetContext(),
                             CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             thicknessLUT.size() * sizeof(float),
                             const_cast<float*>(thicknessLUT.data()));

        // Run kernel
        m_programFront->run("sampleCornersVariableThickness",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           valueBuffer,
                           count,
                           PAYLOAD_ARGUMENTS,
                           baseIsoValue,
                           lutBuffer,
                           lutResolution);

        // Read results
        m_ComputeContext->GetQueue().enqueueReadBuffer(valueBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outValues.size() * sizeof(cl_float),
                                                       outValues.data());
    }

    void DualContouringSamplingProgram::sampleCornersShellVolume(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<float> & outValues,
      Primitives const & primitives,
      std::vector<float> const & outerLUT,
      std::vector<float> const & innerLUT,
      int lutResolution,
      bool isInnermostLayer)
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

        cl::Buffer outerLutBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  outerLUT.size() * sizeof(float),
                                  const_cast<float*>(outerLUT.data()));

        // For innermost layer, we still need a valid buffer but it won't be read
        cl::Buffer innerLutBuffer(m_ComputeContext->GetContext(),
                                  CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  innerLUT.size() * sizeof(float),
                                  const_cast<float*>(innerLUT.data()));

        cl_int const innermostFlag = isInnermostLayer ? 1 : 0;

        // Run kernel
        m_programFront->run("sampleCornersShellVolume",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           valueBuffer,
                           count,
                           PAYLOAD_ARGUMENTS,
                           outerLutBuffer,
                           innerLutBuffer,
                           lutResolution,
                           innermostFlag);

        // Read results
        m_ComputeContext->GetQueue().enqueueReadBuffer(valueBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outValues.size() * sizeof(cl_float),
                                                       outValues.data());
    }

    void DualContouringSamplingProgram::sampleCornersWithThicknessField(
      std::vector<Eigen::Vector3f> const & positions,
      std::vector<float> & outValues,
      Primitives const & primitives,
      std::vector<float> const & outerField,
      std::vector<float> const & innerField,
      int fieldResolution,
      Eigen::Matrix4f const & worldToField,
      bool isInnermostLayer)
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

        cl::Buffer outerFieldBuffer(m_ComputeContext->GetContext(),
                                    CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    outerField.size() * sizeof(float),
                                    const_cast<float*>(outerField.data()));

        // For innermost layer, inner field is empty but we need a valid buffer
        std::vector<float> const& innerFieldData = innerField.empty() ? outerField : innerField;
        cl::Buffer innerFieldBuffer(m_ComputeContext->GetContext(),
                                    CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    innerFieldData.size() * sizeof(float),
                                    const_cast<float*>(innerFieldData.data()));

        // Convert worldToField matrix to cl_float16 (row-major)
        cl_float16 clWorldToField;
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                clWorldToField.s[row * 4 + col] = worldToField(row, col);
            }
        }

        cl_int const innermostFlag = isInnermostLayer ? 1 : 0;

        // Run kernel
        m_programFront->run("sampleCornersShellVolumeWithField",
                           cl::NullRange,
                           cl::NDRange(count),
                           positionBuffer,
                           valueBuffer,
                           count,
                           PAYLOAD_ARGUMENTS,
                           outerFieldBuffer,
                           innerFieldBuffer,
                           fieldResolution,
                           clWorldToField,
                           innermostFlag);

        // Read results
        m_ComputeContext->GetQueue().enqueueReadBuffer(valueBuffer,
                                                       CL_TRUE,
                                                       0,
                                                       outValues.size() * sizeof(cl_float),
                                                       outValues.data());
    }
}
