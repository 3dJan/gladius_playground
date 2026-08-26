#include "webgpu/WebGPUSdfShaderComposer.h"

#include <cmrc/cmrc.hpp>

#include <fmt/format.h>

#include <stdexcept>
#include <string_view>
#include <vector>

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
                                    std::vector<std::string> const & modules)
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

            std::string replacement;
            for (auto const & module : modules)
            {
                replacement += module + "\n";
            }
            replacement += std::string{modelEvaluator};
            shader.replace(markerPosition, evaluatorMarker.size(), replacement);
            return shader;
        }

        /// Load a shader module and strip its marker comment header.
        std::string loadEmbeddedModule(std::string_view const modulePath,
                                       std::string_view const moduleMarker)
        {
            std::string module = loadEmbeddedShader(modulePath);
            auto const markerPosition = module.find(moduleMarker);
            if (markerPosition != std::string::npos)
            {
                module.erase(markerPosition, moduleMarker.size());
            }
            return module;
        }
    }

    std::string WebGPUSdfShaderComposer::compose(std::string_view const modelEvaluator)
    {
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate.wgsl", modelEvaluator, {});
    }

    std::string WebGPUSdfShaderComposer::composeWithMeshSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, true, false, false);
    }

    std::string WebGPUSdfShaderComposer::composeWithBeamSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, false, true, false);
    }

    std::string WebGPUSdfShaderComposer::composeWithMeshAndBeamSupport(
      std::string_view const modelEvaluator)
    {
                return composeWithResourceSupport(modelEvaluator, true, true, false);
        }

        std::string WebGPUSdfShaderComposer::composeWithImageSupport(
            std::string_view const modelEvaluator)
        {
                return composeWithResourceSupport(modelEvaluator, false, false, true);
        }

        std::string WebGPUSdfShaderComposer::composeWithResourceSupport(
            std::string_view const modelEvaluator,
            bool const includeMesh,
            bool const includeBeam,
            bool const includeImage)
        {
                std::vector<std::string> modules;
                if (includeMesh)
        {
                        modules.push_back(loadEmbeddedModule(
                            "src/webgpu/shaders/mesh_sdf.wgsl", "// GLADIUS_MESH_SDF_MODULE"));
                }
                if (includeBeam)
                {
                        modules.push_back(loadEmbeddedModule(
                            "src/webgpu/shaders/beam_sdf.wgsl", "// GLADIUS_BEAM_SDF_MODULE"));
                }
                if (includeImage)
                {
                        modules.push_back(loadEmbeddedModule(
                            "src/webgpu/shaders/image_sampling.wgsl", "// GLADIUS_IMAGE_SAMPLING_MODULE"));
                }
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate.wgsl", modelEvaluator, modules);
    }
}
