#pragma once

#include "compute/RenderContracts.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gladius::compute
{
    /**
     * @brief Immutable CPU payload for a supported analytic render scene.
     *
     * The initial payload deliberately covers only resource-independent analytic graphs. The
     * evaluator is WGSL because it is produced by the current analytic graph lowering path; mesh,
     * image, VDB, beam, command-stream, and precomputed-SDF payloads will extend this contract
     * with backend-independent resource descriptors rather than backend handles.
     */
    struct RenderSceneSnapshot
    {
        std::uint64_t sceneGeneration{};
        RendererCapability requiredCapabilities{RendererCapability::AnalyticRendering};
        std::string analyticEvaluatorWgsl;
        std::vector<float> parameterValues;

        [[nodiscard]] bool isValid() const noexcept
        {
            if (sceneGeneration == 0u || analyticEvaluatorWgsl.empty() ||
                !hasCapability(requiredCapabilities, RendererCapability::AnalyticRendering))
            {
                return false;
            }

            for (auto const value : parameterValues)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
            return true;
        }
    };
}
