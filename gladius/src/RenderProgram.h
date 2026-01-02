#pragma once

#include "GLImageBuffer.h"
#include "Primitives.h"
#include "ProgramBase.h"
#include "ResourceContext.h"

namespace cl
{
    class CommandQueue;
    class Event;
}

namespace gladius
{
    class RenderProgram : public ProgramBase
    {
      public:
        explicit RenderProgram(SharedComputeContext context, const SharedResources & resources);

        void renderScene(const Primitives & lines,
                         ImageRGBA & targetImage,
                         cl_float z_mm,
                         size_t startHeight,
                         size_t endHeight);

  void renderScene(cl::CommandQueue const & queue,
       const Primitives & lines,
       ImageRGBA & targetImage,
       cl_float z_mm,
       size_t startHeight,
       size_t endHeight);

  [[nodiscard]] cl::Event renderSceneAsync(cl::CommandQueue const & queue,
             const Primitives & lines,
             ImageRGBA & targetImage,
             cl_float z_mm,
             size_t startHeight,
             size_t endHeight);

        void resample(ImageRGBA & sourceImage,
                      ImageRGBA & targetImage,
                      size_t startHeight,
                      size_t endHeight);

        /// @brief Non-blocking resample operation with explicit command queue.
        /// @param queue Command queue to use for execution.
        /// @param sourceImage Source low-resolution image buffer.
        /// @param targetImage Target high-resolution image buffer.
        /// @param startHeight Starting row for resampling.
        /// @param endHeight Ending row for resampling.
        /// @param completionEvent Output parameter for completion event (optional).
        void resampleAsync(cl::CommandQueue const & queue,
                           ImageRGBA & sourceImage,
                           ImageRGBA & targetImage,
                           size_t startHeight,
                           size_t endHeight,
                           cl::Event * completionEvent = nullptr);

      private:
    };
}
