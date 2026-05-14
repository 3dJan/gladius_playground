#include "ui/render/AsyncRenderController.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    TEST(AsyncRenderControllerPresentation, PublishedFrame_CanBePromotedToFront)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

        controller.publishFrame(buffer, 2, 10, 3);
        auto * promoted = controller.promoteReadyToFront();
        ASSERT_EQ(promoted, buffer);

        EXPECT_TRUE(controller.finalizeFrontPromotion(promoted));
        EXPECT_EQ(controller.frontBuffer(), buffer);
        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Front);
        EXPECT_EQ(buffer->epoch.load(std::memory_order_acquire), 10u);
        EXPECT_EQ(buffer->viewEpoch.load(std::memory_order_acquire), 3u);
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

        controller.publishFrame(buffer, 2, 10, 3);
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

        controller.publishFrame(older, 5, 10, 3);
        controller.publishFrame(newer, 6, 10, 3);

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
        controller.publishFrame(buffer, 2, 10, 3);

        controller.discardReadyFrame(2, 10, 3);

        EXPECT_EQ(buffer->state.load(std::memory_order_acquire), FrameState::Idle);
        auto * reacquired = controller.acquireWriteBuffer(11);
        EXPECT_EQ(reacquired, buffer);
    }

    TEST(AsyncRenderControllerPresentation, ReleaseStaleBuffers_WithWritingBuffer_ReleasesIt)
    {
        AsyncRenderController controller;

        auto * buffer = controller.acquireWriteBuffer(10);
        ASSERT_NE(buffer, nullptr);

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
}
