#pragma once

#include "FramePresenter.h"

#include <memory>

#include <webgpu/webgpu_cpp.h>

namespace gladius::webgpu
{
  class WebGPUComputeContext;
}

namespace gladius::ui::async_rendering
{
    /**
     * @brief WebGPU presentation bridge for backend-neutral CPU RGBA frames.
     *
     * Owns the sampled texture used by ImGui for backend-neutral CPU RGBA frames.
     * A single texture is retained across progressive row uploads so unfinished rows
     * remain visible while the renderer produces the next frame chunk.
     */
    class WebGPUFramePresenter final : public FramePresenter
    {
      public:
        explicit WebGPUFramePresenter(std::shared_ptr<webgpu::WebGPUComputeContext> context);
        ~WebGPUFramePresenter() override;

        WebGPUFramePresenter(WebGPUFramePresenter const &) = delete;
        WebGPUFramePresenter & operator=(WebGPUFramePresenter const &) = delete;

        [[nodiscard]] bool present(compute::RenderFrame const & frame) override;
        void release() noexcept override;
        [[nodiscard]] std::uintptr_t getTextureId() const noexcept override;
        [[nodiscard]] std::uint32_t getWidth() const noexcept override;
        [[nodiscard]] std::uint32_t getHeight() const noexcept override;
        [[nodiscard]] std::optional<compute::RenderFreshnessStamp> getFreshness() const noexcept override;

      private:
        void allocateTexture(std::uint32_t width, std::uint32_t height);

        std::shared_ptr<webgpu::WebGPUComputeContext> m_context;
        wgpu::Texture m_texture;
        wgpu::TextureView m_textureView;
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::optional<compute::RenderFreshnessStamp> m_freshness;
    };
}
