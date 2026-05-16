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

        TEST(FramePresentationController, PublishFrame_WithExplicitQuality_StoresMetadata)
        {
                FramePresentationController controller{3};
                auto const stamp = makeStamp(1, 2, 3, 4, 5);
                auto const bufferId = controller.acquireWriteBuffer(stamp);
                ASSERT_TRUE(bufferId.has_value());

                EXPECT_TRUE(controller.publishFrame(
                    *bufferId, 17, stamp, FramePresentationQuality::ProgressivePartial));

                ASSERT_NE(controller.buffer(*bufferId), nullptr);
                EXPECT_EQ(controller.buffer(*bufferId)->quality,
                                    FramePresentationQuality::ProgressivePartial);
        }

    TEST(FramePresentationController, AcquireWriteBuffer_WithNoIdleBuffer_ReturnsNullopt)
    {
        FramePresentationController controller{2};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);

        auto const firstWrite = controller.acquireWriteBuffer(stamp);
        auto const secondWrite = controller.acquireWriteBuffer(stamp);

        ASSERT_TRUE(firstWrite.has_value());
        EXPECT_FALSE(secondWrite.has_value());
    }

    TEST(FramePresentationController, PublishFrame_WithNonWritingBuffer_ReturnsFalse)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);

        EXPECT_FALSE(controller.publishFrame(*controller.frontBufferId(), 17, stamp));
        EXPECT_EQ(controller.buffer(*controller.frontBufferId())->state, FrameState::Front);
    }

    TEST(FramePresentationController, TryTransitionBuffer_WithExpectedState_ReleasesWritingBuffer)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(stamp);
        ASSERT_TRUE(bufferId.has_value());

        EXPECT_TRUE(controller.tryTransitionBuffer(*bufferId, FrameState::Writing, FrameState::Idle));

        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Idle);
        EXPECT_EQ(controller.buffer(*bufferId)->frameId, 0u);
    }

    TEST(FramePresentationController, TryTransitionBuffer_WithWrongExpectedState_ReturnsFalse)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(stamp);
        ASSERT_TRUE(bufferId.has_value());

        EXPECT_FALSE(controller.tryTransitionBuffer(*bufferId, FrameState::Ready, FrameState::Idle));
        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Writing);
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

    TEST(FramePresentationController, SelectNewestReady_WithoutFreshnessMask_SelectsNewestRegardlessOfStamp)
    {
        FramePresentationController controller{4};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const oldView = makeStamp(1, 2, 2, 4, 5);
        auto const currentReady = controller.acquireWriteBuffer(latest);
        auto const staleButNewerReady = controller.acquireWriteBuffer(oldView);
        ASSERT_TRUE(currentReady.has_value());
        ASSERT_TRUE(staleButNewerReady.has_value());
        ASSERT_TRUE(controller.publishFrame(*currentReady, 10, latest));
        ASSERT_TRUE(controller.publishFrame(*staleButNewerReady, 11, oldView));

        auto const selected = controller.selectNewestReady();

        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(*selected, *staleButNewerReady);
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

        TEST(FramePresentationController, SelectBestReadyForPresentation_WithCurrentFullQuality_SkipsPreviewRegression)
        {
                FramePresentationController controller{3};
                auto const current = makeStamp(1, 2, 3, 4, 5);

                auto const fullQuality = controller.acquireWriteBuffer(current);
                ASSERT_TRUE(fullQuality.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *fullQuality, 10, current, FramePresentationQuality::FullQuality));
                auto const promoted =
                    controller.selectNewestReady(current, RenderStampMask::displayFrame());
                ASSERT_TRUE(promoted.has_value());
                ASSERT_TRUE(controller.finalizeFrontPromotion(*promoted));

                auto const preview = controller.acquireWriteBuffer(current);
                ASSERT_TRUE(preview.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *preview, 11, current, FramePresentationQuality::Preview));

                auto const selected =
                    controller.selectBestReadyForPresentation(current, RenderStampMask::displayFrame());

                EXPECT_FALSE(selected.has_value());
                EXPECT_EQ(controller.buffer(*preview)->state, FrameState::Ready);
        }

        TEST(FramePresentationController, SelectBestReadyForPresentation_WithStaleFront_AllowsFreshPreview)
        {
                FramePresentationController controller{3};
                auto const oldStamp = makeStamp(1, 2, 2, 4, 5);
                auto const latest = makeStamp(1, 2, 3, 4, 5);

                auto const oldFullQuality = controller.acquireWriteBuffer(oldStamp);
                ASSERT_TRUE(oldFullQuality.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *oldFullQuality, 10, oldStamp, FramePresentationQuality::FullQuality));
                auto const promoted =
                    controller.selectNewestReady(oldStamp, RenderStampMask::displayFrame());
                ASSERT_TRUE(promoted.has_value());
                ASSERT_TRUE(controller.finalizeFrontPromotion(*promoted));

                auto const preview = controller.acquireWriteBuffer(latest);
                ASSERT_TRUE(preview.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *preview, 11, latest, FramePresentationQuality::Preview));

                auto const selected =
                    controller.selectBestReadyForPresentation(latest, RenderStampMask::displayFrame());

                ASSERT_TRUE(selected.has_value());
                EXPECT_EQ(*selected, *preview);
                EXPECT_EQ(controller.buffer(*selected)->state, FrameState::Resampling);
        }

        TEST(FramePresentationController, SelectBestReadyForPresentation_PrefersHigherQualityOverNewerPreview)
        {
                FramePresentationController controller{4};
                auto const latest = makeStamp(1, 2, 3, 4, 5);

                auto const fullQuality = controller.acquireWriteBuffer(latest);
                auto const preview = controller.acquireWriteBuffer(latest);
                ASSERT_TRUE(fullQuality.has_value());
                ASSERT_TRUE(preview.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *fullQuality, 10, latest, FramePresentationQuality::FullQuality));
                ASSERT_TRUE(controller.publishFrame(
                    *preview, 11, latest, FramePresentationQuality::Preview));

                auto const selected = controller.selectBestReadyForPresentation(
                    latest, RenderStampMask::displayFrame());

                ASSERT_TRUE(selected.has_value());
                EXPECT_EQ(*selected, *fullQuality);
                EXPECT_EQ(controller.buffer(*selected)->state, FrameState::Resampling);
                EXPECT_EQ(controller.buffer(*preview)->state, FrameState::Ready);
        }

        TEST(FramePresentationController, CanPresentCandidate_WithCurrentFullQuality_RejectsPreview)
        {
                FramePresentationController controller{3};
                auto const current = makeStamp(1, 2, 3, 4, 5);

                auto const fullQuality = controller.acquireWriteBuffer(current);
                ASSERT_TRUE(fullQuality.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *fullQuality, 10, current, FramePresentationQuality::FullQuality));
                auto const promoted = controller.selectBestReadyForPresentation(
                    current, RenderStampMask::displayFrame());
                ASSERT_TRUE(promoted.has_value());
                ASSERT_TRUE(controller.finalizeFrontPromotion(*promoted));

                EXPECT_FALSE(controller.canPresentCandidate(
                    FramePresentationCandidate{.frameId = 11,
                                               .stamp = current,
                                               .quality = FramePresentationQuality::Preview},
                    current,
                    RenderStampMask::displayFrame()));
        }

        TEST(FramePresentationController, CanPresentCandidate_WithStaleFront_AllowsFreshPreview)
        {
                FramePresentationController controller{3};
                auto const oldStamp = makeStamp(1, 2, 2, 4, 5);
                auto const latest = makeStamp(1, 2, 3, 4, 5);

                auto const oldFullQuality = controller.acquireWriteBuffer(oldStamp);
                ASSERT_TRUE(oldFullQuality.has_value());
                ASSERT_TRUE(controller.publishFrame(
                    *oldFullQuality, 10, oldStamp, FramePresentationQuality::FullQuality));
                auto const promoted = controller.selectBestReadyForPresentation(
                    oldStamp, RenderStampMask::displayFrame());
                ASSERT_TRUE(promoted.has_value());
                ASSERT_TRUE(controller.finalizeFrontPromotion(*promoted));

                EXPECT_TRUE(controller.canPresentCandidate(
                    FramePresentationCandidate{.frameId = 11,
                                               .stamp = latest,
                                               .quality = FramePresentationQuality::Preview},
                    latest,
                    RenderStampMask::displayFrame()));
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

    TEST(FramePresentationController, FinalizeFrontPromotion_WithWrongState_ReturnsFalseAndKeepsFront)
    {
        FramePresentationController controller{3};
        auto const stamp = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(stamp);
        ASSERT_TRUE(bufferId.has_value());
        auto const originalFront = controller.frontBufferId();

        EXPECT_FALSE(controller.finalizeFrontPromotion(*bufferId));

        EXPECT_EQ(controller.frontBufferId(), originalFront);
        EXPECT_EQ(controller.buffer(*bufferId)->state, FrameState::Writing);
    }

    TEST(FramePresentationController, FinalizeFrontPromotion_AfterPromotion_AllowsOldFrontReuse)
    {
        FramePresentationController controller{3};
        auto const latest = makeStamp(1, 2, 3, 4, 5);
        auto const bufferId = controller.acquireWriteBuffer(latest);
        ASSERT_TRUE(bufferId.has_value());
        ASSERT_TRUE(controller.publishFrame(*bufferId, 12, latest));
        auto const selected = controller.selectNewestReady(latest, RenderStampMask::displayFrame());
        ASSERT_TRUE(selected.has_value());
        ASSERT_TRUE(controller.finalizeFrontPromotion(*selected));

        auto const nextWrite = controller.acquireWriteBuffer(latest);

        ASSERT_TRUE(nextWrite.has_value());
        EXPECT_EQ(*nextWrite, 0u);
        EXPECT_EQ(controller.buffer(*nextWrite)->state, FrameState::Writing);
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
