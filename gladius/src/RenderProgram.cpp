#include "RenderProgram.h"
#include "Profiling.h"
#include "ProgramBase.h"
#include "gpgpu.h"
#include "kernel/types.h"

#include <CL/cl_platform.h>
#include <algorithm>

#include <cstddef>

namespace gladius
{
    /// Maximum render height per dispatch to prevent excessive GPU workload
    constexpr size_t kMaxRenderHeightPerDispatch = 16000;

    RenderProgram::RenderProgram(SharedComputeContext context, const SharedResources & resources)
        : ProgramBase(context, resources)
    {
        m_sourceFiles = {"types.h",
                         "arguments.h",
                         "sdf.h",
                         "sampler.h",
                         "rendering.h",
                         "PNanoVDB_OpenCL.h",
                         "PNanoVDB.h",
                         "PNanoVDB_OpenCL_Helpers.h",
                         "mesh_sdf.cl",
                         "sdf.cl",
                         "rendering.cl",
                         "renderer.cl"};
    }

    RenderProgram::RenderSetup RenderProgram::prepareRenderSetup(
        ImageRGBA & targetImage,
        cl_float z_mm,
        size_t startHeight,
        size_t endHeight)
    {
        RenderSetup setup{};

        if (!m_programFront->isValid())
        {
            return setup;
        }
        swapProgramsIfNeeded();

        if (startHeight >= endHeight)
        {
            return setup;
        }

        auto const start = std::clamp(startHeight, size_t(0), targetImage.getHeight() - 2);
        auto const size =
          std::clamp(endHeight - startHeight, size_t{0}, targetImage.getHeight() - start - 1);

        if (size < 1 || size > kMaxRenderHeightPerDispatch)
        {
            return setup;
        }

        if (auto * glImageBuffer = dynamic_cast<GLImageBuffer *>(&targetImage);
            glImageBuffer != nullptr)
        {
            glImageBuffer->invalidateContent();
        }

        m_resources->getRenderingSettings().time_s = m_resources->getTime_s();
        m_resources->getRenderingSettings().z_mm = z_mm;

        setup.origin = {0, start, 0};
        setup.globalRange = {targetImage.getWidth(), size, 1};
        setup.valid = true;
        return setup;
    }

    void RenderProgram::renderScene(const Primitives & lines,
                                    ImageRGBA & targetImage,
                                    cl_float z_mm,
                                    size_t startHeight,
                                    size_t endHeight)
    {
        renderScene(m_ComputeContext->GetQueue(), lines, targetImage, z_mm, startHeight, endHeight);
    }

