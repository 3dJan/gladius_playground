#pragma once

#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /**
     * @brief Combines the headless frame ray-march template with a generated WGSL model evaluator.
     */
    class WebGPUFrameShaderComposer
    {
      public:
        [[nodiscard]] static std::string compose(std::string_view modelEvaluator);
    };
}