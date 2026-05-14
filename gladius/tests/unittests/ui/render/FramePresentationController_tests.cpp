#include "ui/render/FramePresentationController.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] constexpr RenderStamp makeStamp(uint64_t scene,
                                                      uint64_t parameters,
                                                      uint64_t view,
                                                      uint64_t viewport,
                                                      uint64_t quality) noexcept
        {
            return RenderStamp{.sceneEpoch = scene,
                               .parameterEpoch = parameters,
                               .viewEpoch = view,
                               .viewportEpoch = viewport,
                               .qualityEpoch = quality};
        }
    }

    TEST(FramePresentationController, AfterReset_HasFrontBufferAndIdleBackBuffers)
    {
        FramePresentationController controller{3};

        ASSERT_TRUE(controller.frontBufferId().has_value());
        EXPECT_EQ(*controller.frontBufferId(), 0u);
        ASSERT_NE(controller.buffer(0), nullptr);
        ASSERT_NE(controller.buffer(1), nullptr);
        ASSERT_NE(controller.buffer(2), nullptr);
        EXPECT_EQ(controller.buffer(0)->state, FrameState::Front);
        EXPECT_EQ(controller.buffer(1)->state, FrameState::Idle);
        EXPECT_EQ(controller.buffer(2)->state, FrameState::Idle);
    }

    TEST(FramePresentationController, AcquireWriteBuffer_WithIdleBuffer_MarksBufferWriting)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);

        auto const bufferId = controller.acquireWriteBuffer(stamp);

        ASSERT_TRUE(bufferId.has_value());
        ASSERT_NE(controller.buffer(*bufferId), nullptr);
        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Writing);
        EXPECT_TRUE(matches(controller.buffer(*bufferId)->stamp, stamp));
    }

    TEST(FramePresentationController, PublishFrame_WithWritingBuffer_MarksBufferReady)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(stamp);
        ASSERT_TRUE(bufferId.has_value());

        EXPECT_TRUE(controller.publishFrame(*bufferId, 17, stamp));

        ASSERT_NE(controller.buffer(*bufferId), nullptr);
        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Ready);
        EXPECT_EQ(controller.buffer(*bufferId)->frameId, 17u);
    }

    TEST(FramePresentationController, SelectNewestReady_WithMultipleReadyBuffers_SelectsHighestFrameId)
    {
        FramePresentationController controller{4};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const olderReady = controller.acquireWriteBuffer(latest);
        auto const newerReady = controller.acquireWriteBuffer(latest);
        ASSERT_TRUE(olderReady.has_value());
        ASSERT_TRUE(newerReady.has_value());
        ASSERT_TRUE(controller.publishFrame(*olderReady, 10, latest));
        ASSERT_TRUE(controller.publishFrame(*newerReady, 11, latest));

        auto const selected = controller.selectNewestReady(latest, RenderStampMask::displayFrame());

        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(*selected, *newerReady);
        EXPECT_EQ(controller.buffer(*selected)->state, FrameState::Resampling);
        EXPECT_EQ(controller.buffer(*olderReady)->state, FrameState::Ready);
    }

    TEST(FramePresentationController, SelectNewestReady_WithStaleReadyBuffer_IgnoresStaleFrame)
    {
        FramePresentationController controller{3};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const oldCamera = makeStamp(1, 2, 2, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(oldCamera);
        ASSERT_TRUE(bufferId.has_value());
        ASSERT_TRUE(controller.publishFrame(*bufferId, 99, oldCamera));

        auto const selected = controller.selectNewestReady(latest, RenderStampMask::displayFrame());

        EXPECT_FALSE(selected.has_value());
        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Ready);
    }

    TEST(FramePresentationController, FinalizeFrontPromotion_WithSelectedBuffer_DemotesPreviousFront)
    {
        FramePresentationController controller{3};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(latest);
        ASSERT_TRUE(bufferId.has_value());
        ASSERT_TRUE(controller.publishFrame(*bufferId, 12, latest));
        auto const selected = controller.selectNewestReady(latest, RenderStampMask::displayFrame());
        ASSERT_TRUE(selected.has_value());

        EXPECT_TRUE(controller.finalizeFrontPromotion(*selected));

        ASSERT_TRUE(controller.frontBufferId().has_value());
        EXPECT_EQ(*controller.frontBufferId(), *selected);
        EXPECT_EQ(controller.buffer(*selected)->state, FrameState::Front);
        EXPECT_EQ(controller.buffer(0)->state, *selected == 0u ? FrameState::Front : FrameState::Idle);
    }

    TEST(FramePresentationController, DiscardReadyFrame_WithReadyBuffer_ReleasesItToIdle)
    {
        FramePresentationController controller{3};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(latest);
        ASSERT_TRUE(bufferId.has_value());
        ASSERT_TRUE(controller.publishFrame(*bufferId, 12, latest));

        EXPECT_TRUE(controller.discardReadyFrame(*bufferId));

        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Idle);
        EXPECT_EQ(controller.buffer(*bufferId)->frameId, 0u);
    }

    TEST(FramePresentationController, ReleaseStaleWritingBuffers_WithDifferentMasks_ReleasesRelevantStaleWork)
    {
        FramePresentationController controller{4};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const oldView = makeStamp(1, 2, 2, 4, 5);
        auto const oldScene = makeStamp(0, 2, 3, 4, 5);
        auto const oldViewBuffer = controller.acquireWriteBuffer(oldView);
        auto const oldSceneBuffer = controller.acquireWriteBuffer(oldScene);
        ASSERT_TRUE(oldViewBuffer.has_value());
        ASSERT_TRUE(oldSceneBuffer.has_value());

        EXPECT_EQ(controller.releaseStaleWritingBuffers(latest, RenderStampMask::heavyGeometryTask()), 1u);
        EXPECT_EQ(controller.buffer(*oldViewBuffer)->state, FrameState::Writing);
        EXPECT_EQ(controller.buffer(*oldSceneBuffer)->state, FrameState::Idle);

        EXPECT_EQ(controller.releaseStaleWritingBuffers(latest, RenderStampMask::displayFrame()), 1u);
        EXPECT_EQ(controller.buffer(*oldViewBuffer)->state, FrameState::Idle);
    }
}
