#include "ui/render/RenderUpdateCoordinator.h"

#include <gtest/gtest.h>

#include <optional>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] std::optional<RenderTaskRequest> findStartedTask(RenderUpdateDecision const & decision,
                                                                       RenderTaskType type)
        {
            for (auto const & command : decision.commands)
            {
                if (command.type == RenderCommandType::StartTask && command.task.type == type)
                {
                    return command.task;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool hasCommand(RenderUpdateDecision const & decision, RenderCommandType type)
        {
            for (auto const & command : decision.commands)
            {
                if (command.type == type)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool hasStartedTask(RenderUpdateDecision const & decision, RenderTaskType type)
        {
            return findStartedTask(decision, type).has_value();
        }

        [[nodiscard]] RenderTaskResult completed(RenderTaskRequest const & request,
                                                 bool producedDisplayFrame = false,
                                                 bool completedFrame = false)
        {
            return RenderTaskResult{.requestId = request.requestId,
                                    .type = request.type,
                                    .stamp = request.stamp,
                                    .status = RenderTaskStatus::Completed,
                                    .durationNs = 200'000'000,
                                    .producedDisplayFrame = producedDisplayFrame,
                                    .completedFrame = completedFrame};
        }

        [[nodiscard]] RenderTaskResult failed(RenderTaskRequest const & request)
        {
            auto result = completed(request);
            result.status = RenderTaskStatus::Failed;
            return result;
        }

        [[nodiscard]] RealtimeRaymarchConfig forcedRealtimeConfig()
        {
            RealtimeRaymarchConfig config{};
            config.mode = RealtimeRaymarchMode::Force;
            return config;
        }

        [[nodiscard]] RealtimeRaymarchSample fullFrameSample(float durationMs)
        {
            return RealtimeRaymarchSample{.durationMs = durationMs,
                                          .width = 640,
                                          .height = 480,
                                          .renderedLines = 480,
                                          .totalLines = 480,
                                          .completedFrame = true,
                                          .cancelled = false};
        }

        /// Drives the coordinator through the static pipeline until a ProgressiveHighQualityChunk
        /// completes with a duration under the 100ms Auto-mode threshold.
        /// Call immediately after configureViewport(), passing its returned decision.
        void primeAutoRealtimeAdmission(RenderUpdateCoordinator & coordinator,
                                        RenderUpdateDecision const & viewportDecision)
        {
            auto bbox = findStartedTask(viewportDecision, RenderTaskType::BoundingBoxUpdate);
            ASSERT_TRUE(bbox.has_value());
            auto decision = coordinator.completeTask(completed(*bbox));
            auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
            ASSERT_TRUE(sdf.has_value());
            decision = coordinator.completeTask(completed(*sdf));
            auto hq = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
            ASSERT_TRUE(hq.has_value());
            auto hqResult = completed(*hq, true, true);
            hqResult.durationNs = 50'000'000; // 50ms < 100ms → autoModeAdmitsRealtime() = true
            [[maybe_unused]] auto const finalDecision = coordinator.completeTask(hqResult);
        }
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoLearning_StartsLowResolutionPreview)
    {
        RenderUpdateCoordinator coordinator;
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());
        auto const before = coordinator.latestStamp();

        auto const decision = coordinator.notifyCameraChanged();
        auto const after = coordinator.latestStamp();

        EXPECT_EQ(after.sceneEpoch, before.sceneEpoch);
        EXPECT_EQ(after.parameterEpoch, before.parameterEpoch);
        EXPECT_EQ(after.viewportEpoch, before.viewportEpoch);
        EXPECT_EQ(after.viewEpoch, before.viewEpoch + 1u);
        EXPECT_EQ(coordinator.interactionState(), RenderInteractionState::CameraInteracting);
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::SdfPrecomputation));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoPreviewInFlight_KeepsCurrentFrame)
    {
        RenderUpdateCoordinator coordinator;
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto decision = coordinator.notifyCameraChanged();
        ASSERT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));

        decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, StaticCatchUp_AfterFastProgressiveSample_StartsStaticFullFrame)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.configureViewport(640, 480);
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = coordinator.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());

        decision = coordinator.completeTask(completed(*sdf));
        auto progressive = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
        ASSERT_TRUE(progressive.has_value());

        coordinator.recordStaticProgressiveSample(fullFrameSample(40.0f));

        decision = coordinator.completeTask(completed(*progressive));

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithForcedRealtime_StartsRealtimeFullFrame)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto const decision = coordinator.notifyCameraChanged();

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithForcedRealtimeInFlight_KeepsCurrentFrameWithoutPreview)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto decision = coordinator.notifyCameraChanged();
        ASSERT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));

        decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoRealtimeAfterFastSamples_StartsRealtimeFullFrame)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);

        auto const decision = coordinator.notifyCameraChanged();

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoRealtimeInFlight_KeepsCurrentFrameWithoutPreview)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);

        auto decision = coordinator.notifyCameraChanged();
        ASSERT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));

        decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoRealtimeGuardBlocker_FallsBackToPreview)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);
        async_rendering::RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        coordinator.setRealtimeGuards(guards);

        auto const decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
        EXPECT_TRUE(coordinator.isAutoPreviewFallbackActive());
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithAutoPreviewFallbackLatch_StaysPreviewUntilGestureEnds)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);
        async_rendering::RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        coordinator.setRealtimeGuards(guards);

        auto decision = coordinator.notifyCameraChanged();
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());
        EXPECT_TRUE(coordinator.isAutoPreviewFallbackActive());

        coordinator.setRealtimeGuards(async_rendering::RealtimeRaymarchGuards{});
        decision = coordinator.completeTask(completed(*preview, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::PresentFrame));

        decision = coordinator.notifyCameraChanged();
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_TRUE(coordinator.isAutoPreviewFallbackActive());

        (void) coordinator.notifyCameraInteractionEnded();
        EXPECT_FALSE(coordinator.isAutoPreviewFallbackActive());
    }

    TEST(RenderUpdateCoordinator, CameraInteractionEnded_WithAutoRealtimeCurrentHq_KeepsCurrentFrame)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);

        auto decision = coordinator.notifyCameraChanged();
        auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(realtime.has_value());

        decision = coordinator.completeTask(completed(*realtime, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::PresentFrame));

        decision = coordinator.notifyCameraInteractionEnded();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, CameraInteractionEnded_WithStaleAutoRealtime_StartsHqNotPreview)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);

        auto decision = coordinator.notifyCameraChanged();
        auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(realtime.has_value());

        decision = coordinator.notifyCameraChanged();
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));

        decision = coordinator.completeTask(completed(*realtime, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.notifyCameraInteractionEnded();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithForcedRealtimeAndCurrentHq_EndsInteractionWithoutExtraHq)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto decision = coordinator.notifyCameraChanged();
        auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(realtime.has_value());

        decision = coordinator.completeTask(completed(*realtime, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::PresentFrame));

        decision = coordinator.notifyCameraInteractionEnded();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithForcedRealtimeGuardBlocker_KeepsCurrentFrame)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        async_rendering::RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        coordinator.setRealtimeGuards(guards);
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto const decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, CameraChanged_WithResizePendingEvenForcedRealtime_KeepsCurrentFrame)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        async_rendering::RealtimeRaymarchGuards guards{};
        guards.resizePending = true;
        coordinator.setRealtimeGuards(guards);
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto const decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, FailedForcedRealtime_DoesNotFallbackToPreviewWhileGuarded)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto decision = coordinator.notifyCameraChanged();
        auto realtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(realtime.has_value());

        decision = coordinator.completeTask(failed(*realtime));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        async_rendering::RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        coordinator.setRealtimeGuards(guards);
        decision = coordinator.tick();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, ParameterDrag_WithForcedRealtime_StartsRealtimeFullFrame)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto const decision = coordinator.notifyParameterChanged(true);

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, ParameterDrag_WithForcedRealtimeGuardBlocker_KeepsCurrentFrame)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());
        async_rendering::RealtimeRaymarchGuards guards{};
        guards.hardBlocker = true;
        coordinator.setRealtimeGuards(guards);

        auto const decision = coordinator.notifyParameterChanged(true);

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, ContinuousParameterDrag_WithForcedRealtimeInFlight_NeverStartsPreview)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.configureRealtime(forcedRealtimeConfig());
        ASSERT_FALSE(coordinator.configureViewport(640, 480).commands.empty());

        auto decision = coordinator.notifyParameterChanged(true);
        auto firstUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(firstUpload.has_value());
        auto firstRealtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(firstRealtime.has_value());

        decision = coordinator.notifyParameterChanged(true);
        auto const latestStamp = coordinator.latestStamp();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));

        decision = coordinator.completeTask(completed(*firstUpload));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.tick();
        auto latestUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(latestUpload.has_value());
        EXPECT_TRUE(matches(latestUpload->stamp, latestStamp, RenderStampMask::heavyGeometryTask()));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));

        decision = coordinator.completeTask(completed(*firstRealtime, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.completeTask(completed(*latestUpload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));

        decision = coordinator.tick();
        auto latestRealtime = findStartedTask(decision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(latestRealtime.has_value());
        EXPECT_TRUE(matches(latestRealtime->stamp, latestStamp, RenderStampMask::displayFrame()));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, ParameterDrag_StartsUploadAndPreviewButDefersHeavyTasks)
    {
        RenderUpdateCoordinator coordinator;
        auto const before = coordinator.latestStamp();

        auto const decision = coordinator.notifyParameterChanged(true);
        auto const after = coordinator.latestStamp();

        EXPECT_EQ(after.sceneEpoch, before.sceneEpoch);
        EXPECT_EQ(after.parameterEpoch, before.parameterEpoch + 1u);
        EXPECT_EQ(coordinator.interactionState(), RenderInteractionState::ParameterInteracting);
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::SdfPrecomputation));
    }

    TEST(RenderUpdateCoordinator, ParameterDrag_WithUploadInFlight_CoalescesToLatestUpload)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyParameterChanged(true);
        auto firstUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(firstUpload.has_value());

        decision = coordinator.notifyParameterChanged(true);
        auto const latest = coordinator.latestStamp();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));

        decision = coordinator.completeTask(completed(*firstUpload));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.tick();
        auto latestUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(latestUpload.has_value());
        EXPECT_TRUE(matches(latestUpload->stamp, latest, RenderStampMask::heavyGeometryTask()));
    }

    TEST(RenderUpdateCoordinator, ContinuousParameterDrag_WithStaleAutoPreview_CompletesLatestPreview)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyParameterChanged(true);
        auto firstUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(firstUpload.has_value());
        auto firstPreview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(firstPreview.has_value());

        decision = coordinator.notifyParameterChanged(true);
        auto const latestStamp = coordinator.latestStamp();
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));

        decision = coordinator.completeTask(completed(*firstUpload));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.completeTask(completed(*firstPreview, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.tick();
        auto latestUpload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(latestUpload.has_value());
        EXPECT_TRUE(matches(latestUpload->stamp, latestStamp, RenderStampMask::heavyGeometryTask()));
        auto latestPreview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(latestPreview.has_value());
        EXPECT_TRUE(matches(latestPreview->stamp, latestStamp, RenderStampMask::displayFrame()));

        decision = coordinator.completeTask(completed(*latestUpload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, ParameterChanged_AfterRealtimeLearning_ResetsToProgressiveStaticLearning)
    {
        RenderUpdateCoordinator coordinator;
        coordinator.recordStaticFullFrameSample(fullFrameSample(15.0f));
        coordinator.recordStaticFullFrameSample(fullFrameSample(16.0f));
        coordinator.recordStaticFullFrameSample(fullFrameSample(17.0f));
        ASSERT_TRUE(coordinator.isRealtimeActive());

        auto decision = coordinator.notifyParameterChanged(false);
        auto upload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(upload.has_value());
        EXPECT_FALSE(coordinator.isRealtimeActive());

        decision = coordinator.completeTask(completed(*upload));
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = coordinator.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());

        decision = coordinator.completeTask(completed(*sdf));

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
    }

    TEST(RenderUpdateCoordinator, ActiveParameterChanged_AfterFastAutoSample_ResetsInteractionAdmission)
    {
        RenderUpdateCoordinator coordinator;
        auto const viewportDecision = coordinator.configureViewport(640, 480);
        ASSERT_FALSE(viewportDecision.commands.empty());
        primeAutoRealtimeAdmission(coordinator, viewportDecision);

        auto decision = coordinator.notifyParameterChanged(true);
        auto upload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(upload.has_value());
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());

        [[maybe_unused]] auto const uploadDecision = coordinator.completeTask(completed(*upload));
        [[maybe_unused]] auto const previewDecision = coordinator.completeTask(completed(*preview, true, true));

        decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::RealtimeFullFrame));
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
    }

    TEST(RenderUpdateCoordinator, StaticCatchUp_AfterParameterEdit_SequencesUploadBboxSdfThenHq)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyParameterChanged(true);
        auto upload = findStartedTask(decision, RenderTaskType::ParameterUpload);
        ASSERT_TRUE(upload.has_value());

        decision = coordinator.completeTask(completed(*upload));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));

        decision = coordinator.notifyParameterInteractionEnded();
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));

        decision = coordinator.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());
        EXPECT_TRUE(coordinator.isBoundingBoxCurrent());
        EXPECT_FALSE(coordinator.isHeavyGeometryCurrent());

        decision = coordinator.completeTask(completed(*sdf));
        auto hq = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
        ASSERT_TRUE(hq.has_value());
        EXPECT_TRUE(coordinator.isHeavyGeometryCurrent());
        EXPECT_TRUE(matches(hq->stamp, coordinator.latestStamp(), RenderStampMask::displayFrame()));
    }

    TEST(RenderUpdateCoordinator, CameraBackpressure_KeepsCurrentFrameUntilOldPreviewCompletes)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyCameraChanged();
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());

        decision = coordinator.notifyCameraChanged();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::LowResolutionPreview));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));

        decision = coordinator.completeTask(completed(*preview, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));

        decision = coordinator.tick();
        auto latestPreview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(latestPreview.has_value());
        EXPECT_TRUE(matches(latestPreview->stamp, coordinator.latestStamp(), RenderStampMask::displayFrame()));
    }

    TEST(RenderUpdateCoordinator, CameraInteractionEnded_WithCurrentHeavyGeometry_StartsHqForLatestView)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.tick();
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = coordinator.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());

        decision = coordinator.completeTask(completed(*sdf));
        auto initialHq = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
        ASSERT_TRUE(initialHq.has_value());
        ASSERT_TRUE(coordinator.isHeavyGeometryCurrent());
        decision = coordinator.completeTask(completed(*initialHq, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::PresentFrame));

        decision = coordinator.notifyCameraChanged();
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::SdfPrecomputation));

        decision = coordinator.completeTask(completed(*preview, true, true));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::PresentFrame));

        decision = coordinator.notifyCameraInteractionEnded();
        auto hq = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
        ASSERT_TRUE(hq.has_value());
        EXPECT_TRUE(coordinator.isHeavyGeometryCurrent());
        EXPECT_TRUE(matches(hq->stamp, coordinator.latestStamp(), RenderStampMask::displayFrame()));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::BoundingBoxUpdate));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::SdfPrecomputation));
    }

    TEST(RenderUpdateCoordinator, StructuralModelChanged_StaleDisplayResultIsDiscarded)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyCameraChanged();
        auto preview = findStartedTask(decision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(preview.has_value());

        decision = coordinator.notifyStructuralModelChanged();
        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ProgramCompilation));

        decision = coordinator.completeTask(completed(*preview, true, true));

        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));
        EXPECT_FALSE(hasCommand(decision, RenderCommandType::PresentFrame));
    }

    TEST(RenderUpdateCoordinator, ProgramCompilationCompleted_AfterStructuralChange_StartsParameterUpload)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.notifyStructuralModelChanged();

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ProgramCompilation));

        decision = coordinator.notifyProgramCompilationCompleted();

        EXPECT_TRUE(hasStartedTask(decision, RenderTaskType::ParameterUpload));
    }

    TEST(RenderUpdateCoordinator, StaticTick_WithFreshHeavyTasks_DoesNotStartDuplicateHqWhileInFlight)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.tick();
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = coordinator.completeTask(completed(*bbox));
        auto sdf = findStartedTask(decision, RenderTaskType::SdfPrecomputation);
        ASSERT_TRUE(sdf.has_value());

        decision = coordinator.completeTask(completed(*sdf));
        auto hq = findStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk);
        ASSERT_TRUE(hq.has_value());

        decision = coordinator.tick();

        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::ProgressiveHighQualityChunk));
        EXPECT_TRUE(hasCommand(decision, RenderCommandType::KeepCurrentFrame));
    }

    TEST(RenderUpdateCoordinator, FailedBoundingBox_DoesNotAdvanceToSdfAndCanRetry)
    {
        RenderUpdateCoordinator coordinator;
        auto decision = coordinator.tick();
        auto bbox = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(bbox.has_value());

        decision = coordinator.completeTask(failed(*bbox));

        EXPECT_TRUE(hasCommand(decision, RenderCommandType::DiscardTaskResult));
        EXPECT_FALSE(hasStartedTask(decision, RenderTaskType::SdfPrecomputation));

        decision = coordinator.tick();
        auto retry = findStartedTask(decision, RenderTaskType::BoundingBoxUpdate);
        ASSERT_TRUE(retry.has_value());
        EXPECT_NE(retry->requestId, bbox->requestId);
    }
}
