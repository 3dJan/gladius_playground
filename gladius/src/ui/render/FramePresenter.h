#pragma once

#include "FramePresentationTypes.h"
#include "compute/RenderContracts.h"

#include <array>
#include <cstdint>
#include <optional>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Backend-neutral presentation interface for completed CPU render frames.
     */
    class FramePresenter
    {
      public:
        virtual ~FramePresenter() = default;

        FramePresenter(FramePresenter const &) = delete;
        FramePresenter & operator=(FramePresenter const &) = delete;

        [[nodiscard]] virtual bool present(compute::RenderFrame const & frame,
                  FramePresentationCandidate const & candidate) = 0;
        [[nodiscard]] virtual bool supportsGpuDirectPresentation() const noexcept
        {
            return false;
        }
        virtual void beginFrame() noexcept {}
        virtual void release() noexcept = 0;
        [[nodiscard]] virtual std::uintptr_t getTextureId() const noexcept = 0;
        [[nodiscard]] virtual std::uint32_t getWidth() const noexcept = 0;
        [[nodiscard]] virtual std::uint32_t getHeight() const noexcept = 0;
        [[nodiscard]] virtual std::array<float, 2> getTextureUvMax() const noexcept
        {
            return {1.0f, 1.0f};
        }
        [[nodiscard]] virtual std::optional<compute::RenderFreshnessStamp> getFreshness() const noexcept = 0;

      protected:
        FramePresenter() = default;
    };
}
