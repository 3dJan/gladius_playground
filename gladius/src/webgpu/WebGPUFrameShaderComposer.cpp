#include "webgpu/WebGPUFrameShaderComposer.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

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
