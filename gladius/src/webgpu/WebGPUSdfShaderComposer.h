#pragma once

#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /**
     * @brief Inserts a generated model evaluator into the raw SDF evaluation shader.
     */
    class WebGPUSdfShaderComposer final
    {
      public:
        [[nodiscard]] static std::string compose(std::string_view modelEvaluator);
    };
}
