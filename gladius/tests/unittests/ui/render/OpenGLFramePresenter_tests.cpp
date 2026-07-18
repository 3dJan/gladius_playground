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
}