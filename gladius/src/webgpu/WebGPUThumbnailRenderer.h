#pragma once

#include "compute/AsyncTask.h"
#include "compute/RenderContracts.h"
#include "compute/RenderSceneSnapshot.h"
#include "compute/types.h"

#include <cstdint>
#include <functional>
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

            /// @brief Render a PNG from an existing snapshot and bounds, pumping WebGPU events.
            [[nodiscard]] static PlainImage renderPng(
              std::shared_ptr<WebGPUComputeContext> context,
              std::shared_ptr<const compute::RenderSceneSnapshot> sceneSnapshot,
              compute::RenderBounds modelBounds,
              std::uint32_t width,
              std::uint32_t height,
              std::function<bool()> shouldCancel = {});

            /// @brief Render a PNG from an existing immutable export snapshot and known bounds.
            ///
            /// The returned task yields while the GPU submission is pending, allowing browser
            /// mesh-export jobs to advance it without blocking the event loop.
            [[nodiscard]] static compute::AsyncTask<PlainImage> renderPngAsync(
              std::shared_ptr<WebGPUComputeContext> context,
              std::shared_ptr<const compute::RenderSceneSnapshot> sceneSnapshot,
              compute::RenderBounds modelBounds,
              std::uint32_t width,
              std::uint32_t height,
              compute::RenderSettingsSnapshot settings = {},
              std::function<bool()> shouldCancel = {});
        };
    }
}