#pragma once

#include "compute/types.h"

#include <cstdint>

namespace gladius
{
    class ResourceManager;

    namespace compute
    {
        class RenderBackendSession;
    }

    namespace nodes
    {
        class Assembly;
    }

    namespace webgpu
    {
        /**
         * @brief Renders a complete WebGPU frame and encodes it as a PNG thumbnail.
         */
        class WebGPUThumbnailRenderer
        {
          public:
            [[nodiscard]] static PlainImage renderPng(compute::RenderBackendSession & session,
                                                      nodes::Assembly & assembly,
                                                      ResourceManager const & resourceManager,
                                                      std::uint32_t size,
                                                      std::uint64_t sceneGeneration);
        };
    }
}