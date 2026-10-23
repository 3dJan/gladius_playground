#pragma once

#include "compute/RenderContracts.h"
#include "compute/types.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace gladius
{
    class ResourceManager;

    namespace nodes
    {
        class Assembly;
    }

    namespace webgpu
    {
        class WebGPUComputeContext;

        /**
         * @brief Renders a complete WebGPU frame and encodes it as a PNG thumbnail.
         */
        class WebGPUThumbnailRenderer
        {
          public:
            [[nodiscard]] static PlainImage renderPng(
              std::shared_ptr<WebGPUComputeContext> context,
              nodes::Assembly & assembly,
              ResourceManager const & resourceManager,
              std::uint32_t width,
              std::uint32_t height,
              std::uint64_t sceneGeneration,
              std::optional<compute::RenderCamera> camera = {},
              compute::RenderSettingsSnapshot settings = {});
        };
    }
}