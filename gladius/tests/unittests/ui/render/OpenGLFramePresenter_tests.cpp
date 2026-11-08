#include "ui/render/OpenGLFramePresenter.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(OpenGLFramePresenter, CanPresent_WithCompleteProgressiveFrame_ReturnsTrue)
    {
        compute::RenderFrame const frame{.width = 7u,
                                         .height = 9u,
                                         .firstRow = 2u,
                                         .endRow = 5u,
                                         .pixels = std::vector<std::uint32_t>(7u * 3u)};

        EXPECT_TRUE(OpenGLFramePresenter::canPresent(frame));
    }

    TEST(OpenGLFramePresenter, CanPresent_WithMismatchedPixels_ReturnsFalse)
    {
        compute::RenderFrame const frame{.width = 7u,
                                         .height = 9u,
                                         .firstRow = 2u,
                                         .endRow = 5u,
                                         .pixels = std::vector<std::uint32_t>(7u * 2u)};

        EXPECT_FALSE(OpenGLFramePresenter::canPresent(frame));
    }

    TEST(OpenGLFramePresenter, Present_WithoutPreviewSeed_DoesNotAllocateOrExposePartialTexture)
    {
        OpenGLFramePresenter presenter;
        compute::RenderFrame const frame{.width = 4u,
                                         .height = 4u,
                                         .firstRow = 0u,
                                         .endRow = 1u,
                                         .pixels = std::vector<std::uint32_t>(4u, 0xFFFFFFFFu)};
        FramePresentationCandidate const candidate{
          .quality = FramePresentationQuality::ProgressivePartial,
          .source = FramePresentationSource::ProgressiveHighQuality};

        EXPECT_FALSE(presenter.present(frame, candidate));
        EXPECT_EQ(presenter.getTextureId(), 0u);
    }
}