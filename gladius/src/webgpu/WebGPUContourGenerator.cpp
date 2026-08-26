#include "webgpu/WebGPUContourGenerator.h"

#include "webgpu/WebGPUSdfEvaluator.h"

#include <cmath>

namespace gladius::webgpu
{
    WebGPUContourGenerator::WebGPUContourGenerator()
        : m_context(std::make_shared<WebGPUComputeContext>())
    {
    }

    WebGPUContourGenerator::WebGPUContourGenerator(std::shared_ptr<WebGPUComputeContext> context)
        : m_context(std::move(context))
    {
    }

    bool WebGPUContourGenerator::isAvailable() const noexcept
    {
        return m_context && m_context->isValid();
    }

    std::string const & WebGPUContourGenerator::getErrorMessage() const noexcept
    {
        static std::string const empty;
        return m_context ? m_context->getErrorMessage() : empty;
    }

    std::optional<slicer::SdfGrid> WebGPUContourGenerator::renderSdfGrid(
      compute::SdfEvaluationRequest const & baseRequest,
      ContourGridRequest const & gridRequest) const
    {
        if (!isAvailable())
        {
            return std::nullopt;
        }
        if (gridRequest.width < 2 || gridRequest.height < 2)
        {
            throw std::invalid_argument("SDF grid resolution must be at least 2x2");
        }

        auto const cellSizeX =
          (gridRequest.clippingArea.z - gridRequest.clippingArea.x) /
          static_cast<float>(gridRequest.width - 1);
        auto const cellSizeY =
          (gridRequest.clippingArea.w - gridRequest.clippingArea.y) /
          static_cast<float>(gridRequest.height - 1);

        compute::SdfEvaluationRequest request = baseRequest;
        request.positions.clear();
        request.positions.reserve(static_cast<std::size_t>(gridRequest.width) *
                                  static_cast<std::size_t>(gridRequest.height));
        for (int y = 0; y < gridRequest.height; ++y)
        {
            for (int x = 0; x < gridRequest.width; ++x)
            {
                float const px = gridRequest.clippingArea.x + cellSizeX * static_cast<float>(x);
                float const py = gridRequest.clippingArea.y + cellSizeY * static_cast<float>(y);
                request.positions.push_back({px, py, gridRequest.zHeight_mm});
            }
        }

        auto const sampleCount = request.positions.size();
        auto result = WebGPUSdfEvaluator(m_context).evaluate(std::move(request));
        if (result.values.size() != sampleCount)
        {
            throw std::runtime_error("WebGPU SDF grid evaluation returned unexpected sample count");
        }

        slicer::SdfGrid grid;
        grid.width = gridRequest.width;
        grid.height = gridRequest.height;
        grid.clippingArea = gridRequest.clippingArea;
        grid.values = std::move(result.values);
        return grid;
    }

    PolyLines WebGPUContourGenerator::generateAdaptiveContours(
      compute::SdfEvaluationRequest const & baseRequest,
      ContourGridRequest const & gridRequest,
      events::SharedLogger const & logger) const
    {
        auto grid = renderSdfGrid(baseRequest, gridRequest);
        if (!grid.has_value())
        {
            throw std::runtime_error("WebGPU compute context is unavailable");
        }
        return slicer::GridContourBuilder::extractAdaptiveContours(
          *grid, gridRequest.minFeatureSize_mm, logger);
    }
}
