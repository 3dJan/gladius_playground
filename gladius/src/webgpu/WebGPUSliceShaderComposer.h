#pragma once

#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /**
     * @brief Combines the stable slice entry-point template with a generated WGSL model evaluator.
     */
    class WebGPUSliceShaderComposer
    {
      public:
        [[nodiscard]] static std::string compose(std::string_view modelEvaluator);
    };
}
