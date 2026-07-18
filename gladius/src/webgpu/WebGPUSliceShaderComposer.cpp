#include "webgpu/WebGPUSliceShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <stdexcept>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    std::string WebGPUSliceShaderComposer::compose(std::string_view const modelEvaluator)
    {
        constexpr std::string_view SHADER_PATH = "src/webgpu/shaders/slice_compute.wgsl";
        constexpr std::string_view EVALUATOR_MARKER = "// GLADIUS_MODEL_EVALUATOR";

        if (modelEvaluator.empty())
        {
            throw std::invalid_argument("WGSL model evaluator source must not be empty");
        }

        auto const filesystem = cmrc::gladius_resources::get_filesystem();
        if (!filesystem.exists(SHADER_PATH.data()) || !filesystem.is_file(SHADER_PATH.data()))
        {
            throw std::runtime_error("Missing embedded WebGPU slice shader template");
        }

        auto const file = filesystem.open(SHADER_PATH.data());
        std::string shader{file.begin(), file.end()};
        auto const markerPosition = shader.find(EVALUATOR_MARKER);
        if (markerPosition == std::string::npos)
        {
            throw std::runtime_error("WebGPU slice shader template has no model evaluator insertion point");
        }

        shader.replace(markerPosition, EVALUATOR_MARKER.size(), modelEvaluator);
        return shader;
    }
}
