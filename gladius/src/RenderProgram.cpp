#include "RenderProgram.h"
#include "Profiling.h"
#include "ProgramBase.h"
#include "gpgpu.h"

#include <CL/cl_platform.h>
#include <algorithm>

#include <cstddef>

namespace gladius
{
    RenderProgram::RenderProgram(SharedComputeContext context, const SharedResources & resources)
        : ProgramBase(context, resources)
    {
        m_sourceFiles = {"types.h",
                         "arguments.h",
                         "sdf.h",
                         "sampler.h",
                         "rendering.h",
                         "CNanoVDB.h",
                         "sdf.cl",
                         "rendering.cl",
                         "renderer.cl"};
    }

    void RenderProgram::renderScene(const Primitives & lines,
                                    ImageRGBA & targetImage,
                                    cl_float z_mm,
                                    size_t startHeight,
                                    size_t endHeight)
    {
        renderScene(m_ComputeContext->GetQueue(),
                    lines,
                    targetImage,
                    z_mm,
                    startHeight,
                    endHeight);
    }

    void RenderProgram::renderScene(cl::CommandQueue const & queue,
                                    const Primitives & lines,
                                    ImageRGBA & targetImage,
                                    cl_float z_mm,
                                    size_t startHeight,
                                    size_t endHeight)
    {
        try
        {
            cl::Event const event = renderSceneAsync(queue,
                                                     lines,
                                                     targetImage,
                                                     z_mm,
                                                     startHeight,
                                                     endHeight);
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
        ProfileFunction;
        cl::Event kernelEvent{};
        if (!m_programFront->isValid())
        {
            return kernelEvent;
        }
        swapProgramsIfNeeded();

        if (startHeight >= endHeight)
        {
            return kernelEvent;
        }
        auto const start = std::clamp(startHeight, size_t(0), targetImage.getHeight() - 2);
        auto const size =
          std::clamp(endHeight - startHeight, size_t{0}, targetImage.getHeight() - start - 1);

        if (size < 1 || size > 16000)
        {
            return kernelEvent;
        }
        cl::NDRange const origin = {0, start, 0};
        cl::NDRange const globalRange = {targetImage.getWidth(), size, 1};

        if (auto * glImageBuffer = dynamic_cast<GLImageBuffer *>(&targetImage);
            glImageBuffer != nullptr)
        {
            glImageBuffer->invalidateContent();
        }

        m_resoures->getRenderingSettings().time_s = m_resoures->getTime_s();
        m_resoures->getRenderingSettings().z_mm = z_mm;

        if (!m_programFront->isValid())
        {
            return kernelEvent;
        }

        try
        {
            kernelEvent = m_programFront->runNonBlocking(queue,
                                                         "renderScene",
                                                         origin,
                                                         globalRange,
                                                         targetImage.getBuffer(),
                                                         m_resoures->getBuildArea(),
                                                         lines.primitives.getBuffer(),
                                                         cl_int(lines.primitives.getSize()),
                                                         lines.data.getBuffer(),
                                                         cl_int(lines.data.getSize()),
                                                         m_resoures->getRenderingSettings(),
                                                         m_resoures->getPrecompSdfBuffer().getBuffer(),
                                                         m_resoures->getParameterBuffer().getBuffer(),
                                                         m_resoures->getCommandBuffer().getBuffer(),
                                                         cl_int(m_resoures->getCommandBuffer().getData().size()),
                                                         m_resoures->getPreCompSdfBBox(),
                                                         m_resoures->getEyePosition(),
                                                         m_resoures->getModelViewPerspectiveMat());
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
}
