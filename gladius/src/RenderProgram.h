#pragma once

#include "GLImageBuffer.h"
#include "Primitives.h"
#include "ProgramBase.h"
#include "ResourceContext.h"
#include "compute/RenderSessionInputs.h"

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

        void renderScene(cl::CommandQueue const & queue,
                         const Primitives & lines,
                         ImageRGBA & targetImage,
                         RenderSessionInputs inputs,
                         size_t startHeight,
                         size_t endHeight);

        [[nodiscard]] cl::Event renderSceneAsync(cl::CommandQueue const & queue,
                                                 const Primitives & lines,
                                                 ImageRGBA & targetImage,
                                                 cl_float z_mm,
                                                 size_t startHeight,
                                                 size_t endHeight);

        /// Overload that uses caller-provided session inputs instead of shared camera/settings.
        [[nodiscard]] cl::Event renderSceneAsync(cl::CommandQueue const & queue,
                                                 Primitives const & lines,
                                                 ImageRGBA & targetImage,
                                                 RenderSessionInputs inputs,
                                                 size_t startHeight,
                                                 size_t endHeight);

        [[nodiscard]] cl::Event renderSceneAsync(cl::CommandQueue const & queue,
                                                 Primitives const & lines,
                                                 ImageRGBA & targetImage,
                                                 RenderingSettings settings,
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

        /// @brief Render scene and write traveled distance to buffer for HQ init (T017/T018)
        /// @param queue Command queue to use for execution.
        /// @param lines Primitives for rendering.
        /// @param targetImage Target image buffer for color output.
        /// @param distanceOutput Output buffer for traveled distance (same resolution as
        /// targetImage).
        /// @param z_mm Slice height in millimeters.
        /// @param startHeight Starting row for rendering.
        /// @param endHeight Ending row for rendering.
        /// @return Completion event for synchronization.
        [[nodiscard]] cl::Event
        renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                           Primitives const & lines,
                                           ImageRGBA & targetImage,
                                           DistanceInitBuffer & distanceOutput,
                                           cl_float z_mm,
                                           size_t startHeight,
                                           size_t endHeight);

        /// Overload that uses caller-provided RenderingSettings.
        [[nodiscard]] cl::Event
        renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                           Primitives const & lines,
                                           ImageRGBA & targetImage,
                                           DistanceInitBuffer & distanceOutput,
                                           RenderSessionInputs inputs,
                                           size_t startHeight,
                                           size_t endHeight);

        [[nodiscard]] cl::Event
        renderSceneWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                           Primitives const & lines,
                                           ImageRGBA & targetImage,
                                           DistanceInitBuffer & distanceOutput,
                                           RenderingSettings settings,
                                           cl_float z_mm,
                                           size_t startHeight,
                                           size_t endHeight);

        /// @brief Render scene using distance init buffer to skip empty space (T016/T018)
        /// @param queue Command queue to use for execution.
        /// @param lines Primitives for rendering.
        /// @param targetImage Target image buffer for color output.
        /// @param distanceInit Input buffer with traveled distances from low-res preview.
        /// @param z_mm Slice height in millimeters.
        /// @param startHeight Starting row for rendering.
        /// @param endHeight Ending row for rendering.
        /// @return Completion event for synchronization.
        [[nodiscard]] cl::Event renderSceneWithDistanceInitAsync(cl::CommandQueue const & queue,
                                                                 Primitives const & lines,
                                                                 ImageRGBA & targetImage,
                                                                 DistanceInitBuffer & distanceInit,
                                                                 RenderSessionInputs inputs,
                                                                 size_t startHeight,
                                                                 size_t endHeight);

        [[nodiscard]] cl::Event renderSceneWithDistanceInitAsync(cl::CommandQueue const & queue,
                                                                 Primitives const & lines,
                                                                 ImageRGBA & targetImage,
                                                                 DistanceInitBuffer & distanceInit,
                                                                 cl_float z_mm,
                                                                 size_t startHeight,
                                                                 size_t endHeight);

        /// @brief Render scene with metrics collection for performance analysis (T033)
        /// @param queue Command queue to use for execution.
        /// @param lines Primitives for rendering.
        /// @param targetImage Target image buffer for color output.
        /// @param metricsBuffer Buffer for collecting ray march metrics (4 x uint32).
        /// @param z_mm Slice height in millimeters.
        /// @param startHeight Starting row for rendering.
        /// @param endHeight Ending row for rendering.
        /// @return Completion event for synchronization.
        [[nodiscard]] cl::Event renderSceneWithMetricsAsync(cl::CommandQueue const & queue,
                                                            Primitives const & lines,
                                                            ImageRGBA & targetImage,
                                                            cl::Buffer & metricsBuffer,
                                                            RenderSessionInputs inputs,
                                                            size_t startHeight,
                                                            size_t endHeight);

        [[nodiscard]] cl::Event renderSceneWithMetricsAsync(cl::CommandQueue const & queue,
                                                            Primitives const & lines,
                                                            ImageRGBA & targetImage,
                                                            cl::Buffer & metricsBuffer,
                                                            cl_float z_mm,
                                                            size_t startHeight,
                                                            size_t endHeight);

      private:
        /// @brief Result of render setup preparation
        struct RenderSetup
        {
            cl::NDRange origin;
            cl::NDRange globalRange;
            bool valid = false;
        };

        /// @brief Prepare common render parameters (validity checks, clamping, GL buffer handling)
        /// @param targetImage Target image buffer
        /// @param z_mm Slice height in millimeters
        /// @param startHeight Starting row for rendering
        /// @param endHeight Ending row for rendering
        /// @return RenderSetup with origin/range and validity flag
        [[nodiscard]] RenderSetup
        prepareRenderSetup(ImageRGBA & targetImage, size_t startHeight, size_t endHeight);
    };
}
