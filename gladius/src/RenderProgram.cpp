#include "RenderProgram.h"
#include "Profiling.h"
#include "ProgramBase.h"
#include "compute/GpuKernelAccessGuard.h"
#include "compute/RenderPayloadSnapshot.h"
#include "gpgpu.h"
#include "kernel/types.h"

#include <CL/cl_platform.h>
#include <algorithm>

#include <cstddef>

namespace gladius
{
    namespace
    {
        [[nodiscard]] std::vector<GpuKernelResourceAccess>
        makeRenderPayloadReadAccesses(SharedResources const & resources, Primitives const & lines)
        { return RenderPayloadSnapshot::capture(resources, lines).readAccesses(); }

        void appendAccess(std::vector<GpuKernelResourceAccess> & accesses,
                          GpuResourceHandle resource,
                          GpuAccessMode mode)
        { accesses.push_back(GpuKernelResourceAccess{.resource = resource, .mode = mode}); }
    }

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

    RenderProgram::RenderSetup
    RenderProgram::prepareRenderSetup(ImageRGBA & targetImage, size_t startHeight, size_t endHeight)
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

        if (targetImage.getHeight() < 2 || targetImage.getWidth() == 0)
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
    { renderScene(m_ComputeContext->GetQueue(), lines, targetImage, z_mm, startHeight, endHeight); }

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

