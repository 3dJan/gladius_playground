#pragma once

#include "compute/SdfEvaluation.h"
#include "webgpu/WebGPUComputeContext.h"

#include <memory>
#include <string>

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
        WebGPUSdfEvaluator();
        explicit WebGPUSdfEvaluator(std::shared_ptr<WebGPUComputeContext> context);

        [[nodiscard]] bool isAvailable() const noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;
        [[nodiscard]] compute::SdfEvaluationResult evaluate(compute::SdfEvaluationRequest request) const;

      private:
        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