    void RenderProgram::renderScene(cl::CommandQueue const & queue,
                                    const Primitives & lines,
                                    ImageRGBA & targetImage,
                                    cl_float z_mm,
                                    size_t startHeight,
                                    size_t endHeight)
    {
        ProfileFunction;
        try
        {
            cl::Event const event =
              renderSceneAsync(queue, lines, targetImage, z_mm, startHeight, endHeight);
            if (event())
            {
                queue.flush();
                event.wait();
                queue.finish();
            }
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->logError(std::string("RenderProgram error: ") + e.what());
            }
        }
    }

    cl::Event RenderProgram::renderSceneAsync(cl::CommandQueue const & queue,
                                              const Primitives & lines,
                                              ImageRGBA & targetImage,
                                              cl_float z_mm,
                                              size_t startHeight,
                                              size_t endHeight)
    {
        return renderSceneAsync(
          queue, lines, targetImage, m_resources->getRenderingSettings(),
          z_mm, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneAsync(cl::CommandQueue const & queue,
                                              Primitives const & lines,
                                              ImageRGBA & targetImage,
                                              RenderingSettings settings,
                                              cl_float z_mm,
                                              size_t startHeight,
                                              size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, z_mm, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        // Ensure time and z_mm are current in the caller's copy
        settings.time_s = m_resources->getTime_s();
        settings.z_mm = z_mm;

        try
        {
            kernelEvent = m_programFront->runNonBlocking(
              queue,
              "renderScene",
              setup.origin,
              setup.globalRange,
              targetImage.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              m_resources->getEyePosition(),
              m_resources->getModelViewPerspectiveMat());
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->logError(std::string("RenderProgram error: ") + e.what());
            }
        }

        return kernelEvent;
    }

    void RenderProgram::resample(ImageRGBA & sourceImage,
                                 ImageRGBA & targetImage,
                                 size_t startHeight,
                                 size_t endHeight)
    {
        ProfileFunction;
        swapProgramsIfNeeded();
        if (!m_programFront->isValid())
        {
            return;
        }
        cl::NDRange const origin = {0, startHeight, 0};
        cl::NDRange const range = {targetImage.getWidth(), endHeight, 1};
        m_programFront->run(
          "resample", origin, range, targetImage.getBuffer(), sourceImage.getBuffer());
    }

    void RenderProgram::resampleAsync(cl::CommandQueue const & queue,
                                      ImageRGBA & sourceImage,
                                      ImageRGBA & targetImage,
                                      size_t startHeight,
                                      size_t endHeight,
                                      cl::Event * completionEvent)
    {
        ProfileFunction;
        swapProgramsIfNeeded();
        if (!m_programFront->isValid())
        {
            if (completionEvent)
            {
                *completionEvent = cl::Event{};
            }
            return;
        }
        cl::NDRange const origin = {0, startHeight, 0};
        cl::NDRange const range = {targetImage.getWidth(), endHeight - startHeight, 1};
        cl::Event const event = m_programFront->runNonBlocking(
          queue, "resample", origin, range, targetImage.getBuffer(), sourceImage.getBuffer());
        if (completionEvent)
        {
            *completionEvent = event;
        }
    }

    cl::Event RenderProgram::renderSceneWithDistanceOutputAsync(
        cl::CommandQueue const & queue,
        Primitives const & lines,
        ImageRGBA & targetImage,
        DistanceInitBuffer & distanceOutput,
        cl_float z_mm,
        size_t startHeight,
        size_t endHeight)
    {
        return renderSceneWithDistanceOutputAsync(
          queue, lines, targetImage, distanceOutput,
          m_resources->getRenderingSettings(), z_mm, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneWithDistanceOutputAsync(
        cl::CommandQueue const & queue,
        Primitives const & lines,
        ImageRGBA & targetImage,
        DistanceInitBuffer & distanceOutput,
        RenderingSettings settings,
        cl_float z_mm,
        size_t startHeight,
        size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, z_mm, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        // Ensure time and z_mm are current in the caller's copy
        settings.time_s = m_resources->getTime_s();
        settings.z_mm = z_mm;

        try
        {
            kernelEvent = m_programFront->runNonBlocking(
              queue,
              "renderSceneWithDistanceOutput",
              setup.origin,
              setup.globalRange,
              targetImage.getBuffer(),
              distanceOutput.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              m_resources->getEyePosition(),
              m_resources->getModelViewPerspectiveMat());
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->logError(std::string("RenderProgram error: ") + e.what());
            }
        }

        return kernelEvent;
    }

    cl::Event RenderProgram::renderSceneWithDistanceInitAsync(
        cl::CommandQueue const & queue,
        Primitives const & lines,
        ImageRGBA & targetImage,
        DistanceInitBuffer & distanceInit,
        cl_float z_mm,
        size_t startHeight,
        size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, z_mm, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        // Enable distance initialization flag (T020)
        m_resources->getRenderingSettings().approximation = static_cast<ApproximationMode>(
            m_resources->getRenderingSettings().approximation | AM_USE_DISTANCE_INIT);

        try
        {
            kernelEvent = m_programFront->runNonBlocking(
              queue,
              "renderSceneWithDistanceInit",
              setup.origin,
              setup.globalRange,
              targetImage.getBuffer(),
              distanceInit.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              m_resources->getRenderingSettings(),
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              m_resources->getEyePosition(),
              m_resources->getModelViewPerspectiveMat());
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->logError(std::string("RenderProgram error: ") + e.what());
            }
        }

        // Clear the distance init flag after use
        m_resources->getRenderingSettings().approximation = static_cast<ApproximationMode>(
            m_resources->getRenderingSettings().approximation & ~AM_USE_DISTANCE_INIT);

        return kernelEvent;
    }

    cl::Event RenderProgram::renderSceneWithMetricsAsync(
        cl::CommandQueue const & queue,
        Primitives const & lines,
        ImageRGBA & targetImage,
        cl::Buffer & metricsBuffer,
        cl_float z_mm,
        size_t startHeight,
        size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, z_mm, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        try
        {
            kernelEvent = m_programFront->runNonBlocking(
              queue,
              "renderSceneWithMetrics",
              setup.origin,
              setup.globalRange,
              targetImage.getBuffer(),
              metricsBuffer,
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              m_resources->getRenderingSettings(),
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              m_resources->getEyePosition(),
              m_resources->getModelViewPerspectiveMat());
        }
        catch (std::exception const & e)
        {
            if (m_logger)
            {
                m_logger->logError(std::string("RenderProgram error: ") + e.what());
            }
        }

        return kernelEvent;
    }
}