    void RenderProgram::renderScene(cl::CommandQueue const & queue,
                                    const Primitives & lines,
                                    ImageRGBA & targetImage,
                                    RenderSessionInputs inputs,
                                    size_t startHeight,
                                    size_t endHeight)
    {
        ProfileFunction;
        try
        {
            cl::Event const event = renderSceneAsync(
              queue, lines, targetImage, std::move(inputs), startHeight, endHeight);
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
        RenderSessionInputs inputs{m_resources->getRenderingSettings(),
                                   m_resources->getEyePosition(),
                                   m_resources->getModelViewPerspectiveMat()};
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneAsync(queue, lines, targetImage, inputs, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneAsync(cl::CommandQueue const & queue,
                                              Primitives const & lines,
                                              ImageRGBA & targetImage,
                                              RenderSessionInputs inputs,
                                              size_t startHeight,
                                              size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        try
        {
            auto accesses = makeRenderPayloadReadAccesses(m_resources, lines);
            appendAccess(accesses, targetImage.gpuResourceHandle(), GpuAccessMode::Write);
            GpuKernelAccessGuard gpuAccess(
              *m_ComputeContext, queue, "renderScene", std::move(accesses));
            if (!gpuAccess.granted())
            {
                return kernelEvent;
            }

            kernelEvent = m_programFront->runNonBlockingWithWaitList(
              queue,
              "renderScene",
              setup.origin,
              setup.globalRange,
              gpuAccess.waitEvents(),
              targetImage.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              inputs.settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              inputs.eyePosition,
              inputs.modelViewPerspectiveMat);
            gpuAccess.complete(kernelEvent);
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

    cl::Event RenderProgram::renderSceneAsync(cl::CommandQueue const & queue,
                                              Primitives const & lines,
                                              ImageRGBA & targetImage,
                                              RenderingSettings settings,
                                              cl_float z_mm,
                                              size_t startHeight,
                                              size_t endHeight)
    {
        RenderSessionInputs inputs{
          settings, m_resources->getEyePosition(), m_resources->getModelViewPerspectiveMat()};
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneAsync(queue, lines, targetImage, inputs, startHeight, endHeight);
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
        GpuKernelAccessGuard gpuAccess(*m_ComputeContext,
                                       queue,
                                       "resample",
                                       {{targetImage.gpuResourceHandle(), GpuAccessMode::Write},
                                        {sourceImage.gpuResourceHandle(), GpuAccessMode::Read}});
        if (!gpuAccess.granted())
        {
            if (completionEvent)
            {
                *completionEvent = cl::Event{};
            }
            return;
        }
        cl::Event const event = m_programFront->runNonBlockingWithWaitList(queue,
                                                                           "resample",
                                                                           origin,
                                                                           range,
                                                                           gpuAccess.waitEvents(),
                                                                           targetImage.getBuffer(),
                                                                           sourceImage.getBuffer());
        gpuAccess.complete(event);
        if (completionEvent)
        {
            *completionEvent = event;
        }
    }

    cl::Event RenderProgram::renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                                                Primitives const & lines,
                                                                ImageRGBA & targetImage,
                                                                DistanceInitBuffer & distanceOutput,
                                                                cl_float z_mm,
                                                                size_t startHeight,
                                                                size_t endHeight)
    {
        RenderSessionInputs inputs{m_resources->getRenderingSettings(),
                                   m_resources->getEyePosition(),
                                   m_resources->getModelViewPerspectiveMat()};
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneWithDistanceOutputAsync(
          queue, lines, targetImage, distanceOutput, inputs, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                                                Primitives const & lines,
                                                                ImageRGBA & targetImage,
                                                                DistanceInitBuffer & distanceOutput,
                                                                RenderingSettings settings,
                                                                cl_float z_mm,
                                                                size_t startHeight,
                                                                size_t endHeight)
    {
        RenderSessionInputs inputs{
          settings, m_resources->getEyePosition(), m_resources->getModelViewPerspectiveMat()};
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneWithDistanceOutputAsync(
          queue, lines, targetImage, distanceOutput, inputs, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                                                Primitives const & lines,
                                                                ImageRGBA & targetImage,
                                                                DistanceInitBuffer & distanceOutput,
                                                                RenderSessionInputs inputs,
                                                                size_t startHeight,
                                                                size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        try
        {
            auto accesses = makeRenderPayloadReadAccesses(m_resources, lines);
            appendAccess(accesses, targetImage.gpuResourceHandle(), GpuAccessMode::Write);
            appendAccess(accesses, distanceOutput.gpuResourceHandle(), GpuAccessMode::Write);
            GpuKernelAccessGuard gpuAccess(
              *m_ComputeContext, queue, "renderSceneWithDistanceOutput", std::move(accesses));
            if (!gpuAccess.granted())
            {
                return kernelEvent;
            }

            kernelEvent = m_programFront->runNonBlockingWithWaitList(
              queue,
              "renderSceneWithDistanceOutput",
              setup.origin,
              setup.globalRange,
              gpuAccess.waitEvents(),
              targetImage.getBuffer(),
              distanceOutput.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              inputs.settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              inputs.eyePosition,
              inputs.modelViewPerspectiveMat);
            gpuAccess.complete(kernelEvent);
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

    cl::Event RenderProgram::renderSceneWithDistanceInitAsync(cl::CommandQueue const & queue,
                                                              Primitives const & lines,
                                                              ImageRGBA & targetImage,
                                                              DistanceInitBuffer & distanceInit,
                                                              cl_float z_mm,
                                                              size_t startHeight,
                                                              size_t endHeight)
    {
        RenderSessionInputs inputs{m_resources->getRenderingSettings(),
                                   m_resources->getEyePosition(),
                                   m_resources->getModelViewPerspectiveMat()};
        inputs.settings.approximation =
          static_cast<ApproximationMode>(inputs.settings.approximation | AM_USE_DISTANCE_INIT);
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneWithDistanceInitAsync(
          queue, lines, targetImage, distanceInit, inputs, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneWithDistanceInitAsync(cl::CommandQueue const & queue,
                                                              Primitives const & lines,
                                                              ImageRGBA & targetImage,
                                                              DistanceInitBuffer & distanceInit,
                                                              RenderSessionInputs inputs,
                                                              size_t startHeight,
                                                              size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        try
        {
            auto accesses = makeRenderPayloadReadAccesses(m_resources, lines);
            appendAccess(accesses, targetImage.gpuResourceHandle(), GpuAccessMode::Write);
            appendAccess(accesses, distanceInit.gpuResourceHandle(), GpuAccessMode::Read);
            GpuKernelAccessGuard gpuAccess(
              *m_ComputeContext, queue, "renderSceneWithDistanceInit", std::move(accesses));
            if (!gpuAccess.granted())
            {
                return kernelEvent;
            }

            kernelEvent = m_programFront->runNonBlockingWithWaitList(
              queue,
              "renderSceneWithDistanceInit",
              setup.origin,
              setup.globalRange,
              gpuAccess.waitEvents(),
              targetImage.getBuffer(),
              distanceInit.getBuffer(),
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              inputs.settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              inputs.eyePosition,
              inputs.modelViewPerspectiveMat);
            gpuAccess.complete(kernelEvent);
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

    cl::Event RenderProgram::renderSceneWithMetricsAsync(cl::CommandQueue const & queue,
                                                         Primitives const & lines,
                                                         ImageRGBA & targetImage,
                                                         cl::Buffer & metricsBuffer,
                                                         cl_float z_mm,
                                                         size_t startHeight,
                                                         size_t endHeight)
    {
        RenderSessionInputs inputs{m_resources->getRenderingSettings(),
                                   m_resources->getEyePosition(),
                                   m_resources->getModelViewPerspectiveMat()};
        inputs.settings.z_mm = z_mm;
        inputs.settings.time_s = m_resources->getTime_s();
        return renderSceneWithMetricsAsync(
          queue, lines, targetImage, metricsBuffer, inputs, startHeight, endHeight);
    }

    cl::Event RenderProgram::renderSceneWithMetricsAsync(cl::CommandQueue const & queue,
                                                         Primitives const & lines,
                                                         ImageRGBA & targetImage,
                                                         cl::Buffer & metricsBuffer,
                                                         RenderSessionInputs inputs,
                                                         size_t startHeight,
                                                         size_t endHeight)
    {
        ProfileFunction;
        cl::Event kernelEvent{};

        auto const setup = prepareRenderSetup(targetImage, startHeight, endHeight);
        if (!setup.valid)
        {
            return kernelEvent;
        }

        try
        {
            auto accesses = makeRenderPayloadReadAccesses(m_resources, lines);
            appendAccess(accesses, targetImage.gpuResourceHandle(), GpuAccessMode::Write);
            appendAccess(
              accesses, m_resources->getMetricsBufferGpuResource(), GpuAccessMode::Write);
            GpuKernelAccessGuard gpuAccess(
              *m_ComputeContext, queue, "renderSceneWithMetrics", std::move(accesses));
            if (!gpuAccess.granted())
            {
                return kernelEvent;
            }

            kernelEvent = m_programFront->runNonBlockingWithWaitList(
              queue,
              "renderSceneWithMetrics",
              setup.origin,
              setup.globalRange,
              gpuAccess.waitEvents(),
              targetImage.getBuffer(),
              metricsBuffer,
              m_resources->getBuildArea(),
              lines.primitives.getBuffer(),
              cl_int(lines.primitives.getSize()),
              lines.data.getBuffer(),
              cl_int(lines.data.getSize()),
              inputs.settings,
              m_resources->getPrecompSdfBuffer().getBuffer(),
              m_resources->getParameterBuffer().getBuffer(),
              m_resources->getCommandBuffer().getBuffer(),
              cl_int(m_resources->getCommandBuffer().getData().size()),
              m_resources->getPreCompSdfBBox(),
              inputs.eyePosition,
              inputs.modelViewPerspectiveMat);
            gpuAccess.complete(kernelEvent);
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
