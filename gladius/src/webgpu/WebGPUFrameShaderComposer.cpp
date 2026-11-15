#include "webgpu/WebGPUFrameShaderComposer.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>

namespace
{
    std::filesystem::path ensureWebGpuDumpDir()
    {
        std::filesystem::path dir = std::filesystem::current_path() / "webgpu_dumps";
        try
        {
            if (!std::filesystem::exists(dir))
            {
                std::filesystem::create_directories(dir);
            }
        }
        catch (...)
        {
        }
        return dir;
    }

    void dumpShader(std::string const & shader, std::string const & filename)
    {
        try
        {
            auto const dumpDir = ensureWebGpuDumpDir();
            std::ofstream f(dumpDir / filename, std::ios::out | std::ios::trunc);
            if (!f.is_open())
                return;
            f << shader;
        }
        catch (...)
        {
        }
    }
}

#include <cmrc/cmrc.hpp>

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

        /// Insert the given modules (in order) plus the model evaluator at the
        /// evaluator marker of the frame shader template.
        /// \param translucencyEvaluator Optional second entry point appended after the model
        ///        evaluator so it can use the same resource modules and parameter storage.
        std::string composeFrameShader(std::vector<std::string> modules,
                                       std::string_view const modelEvaluator,
                                       std::string_view const translucencyEvaluator = {})
        {
            constexpr std::string_view SHADER_PATH = "src/webgpu/shaders/frame_compute.wgsl";
            constexpr std::string_view EVALUATOR_MARKER = "// GLADIUS_MODEL_EVALUATOR";

            if (modelEvaluator.empty())
            {
                throw std::invalid_argument("WGSL model evaluator source must not be empty");
            }

            std::string shader = loadEmbeddedShader(SHADER_PATH);
            auto const markerPosition = shader.find(EVALUATOR_MARKER);
            if (markerPosition == std::string::npos)
            {
                throw std::runtime_error(
                  "WebGPU frame shader template has no model evaluator insertion point");
            }

            std::string replacement;
            for (auto const & module : modules)
            {
                replacement += module + "\n";
            }
            replacement += std::string{modelEvaluator};
            replacement += "\n";
            replacement += translucencyEvaluator.empty()
                               ? std::string{"fn evaluateTranslucency(position: vec3<f32>) -> f32 {\n"
                                             "    return 0.0f;\n}\n"}
                               : std::string{translucencyEvaluator};

            shader.replace(markerPosition, EVALUATOR_MARKER.size(), replacement);

            dumpShader(shader, "frame_compute_composed.wgsl");

            return shader;
        }
    }

    std::string WebGPUFrameShaderComposer::composeWithMeshSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, true, false, false);
    }

    std::string WebGPUFrameShaderComposer::compose(std::string_view const modelEvaluator,
                                                   std::string_view const translucencyEvaluator)
    {
        return composeFrameShader({}, modelEvaluator, translucencyEvaluator);
    }

    std::string WebGPUFrameShaderComposer::composeWithBeamSupport(std::string_view const modelEvaluator)
    {
        return composeWithResourceSupport(modelEvaluator, false, true, false);
    }

    std::string WebGPUFrameShaderComposer::composeWithMeshAndBeamSupport(
      std::string_view const modelEvaluator)
    {
                return composeWithResourceSupport(modelEvaluator, true, true, false);
        }

        std::string WebGPUFrameShaderComposer::composeWithImageSupport(
            std::string_view const modelEvaluator)
        {
                return composeWithResourceSupport(modelEvaluator, false, false, true);
        }

        std::string WebGPUFrameShaderComposer::composeWithResourceSupport(
            std::string_view const modelEvaluator,
            bool const includeMesh,
            bool const includeBeam,
            bool const includeImage,
            std::string_view const translucencyEvaluator)
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
        return composeFrameShader(std::move(modules), modelEvaluator, translucencyEvaluator);
    }

        std::string WebGPUFrameShaderComposer::composeSdfPrebake(std::string_view const modelEvaluator,
                                                     bool const includeMesh,
                                                     bool const includeBeam,
                                                     bool const includeImage)
        {
                if (modelEvaluator.empty())
                {
                    throw std::invalid_argument("WGSL model evaluator source must not be empty");
                }

                std::string shader = loadEmbeddedShader("src/webgpu/shaders/sdf_prebake.wgsl");
                auto const replaceMarker = [&](std::string_view marker, std::string const & content)
                {
                    auto const pos = shader.find(marker);
                    if (pos != std::string::npos)
                    {
        shader.replace(pos, marker.size(), content);
                    }
                };

                replaceMarker("// GLADIUS_MESH_SDF_MODULE",
              includeMesh ? loadEmbeddedModule("src/webgpu/shaders/mesh_sdf.wgsl",
                                               "// GLADIUS_MESH_SDF_MODULE")
                          : std::string{});
                replaceMarker("// GLADIUS_BEAM_SDF_MODULE",
              includeBeam ? loadEmbeddedModule("src/webgpu/shaders/beam_sdf.wgsl",
                                               "// GLADIUS_BEAM_SDF_MODULE")
                          : std::string{});
                replaceMarker("// GLADIUS_IMAGE_SAMPLING_MODULE",
              includeImage ? loadEmbeddedModule("src/webgpu/shaders/image_sampling.wgsl",
                                                "// GLADIUS_IMAGE_SAMPLING_MODULE")
                           : std::string{});
                replaceMarker("// GLADIUS_MODEL_EVALUATOR", std::string{modelEvaluator});
                return shader;
        }

    std::string WebGPUFrameShaderComposer::applyWorkgroupShape(std::string shaderSource,
                                                               compute::WorkgroupShape const shape)
    {
        if (shape.isDefault())
        {
            return shaderSource;
        }
        if (!shape.isValid())
        {
            throw std::invalid_argument("Invalid WebGPU workgroup shape " + shape.toString());
        }

        static constexpr std::string_view defaultDeclaration = "@compute @workgroup_size(16, 16, 1)";
        auto const position = shaderSource.find(defaultDeclaration);
        if (position == std::string::npos)
        {
            throw std::runtime_error("WebGPU frame shader has no default workgroup declaration");
        }

        auto const replacement =
            fmt::format("@compute @workgroup_size({}, {}, 1)", shape.width, shape.height);
        shaderSource.replace(position, defaultDeclaration.size(), replacement);

        static constexpr std::string_view tileColumns = "const tile_columns: u32 = 16u;";
        static constexpr std::string_view tileRows = "const tile_rows: u32 = 16u;";
        auto const columnsPosition = shaderSource.find(tileColumns);
        auto const rowsPosition = shaderSource.find(tileRows);
        if (columnsPosition == std::string::npos || rowsPosition == std::string::npos)
        {
            throw std::runtime_error("WebGPU frame shader has no tile shape declaration");
        }

        // Replace the later declaration first so the earlier offset stays valid.
        shaderSource.replace(rowsPosition, tileRows.size(),
                             fmt::format("const tile_rows: u32 = {}u;", shape.height));
        shaderSource.replace(columnsPosition, tileColumns.size(),
                             fmt::format("const tile_columns: u32 = {}u;", shape.width));
        return shaderSource;
    }
}
