#pragma once

#include "compute/IComputeBackend.h"
#include "compute/RenderSceneSnapshot.h"

#include <cstdint>

namespace gladius::nodes
{
  class Assembly;
    class Model;
}

namespace gladius::webgpu
{
    /**
     * @brief Builds a WebGPU slice request directly from a supported Gladius model.
     */
    class WebGPUModelSliceRequestFactory
    {
      public:
        [[nodiscard]] static compute::SliceRequest create(nodes::Model & model,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           float sliceZ,
                                                           float scale);

        /**
         * @brief Builds a request from an assembly after lowering function calls into its root model.
         */
        [[nodiscard]] static compute::SliceRequest create(nodes::Assembly const & assembly,
                       std::uint32_t width,
                       std::uint32_t height,
                       float sliceZ,
                       float scale);

        /**
         * @brief Builds a headless WebGPU frame request directly from a supported Gladius model.
         */
        [[nodiscard]] static compute::FrameRequest createFrame(nodes::Model & model,
                                    compute::FrameRequest frameRequest);

        /**
         * @brief Builds a frame request from an assembly after lowering function calls into its root model.
         */
        [[nodiscard]] static compute::FrameRequest createFrame(nodes::Assembly const & assembly,
                                    compute::FrameRequest frameRequest);

        /// @brief Builds an immutable analytic scene snapshot from a supported model.
        [[nodiscard]] static compute::RenderSceneSnapshot createScene(nodes::Model & model,
                                         std::uint64_t sceneGeneration);

        /// @brief Builds an immutable analytic scene snapshot after flattening assembly calls.
        [[nodiscard]] static compute::RenderSceneSnapshot createScene(nodes::Assembly const & assembly,
                                         std::uint64_t sceneGeneration);
    };
}
