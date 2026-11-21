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

        void replaceMarker(std::string & shader, std::string_view const marker, std::string_view const replacement)
        {
            auto const markerPosition = shader.find(marker);
            if (markerPosition == std::string::npos)
            {
                throw std::runtime_error(fmt::format("WebGPU SDF evaluation shader has no insertion point: {}", marker));
            }

            shader.replace(markerPosition, marker.size(), replacement);
        }

        std::string modulesPrefix(std::vector<std::string> const & modules)
        {
            std::string replacement;
            for (auto const & module : modules)
            {
                replacement += module + "\n";
            }
            return replacement;
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
            replaceMarker(shader, evaluatorMarker, modulesPrefix(modules) + std::string{modelEvaluator});
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

        std::vector<std::string> collectResourceModules(bool const includeMesh,
                                                        bool const includeBeam,
                                                        bool const includeImage)
        {
            std::vector<std::string> modules;
            if (includeMesh)
            {
                modules.push_back(
                  loadEmbeddedModule("src/webgpu/shaders/mesh_sdf.wgsl", "// GLADIUS_MESH_SDF_MODULE"));
            }
            if (includeBeam)
            {
                modules.push_back(
                  loadEmbeddedModule("src/webgpu/shaders/beam_sdf.wgsl", "// GLADIUS_BEAM_SDF_MODULE"));
            }
            if (includeImage)
            {
                modules.push_back(
                  loadEmbeddedModule("src/webgpu/shaders/image_sampling.wgsl", "// GLADIUS_IMAGE_SAMPLING_MODULE"));
            }
            return modules;
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


    std::string WebGPUSdfShaderComposer::composeWithMeshAndBeamSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, true, true, false);
    }

    std::string WebGPUSdfShaderComposer::composeWithImageSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, false, false, true);
    }

    std::string WebGPUSdfShaderComposer::composeWithResourceSupport(std::string_view const modelEvaluator,
                                                                   bool const includeMesh,
                                                                   bool const includeBeam,
                                                                   bool const includeImage)
    {
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate.wgsl",
                               modelEvaluator,
                               collectResourceModules(includeMesh, includeBeam, includeImage));
    }

    std::string WebGPUSdfShaderComposer::composeColorWithResourceSupport(std::string_view const modelEvaluator,
                                                                        bool const includeMesh,
                                                                        bool const includeBeam,
                                                                        bool const includeImage)
    {
        return insertEvaluator("src/webgpu/shaders/sdf_evaluate_color.wgsl",
                               modelEvaluator,
                               collectResourceModules(includeMesh, includeBeam, includeImage));
    }

    std::string WebGPUSdfShaderComposer::composeFunctionWithResourceSupport(std::string_view const modelEvaluator,
                                                                           std::string_view const abiDeclarations,
                                                                           bool const includeMesh,
                                                                           bool const includeBeam,
                                                                           bool const includeImage)
    {
        constexpr std::string_view abiMarker = "// GLADIUS_FUNCTION_ABI";

        if (abiDeclarations.empty())
        {
            throw std::invalid_argument("WGSL function evaluation ABI must not be empty");
        }

        auto shader = insertEvaluator("src/webgpu/shaders/sdf_evaluate_function.wgsl",
                                      modelEvaluator,
                                      collectResourceModules(includeMesh, includeBeam, includeImage));
        replaceMarker(shader, abiMarker, abiDeclarations);
        return shader;
    }
}
