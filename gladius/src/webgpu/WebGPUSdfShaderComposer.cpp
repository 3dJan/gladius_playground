#include "webgpu/WebGPUSdfShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <fmt/format.h>

#include <stdexcept>
#include <string_view>

CMRC_DECLARE(gladius_resources);

namespace gladius::webgpu
{
    namespace
    {
        std::string loadEmbeddedShader(std::string_view const shaderPath)
        {
            auto const filesystem = cmrc::gladius_resources::get_filesystem();
            if (!filesystem.exists(shaderPath.data()) || !filesystem.is_file(shaderPath.data()))
            {
                throw std::runtime_error(
                  fmt::format("Missing embedded WebGPU shader template: {}", shaderPath));
            }

            auto const file = filesystem.open(shaderPath.data());
            return std::string{file.begin(), file.end()};
        }

        std::string insertEvaluator(std::string_view const shaderPath,
                                    std::string_view const modelEvaluator,
                                    std::string const & meshModule)
        {
            constexpr std::string_view evaluatorMarker = "// GLADIUS_MODEL_EVALUATOR";

            if (modelEvaluator.empty())
            {
                throw std::invalid_argument("WGSL model evaluator source must not be empty");
            }

            std::string shader = loadEmbeddedShader(shaderPath);
            auto const markerPosition = shader.find(evaluatorMarker);
            if (markerPosition == std::string::npos)
            {
                throw std::runtime_error("WebGPU SDF evaluation shader has no evaluator insertion point");
            }

            std::string const replacement =
              meshModule.empty() ? std::string{modelEvaluator} : meshModule + "\n" + std::string{modelEvaluator};
            shader.replace(markerPosition, evaluatorMarker.size(), replacement);
            return shader;
        }
    }

    std::string WebGPUSdfShaderComposer::compose(std::string_view const modelEvaluator)
    {
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate.wgsl", modelEvaluator, {});
    }

    std::string WebGPUSdfShaderComposer::composeWithMeshSupport(std::string_view const modelEvaluator)
    {
        constexpr std::string_view MESH_MODULE_PATH = "src/webgpu/shaders/mesh_sdf.wgsl";
        constexpr std::string_view MESH_MODULE_MARKER = "// GLADIUS_MESH_SDF_MODULE";

        std::string meshModule = loadEmbeddedShader(MESH_MODULE_PATH);
        auto const markerPosition = meshModule.find(MESH_MODULE_MARKER);
        if (markerPosition != std::string::npos)
        {
            meshModule.erase(markerPosition, MESH_MODULE_MARKER.size());
        }
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate.wgsl", modelEvaluator, meshModule);
    }
}
