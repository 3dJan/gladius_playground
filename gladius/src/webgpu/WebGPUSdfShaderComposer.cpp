#include "webgpu/WebGPUSdfShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <stdexcept>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    std::string WebGPUSdfShaderComposer::compose(std::string_view const modelEvaluator)
    {
        constexpr std::string_view shaderPath = "src/webgpu/shaders/sdf_evaluate.wgsl";
        constexpr std::string_view evaluatorMarker = "// GLADIUS_MODEL_EVALUATOR";

        if (modelEvaluator.empty())
        {
            throw std::invalid_argument("WGSL model evaluator source must not be empty");
        }

        auto const filesystem = cmrc::gladius_resources::get_filesystem();
        if (!filesystem.exists(shaderPath.data()) || !filesystem.is_file(shaderPath.data()))
        {
            throw std::runtime_error("Missing embedded WebGPU SDF evaluation shader");
        }

        auto const file = filesystem.open(shaderPath.data());
        std::string shader{file.begin(), file.end()};
        auto const markerPosition = shader.find(evaluatorMarker);
        if (markerPosition == std::string::npos)
        {
            throw std::runtime_error("WebGPU SDF evaluation shader has no evaluator insertion point");
        }

        shader.replace(markerPosition, evaluatorMarker.size(), modelEvaluator);
        return shader;
    }
}
