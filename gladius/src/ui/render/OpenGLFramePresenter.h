#pragma once

#include "FramePresenter.h"

#include <cstdint>
#include <optional>

namespace gladius::ui::async_rendering
{
    /**
     * @brief UI-thread OpenGL presentation bridge for backend-neutral CPU RGBA frames.
     *
     * A frame can represent a progressive row range. The presenter allocates an RGBA8 texture
     * for the complete viewport and uploads only the completed rows. Every method that creates,
     * changes, or releases the texture must execute while the UI OpenGL context is current.
     */
    class OpenGLFramePresenter final : public FramePresenter
    {
      public:
        OpenGLFramePresenter() = default;
        ~OpenGLFramePresenter() override;

        OpenGLFramePresenter(OpenGLFramePresenter const &) = delete;
        OpenGLFramePresenter & operator=(OpenGLFramePresenter const &) = delete;

        [[nodiscard]] static bool canPresent(compute::RenderFrame const & frame) noexcept;
        [[nodiscard]] bool present(compute::RenderFrame const & frame) override;
        void release() noexcept override;

        [[nodiscard]] std::uintptr_t getTextureId() const noexcept override;
        [[nodiscard]] std::uint32_t getWidth() const noexcept override;
        [[nodiscard]] std::uint32_t getHeight() const noexcept override;
        [[nodiscard]] std::optional<compute::RenderFreshnessStamp> getFreshness() const noexcept override;

      private:
        void allocateTexture(std::uint32_t width, std::uint32_t height);

        std::uint32_t m_textureId{};
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::optional<compute::RenderFreshnessStamp> m_freshness;
    };
}
