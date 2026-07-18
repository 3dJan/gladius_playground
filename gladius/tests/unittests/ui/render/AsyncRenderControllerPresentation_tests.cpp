#include "ui/render/AsyncRenderController.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(AsyncRenderControllerPresentation, PublishedFrame_CanBePromotedToFront)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

                controller.publishFrame(
                    buffer, 2, 10, 3, FramePresentationQuality::FullQuality);
        auto * promoted = controller.promoteReadyToFront();
        ASSERT_EQ(promoted, buffer);

        EXPECT_TRUE(controller.finalizeFrontPromotion(promoted));
        EXPECT_EQ(controller.frontBuffer(), buffer);
        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Front);
        EXPECT_EQ(buffer->epoch.load(std::memory_order_acquire), 10u);
        EXPECT_EQ(buffer->viewEpoch.load(std::memory_order_acquire), 3u);

                auto const mirroredFront = controller.mirroredFrontPresentationBuffer();
                ASSERT_TRUE(mirroredFront.has_value());
                EXPECT_EQ(mirroredFront->quality, FramePresentationQuality::FullQuality);
    }

    TEST(AsyncRenderControllerPresentation,
         FinalizeFrontPromotion_WithExistingFront_DemotesPreviousFrontForReuse)
    {
        AsyncRenderController controller;

        auto * initialFront = controller.frontBuffer();
        ASSERT_NE(initialFront, nullptr);
        ASSERT_TRUE(controller.tryTransitionBuffer(initialFront, FrameState::Idle, FrameState::Front));

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);
        ASSERT_NE(buffer, initialFront);

                controller.publishFrame(
                    buffer, 2, 10, 3, FramePresentationQuality::FullQuality);
        auto * promoted = controller.promoteReadyToFront();
        ASSERT_EQ(promoted, buffer);
        ASSERT_TRUE(controller.finalizeFrontPromotion(promoted));

        EXPECT_EQ(controller.frontBuffer(), buffer);
        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Front);
        EXPECT_EQ(initialFront->state.load(std::memory_order_acquire), FrameState::Idle);

        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_EQ(reacquired, initialFront);
    }

    TEST(AsyncRenderControllerPresentation, PromoteReadyToFront_SelectsNewestFrame)
    {
        AsyncRenderController controller;

        auto * older = controller.acquireWriteBuffer(10);
        auto * newer = controller.acquireWriteBuffer(10);
        ASSERT_NE(older, nullptr);
        ASSERT_NE(newer, nullptr);
        ASSERT_NE(older, newer);

                controller.publishFrame(
                    older, 5, 10, 3, FramePresentationQuality::FullQuality);
                controller.publishFrame(
                    newer, 6, 10, 3, FramePresentationQuality::FullQuality);

        auto * promoted = controller.promoteReadyToFront();

        EXPECT_EQ(promoted, newer);
        EXPECT_EQ(newer->state.load(std::memory_order_acquire), FrameState::Resampling);
        EXPECT_EQ(older->state.load(std::memory_order_acquire), FrameState::Ready);
    }

    TEST(AsyncRenderControllerPresentation, DiscardReadyFrame_WithMatchingMetadata_ReleasesBuffer)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);
                controller.publishFrame(
                    buffer, 2, 10, 3, FramePresentationQuality::FullQuality);

        controller.discardReadyFrame(2, 10, 3);

        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Idle);
        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_EQ(reacquired, buffer);
    }

    TEST(AsyncRenderControllerPresentation, ReleaseStaleBuffers_WithWritingBuffer_KeepsItReserved)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

        controller.releaseStaleBuffers(10);

        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Writing);
        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_NE(reacquired, buffer);
    }

    TEST(AsyncRenderControllerPresentation, ReleaseStaleBuffers_WithReadyBuffer_ReleasesIt)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);
        controller.publishFrame(buffer, 2, 10, 3, FramePresentationQuality::FullQuality);

        controller.releaseStaleBuffers(10);

        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Idle);
        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_EQ(reacquired, buffer);
    }

    TEST(AsyncRenderControllerPresentation, TryTransitionBuffer_WithWritingBuffer_KeepsMirrorReusable)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

        EXPECT_TRUE(controller.tryTransitionBuffer(buffer, FrameState::Writing, FrameState::Idle));

        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_EQ(reacquired, buffer);
    }

    TEST(AsyncRenderControllerPresentation, PublishFrame_MirrorsQualityMetadataWhileReady)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

        controller.publishFrame(
          buffer, 2, 10, 3, FramePresentationQuality::ProgressivePartial);

        auto const mirrored = controller.mirroredPresentationBuffer(buffer);
        ASSERT_TRUE(mirrored.has_value());
        EXPECT_EQ(mirrored->state, FrameState::Ready);
        EXPECT_EQ(mirrored->quality, FramePresentationQuality::ProgressivePartial);
        EXPECT_EQ(mirrored->frameId, 2u);
    }

    TEST(AsyncRenderControllerPresentation, PublishFrame_WithFullStamp_MirrorsStampMetadata)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

        RenderStamp const stamp{.sceneEpoch = 1,
                                .parameterEpoch = 2,
                                .viewEpoch = 3,
                                .viewportEpoch = 4,
                                .qualityEpoch = 5};
        controller.publishFrame(buffer, 2, 10, 3, stamp, FramePresentationQuality::FullQuality);

        auto const mirrored = controller.mirroredPresentationBuffer(buffer);
        ASSERT_TRUE(mirrored.has_value());
        EXPECT_TRUE(matches(mirrored->stamp, stamp, RenderStampMask::displayFrame()));
        EXPECT_EQ(mirrored->quality, FramePresentationQuality::FullQuality);
    }

    TEST(AsyncRenderControllerPresentation,
         PromoteReadyToFront_WithCurrentFullQuality_SkipsPreviewRegression)
    {
        AsyncRenderController controller;
        RenderStamp const stamp{.sceneEpoch = 1,
                                .parameterEpoch = 2,
                                .viewEpoch = 3,
                                .viewportEpoch = 4,
                                .qualityEpoch = 5};

        auto * fullQuality = controller.acquireWriteBuffer(10);
        ASSERT_NE(fullQuality, nullptr);
        controller.publishFrame(fullQuality, 2, 10, 3, stamp, FramePresentationQuality::FullQuality);
        auto * promoted = controller.promoteReadyToFront(stamp, RenderStampMask::displayFrame());
        ASSERT_EQ(promoted, fullQuality);
        ASSERT_TRUE(controller.finalizeFrontPromotion(promoted));

        auto * preview = controller.acquireWriteBuffer(10);
        ASSERT_NE(preview, nullptr);
        controller.publishFrame(preview, 3, 10, 3, stamp, FramePresentationQuality::Preview);

        EXPECT_EQ(controller.promoteReadyToFront(stamp, RenderStampMask::displayFrame()), nullptr);
        EXPECT_EQ(preview->state.load(std::memory_order_acquire), FrameState::Ready);
        EXPECT_FALSE(controller.canPresentFrame(stamp,
                                               FramePresentationQuality::Preview,
                                               stamp,
                                               RenderStampMask::displayFrame()));
    }
}
