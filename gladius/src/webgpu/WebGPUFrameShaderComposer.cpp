#include "webgpu/WebGPUFrameShaderComposer.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

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
    }

    std::string WebGPUFrameShaderComposer::composeWithMeshSupport(std::string_view const modelEvaluator)
    {
        constexpr std::string_view SHADER_PATH = "src/webgpu/shaders/frame_compute.wgsl";
        constexpr std::string_view MESH_MODULE_PATH = "src/webgpu/shaders/mesh_sdf.wgsl";
        constexpr std::string_view EVALUATOR_MARKER = "// GLADIUS_MODEL_EVALUATOR";
        constexpr std::string_view MESH_MODULE_MARKER = "// GLADIUS_MESH_SDF_MODULE";

        if (modelEvaluator.empty())
        {
            throw std::invalid_argument("WGSL model evaluator source must not be empty");
        }

        std::string shader = loadEmbeddedShader(SHADER_PATH);
        std::string meshModule = loadEmbeddedShader(MESH_MODULE_PATH);

        // The mesh module declares its payload and offset-table bindings and the mesh
        // SDF entry points; strip its marker comment header before injection.
        auto const meshMarkerPosition = meshModule.find(MESH_MODULE_MARKER);
        if (meshMarkerPosition != std::string::npos)
        {
            meshModule.erase(meshMarkerPosition, MESH_MODULE_MARKER.size());
        }

        auto const markerPosition = shader.find(EVALUATOR_MARKER);
        if (markerPosition == std::string::npos)
        {
            throw std::runtime_error("WebGPU frame shader template has no model evaluator insertion point");
        }

        shader.replace(markerPosition, EVALUATOR_MARKER.size(), meshModule + "\n" + std::string{modelEvaluator});

        dumpShader(shader, "frame_compute_composed.wgsl");

        return shader;
    }

    std::string WebGPUFrameShaderComposer::compose(std::string_view const modelEvaluator)
    {
        constexpr std::string_view SHADER_PATH = "src/webgpu/shaders/frame_compute.wgsl";
        constexpr std::string_view EVALUATOR_MARKER = "// GLADIUS_MODEL_EVALUATOR";

        if (modelEvaluator.empty())
        {
            throw std::invalid_argument("WGSL model evaluator source must not be empty");
        }

        auto const filesystem = cmrc::gladius_resources::get_filesystem();
        if (!filesystem.exists(SHADER_PATH.data()) || !filesystem.is_file(SHADER_PATH.data()))
        {
            throw std::runtime_error("Missing embedded WebGPU frame shader template");
        }

        auto const file = filesystem.open(SHADER_PATH.data());
        std::string shader{file.begin(), file.end()};
        auto const markerPosition = shader.find(EVALUATOR_MARKER);
        if (markerPosition == std::string::npos)
        {
            throw std::runtime_error("WebGPU frame shader template has no model evaluator insertion point");
        }

        shader.replace(markerPosition, EVALUATOR_MARKER.size(), modelEvaluator);

        dumpShader(shader, "frame_compute_composed.wgsl");

        return shader;
    }
}
