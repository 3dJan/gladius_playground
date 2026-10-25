#include "ui/render/PreviewBackendPolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(PreviewBackendPolicy, WithValidSdf_UsesPrecomputedSdfBackend)
    {
        auto const backend = choosePreviewBackend(
          PreviewBackendInput{.rendererReady = true,
                              .lowResTargetAvailable = true,
                              .precomputedSdfValid = true,
                              .allowDynamicFullModelFallback = true});

        EXPECT_EQ(backend, PreviewBackend::PrecomputedSdfWithDistanceInit);
        EXPECT_TRUE(previewBackendProducesDistanceInit(backend));
    }

    TEST(PreviewBackendPolicy, WithInvalidSdfAndFallbackAllowed_UsesDynamicFullModel)
    {
        auto const backend = choosePreviewBackend(
          PreviewBackendInput{.rendererReady = true,
                              .lowResTargetAvailable = true,
                              .precomputedSdfValid = false,
                              .allowDynamicFullModelFallback = true});

        EXPECT_EQ(backend, PreviewBackend::DynamicFullModel);
        EXPECT_FALSE(previewBackendProducesDistanceInit(backend));
    }

    TEST(PreviewBackendPolicy, WithInvalidSdfAndFallbackDisabled_ReturnsNone)
    {
        auto const backend = choosePreviewBackend(
          PreviewBackendInput{.rendererReady = true,
                              .lowResTargetAvailable = true,
                              .precomputedSdfValid = false,
                              .allowDynamicFullModelFallback = false});

        EXPECT_EQ(backend, PreviewBackend::None);
    }

    TEST(PreviewBackendPolicy, WithoutRendererReady_ReturnsNone)
    {
        auto const backend = choosePreviewBackend(
          PreviewBackendInput{.rendererReady = false,
                              .lowResTargetAvailable = true,
                              .precomputedSdfValid = true,
                              .allowDynamicFullModelFallback = true});

        EXPECT_EQ(backend, PreviewBackend::None);
    }

    TEST(PreviewBackendPolicy, WithoutLowResTarget_ReturnsNone)
    {
        auto const backend = choosePreviewBackend(
          PreviewBackendInput{.rendererReady = true,
                              .lowResTargetAvailable = false,
                              .precomputedSdfValid = true,
                              .allowDynamicFullModelFallback = true});

        EXPECT_EQ(backend, PreviewBackend::None);
    }
}
