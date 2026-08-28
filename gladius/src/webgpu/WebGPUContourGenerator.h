#pragma once

#include "Contour.h"
#include "EventLogger.h"
#include "compute/SdfEvaluation.h"
#include "slicer/GridContourBuilder.h"
#include "webgpu/WebGPUComputeContext.h"

#include <memory>
#include <string>

namespace gladius::webgpu
{
    /// Parameters describing the slice plane and sampling resolution.
    struct ContourGridRequest
    {
        float zHeight_mm{};
        /// Clipping area: x = xMin, y = yMin, z = xMax, w = yMax (mm).
        float4 clippingArea{};
        int width{256};
        int height{256};
        float minFeatureSize_mm{0.2f};
        bool useAdaptiveContour{true};
    };

    /**
     * @brief Generates 2D contours on the WebGPU backend using a hybrid approach:
     *
     * The model SDF is evaluated on an NxM grid at fixed z by the WebGPU backend
     * (WebGPUSdfEvaluator), read back to host memory, and contour extraction runs
     * through the shared CPU code path (GridContourBuilder), mirroring the OpenCL
     * dense marching-squares and adaptive quadtree pipelines.
     */
    class WebGPUContourGenerator final
    {
      public:
        WebGPUContourGenerator();
        explicit WebGPUContourGenerator(std::shared_ptr<WebGPUComputeContext> context);

        [[nodiscard]] bool isAvailable() const noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;

        /**
         * @brief Evaluate the composed model SDF on the request grid via WebGPU.
         * @return The sampled SDF grid, or std::nullopt if the device is unavailable.
         */
        [[nodiscard]] std::optional<slicer::SdfGrid> renderSdfGrid(
          compute::SdfEvaluationRequest const & baseRequest,
          ContourGridRequest const & gridRequest) const;

        /**
         * @brief Full pipeline: render the SDF grid on WebGPU and extract polylines
         *        through the shared adaptive quadtree host code.
         *
         * For the dense marching-squares path use renderSdfGrid() plus
         * slicer::GridContourBuilder::extractDenseContours() with an OpenCL
         * ComputeContext (required by the MarchingSquaresStates image type).
         */
        [[nodiscard]] PolyLines generateAdaptiveContours(
          compute::SdfEvaluationRequest const & baseRequest,
          ContourGridRequest const & gridRequest,
          events::SharedLogger const & logger) const;

      private:
        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
