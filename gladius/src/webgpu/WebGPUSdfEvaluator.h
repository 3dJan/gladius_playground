#pragma once

#include "compute/SdfEvaluation.h"
#include "webgpu/WebGPUComputeContext.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gladius::webgpu
{
    /**
     * @brief Evaluates generated analytic SDF functions at arbitrary world-space points.
     *
     * Unlike the render backends, this class returns the model's scalar distance directly and
     * does not perform ray marching, shading, or pixel packing.
     */
    class WebGPUSdfEvaluator final
    {
      public:
        static constexpr std::size_t MAX_BATCH_POINTS = 1024u * 1024u;

        using PositionBatchSource =
          std::function<void(std::size_t, std::size_t, std::vector<std::array<float, 3>> &)>;
        using ValueBatchSink = std::function<void(std::size_t, std::vector<float> &&)>;
        using ColorBatchSink =
          std::function<void(std::size_t, std::vector<std::array<float, 3>> &&)>;
        using CancellationPredicate = std::function<bool()>;

        WebGPUSdfEvaluator();
        explicit WebGPUSdfEvaluator(std::shared_ptr<WebGPUComputeContext> context);

        [[nodiscard]] bool isAvailable() const noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;
        [[nodiscard]] compute::SdfEvaluationResult evaluate(compute::SdfEvaluationRequest request) const;

        /// @brief Evaluate generated positions in bounded batches, retaining GPU buffers between batches.
        [[nodiscard]] bool evaluateGenerated(compute::SdfEvaluationRequest const& request,
                     std::size_t pointCount,
                     PositionBatchSource positionSource,
                     ValueBatchSink valueSink,
                     CancellationPredicate shouldCancel = {}) const;

        /// @brief Evaluate generated positions and return the model RGB output in bounded batches.
        [[nodiscard]] bool evaluateColorsGenerated(
          compute::SdfEvaluationRequest const & request,
          std::size_t pointCount,
          PositionBatchSource positionSource,
          ColorBatchSink colorSink,
          CancellationPredicate shouldCancel = {}) const;

      private:
        using RawValueBatchSink = std::function<void(std::size_t, std::vector<float> &&)>;

        [[nodiscard]] bool evaluateGeneratedImpl(
          compute::SdfEvaluationRequest const & request,
          std::size_t pointCount,
          std::size_t outputComponents,
          PositionBatchSource positionSource,
          RawValueBatchSink valueSink,
          CancellationPredicate shouldCancel) const;

        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
