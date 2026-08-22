#include "ui/render/NeutralRenderScheduler.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        class ImmediateSubmission final : public compute::IRenderSubmission
        {
          public:
            explicit ImmediateSubmission(compute::RenderFrame frame)
                : m_frame{std::move(frame)}
            {
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                return m_frame.has_value() ? compute::RenderSubmissionStatus::Succeeded
                                           : compute::RenderSubmissionStatus::Cancelled;
            }

            void requestCancellation() noexcept override
            {
                m_frame.reset();
            }

            void wait() override
            {
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                return std::exchange(m_frame, std::nullopt);
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return {};
            }

          private:
            std::optional<compute::RenderFrame> m_frame;
        };

        class PendingSubmission final : public compute::IRenderSubmission
        {
          public:
            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                return m_status;
            }

            void requestCancellation() noexcept override
            {
                m_status = compute::RenderSubmissionStatus::Cancelled;
            }

            void wait() override
            {
                m_status = compute::RenderSubmissionStatus::Cancelled;
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                return std::nullopt;
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return {};
            }

          private:
            compute::RenderSubmissionStatus m_status{compute::RenderSubmissionStatus::Pending};
        };

        class ImmediateRenderer : public compute::IComputeRenderer
        {
          public:
            [[nodiscard]] compute::ComputeBackendKind getBackendKind() const noexcept override
            {
                return compute::ComputeBackendKind::OpenCL;
            }

            [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override
            {
                return compute::RendererCapability::AnalyticRendering |
                       compute::RendererCapability::ProgressiveRendering;
            }

            [[nodiscard]] bool isAvailable() const noexcept override
            {
                return true;
            }

            [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
            submitFrame(compute::RenderRequest request) override
            {
                auto const & viewport = request.viewport;
                return std::make_unique<ImmediateSubmission>(compute::RenderFrame{
                  .width = viewport.width,
                  .height = viewport.height,
                  .firstRow = viewport.firstRow,
                  .endRow = viewport.endRow,
                  .freshness = request.freshness,
                  .pixels = std::vector<std::uint32_t>(viewport.pixelCount())});
            }
        };

        class PendingRenderer final : public ImmediateRenderer
        {
          public:
            [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
            submitFrame(compute::RenderRequest) override
            {
                return std::make_unique<PendingSubmission>();
            }
        };

        struct ControlledSubmissionState
        {
            compute::RenderRequest request;
            compute::RenderSubmissionStatus status{compute::RenderSubmissionStatus::Pending};
            bool frameTaken{false};
        };

        class ControlledSubmission final : public compute::IRenderSubmission
        {
          public:
            explicit ControlledSubmission(std::shared_ptr<ControlledSubmissionState> state)
                : m_state{std::move(state)}
            {
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                return m_state->status;
            }

            void requestCancellation() noexcept override
            {
                // Simulate a backend such as Dawn where an already submitted dispatch cannot be
                // cancelled reliably. The tracker must retain this submission until completion.
            }

            void wait() override
            {
                m_state->status = compute::RenderSubmissionStatus::Cancelled;
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                if (m_state->frameTaken || m_state->status != compute::RenderSubmissionStatus::Succeeded)
                {
                    return std::nullopt;
                }

                m_state->frameTaken = true;
                auto const & viewport = m_state->request.viewport;
                return compute::RenderFrame{.width = viewport.width,
                                            .height = viewport.height,
                                            .firstRow = viewport.firstRow,
                                            .endRow = viewport.endRow,
                                            .freshness = m_state->request.freshness,
                                            .pixels = std::vector<std::uint32_t>(viewport.pixelCount())};
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return {};
            }

          private:
            std::shared_ptr<ControlledSubmissionState> m_state;
        };

        class ControlledRenderer final : public ImmediateRenderer
        {
          public:
            [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
            submitFrame(compute::RenderRequest request) override
            {
                auto state = std::make_shared<ControlledSubmissionState>(
                  ControlledSubmissionState{.request = std::move(request)});
                m_submissions.push_back(state);
                return std::make_unique<ControlledSubmission>(std::move(state));
            }

            void complete(std::size_t const index)
            {
                m_submissions.at(index)->status = compute::RenderSubmissionStatus::Succeeded;
            }

            [[nodiscard]] std::size_t submissionCount() const noexcept
            {
                return m_submissions.size();
            }

          private:
            std::vector<std::shared_ptr<ControlledSubmissionState>> m_submissions;
        };

        class ImmediateScene final : public compute::IRenderScene
        {
          public:
            [[nodiscard]] compute::ComputeBackendKind getBackendKind() const noexcept override
            {
                return compute::ComputeBackendKind::OpenCL;
            }

            [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept override
            {
                return 1u;
            }

            [[nodiscard]] compute::RendererCapability getCapabilities() const noexcept override
            {
                return compute::RendererCapability::AnalyticRendering;
            }
        };

        class SceneAwareImmediateRenderer final : public ImmediateRenderer
        {
          public:
                        [[nodiscard]] std::unique_ptr<compute::IRenderScene>
                        materializeScene(compute::RenderSceneSnapshot snapshot) override
                        {
                                return std::make_unique<ImmediateScene>();
                        }

            [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
            submitFrame(compute::IRenderScene const &, compute::RenderRequest request) override
            {
                return ImmediateRenderer::submitFrame(std::move(request));
            }
        };

        [[nodiscard]] RenderTaskRequest makeTask(RenderTaskType const type = RenderTaskType::ProgressiveHighQualityChunk)
        {
            return RenderTaskRequest{.requestId = 71u,
                                     .type = type,
                                     .width = 9u,
                                     .height = 7u,
                                     .startLine = 2u,
                                     .lineCount = 3u};
        }

        [[nodiscard]] std::optional<compute::RenderRequest> makeRequest(RenderTaskRequest const & task)
        {
            return compute::RenderRequest{
              .viewport = {.width = task.width,
                           .height = task.height,
                           .firstRow = static_cast<std::uint32_t>(task.startLine),
                           .endRow = static_cast<std::uint32_t>(task.startLine + task.lineCount)},
              .freshness = {.sceneGeneration = task.stamp.sceneEpoch,
                            .viewGeneration = task.stamp.viewEpoch,
                            .parameterGeneration = task.stamp.parameterEpoch}};
        }

        [[nodiscard]] std::optional<RenderTaskRequest> findStartedTask(
          RenderWorkflowDecision const & decision,
          RenderTaskType const type)
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
    }

    TEST(NeutralRenderScheduler, SubmitAndPoll_WithProgressiveTask_ReturnsAcceptedFrame)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;
        auto const task = makeTask();

        ASSERT_TRUE(scheduler.submit(task, renderer));
        EXPECT_TRUE(scheduler.hasInFlightSubmissions());

        auto result = scheduler.poll();

        ASSERT_EQ(result.acceptedFrames.size(), 1u);
        EXPECT_EQ(result.acceptedFrames.front().candidate.frameId, task.requestId);
        EXPECT_EQ(result.acceptedFrames.front().candidate.quality, FramePresentationQuality::ProgressivePartial);
        EXPECT_EQ(result.acceptedFrames.front().frame.pixels.size(), task.width * task.lineCount);
        EXPECT_FALSE(result.commands.empty());
        ASSERT_EQ(result.completions.size(), 1u);
        EXPECT_TRUE(result.completions.front().taskResult.succeeded());
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }

    TEST(NeutralRenderScheduler, SubmitAndPoll_WithLowResolutionTask_ReturnsAcceptedFrame)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;

        auto const task = makeTask(RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(scheduler.submit(task, renderer));
        auto const result = scheduler.poll();

        ASSERT_EQ(result.acceptedFrames.size(), 1u);
        EXPECT_EQ(result.acceptedFrames.front().candidate.frameId, task.requestId);
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }

    TEST(NeutralRenderScheduler, Submit_WithMaterializedScene_ReturnsAcceptedFrame)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        SceneAwareImmediateRenderer renderer;
        ImmediateScene scene;
        auto const task = makeTask();

        ASSERT_TRUE(scheduler.submit(task, renderer, scene));

                auto result = scheduler.poll();

                ASSERT_EQ(result.acceptedFrames.size(), 1u);
                EXPECT_EQ(result.acceptedFrames.front().frame.firstRow, task.startLine);
                EXPECT_EQ(result.acceptedFrames.front().frame.endRow, task.startLine + task.lineCount);
        }

        TEST(NeutralRenderScheduler, Submit_WithBackendSession_ReturnsAcceptedFrame)
        {
                NeutralRenderScheduler scheduler{makeRequest};
                auto renderer = std::make_unique<SceneAwareImmediateRenderer>();
                compute::RenderBackendSession session{std::move(renderer)};
                ASSERT_TRUE(session.replaceScene(
                    compute::RenderSceneSnapshot{.sceneGeneration = 1u,
                                                                             .analyticEvaluatorWgsl = "fn evaluateModel() {}"}));
                (void) scheduler.workflow().notifyStructuralModelChanged();
                auto task = makeTask();
                task.stamp = scheduler.workflow().latestStamp();

                ASSERT_TRUE(scheduler.submit(task, session));

                auto result = scheduler.poll();
                ASSERT_EQ(result.acceptedFrames.size(), 1u);
                EXPECT_EQ(result.acceptedFrames.front().candidate.frameId, task.requestId);
    }

    TEST(NeutralRenderScheduler, Poll_WithCurrentFullQualityFrame_RejectsProgressiveRegression)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;
        auto const task = makeTask();
        scheduler.workflow().seedPresentedFrame(
          PresentedFrame{.frameId = 1u,
                         .stamp = task.stamp,
                         .quality = FramePresentationQuality::FullQuality,
                         .source = FramePresentationSource::HeldFrame,
                         .completedFrame = true});

        ASSERT_TRUE(scheduler.submit(task, renderer));

        EXPECT_TRUE(scheduler.poll().acceptedFrames.empty());
        ASSERT_TRUE(scheduler.workflow().presentedFrame().has_value());
        EXPECT_EQ(scheduler.workflow().presentedFrame()->frameId, 1u);
    }

    TEST(NeutralRenderScheduler, Submit_WithStaleViewportTask_RejectsSubmission)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;

        auto task = makeTask(RenderTaskType::RealtimeFullFrame);
        auto const viewportDecision = scheduler.workflow().configureViewport(640u, 480u);
        ASSERT_FALSE(viewportDecision.commands.empty());

        EXPECT_FALSE(scheduler.submit(task, renderer));
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }

    TEST(NeutralRenderScheduler, StaleSubmission_DoesNotBlockCurrentReplacementFrame)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ASSERT_FALSE(scheduler.workflow().configureViewport(9u, 7u).commands.empty());

        auto staleTask = makeTask(RenderTaskType::RealtimeFullFrame);
        staleTask.stamp = scheduler.workflow().latestStamp();
        PendingRenderer pendingRenderer;
        ASSERT_TRUE(scheduler.submit(staleTask, pendingRenderer));

        (void) scheduler.workflow().notifyEmbeddedParameterChanged(true);
        scheduler.requestCancellationForStale();

        EXPECT_TRUE(scheduler.hasInFlightSubmissions());
        EXPECT_FALSE(scheduler.hasCurrentInFlightSubmission());

        auto const currentDecision = scheduler.workflow().startDisplayTask(
          RenderTaskType::RealtimeFullFrame);
        auto currentTask = findStartedTask(currentDecision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(currentTask.has_value());

        ImmediateRenderer currentRenderer;
        EXPECT_TRUE(scheduler.submit(*currentTask, currentRenderer));
        EXPECT_TRUE(scheduler.hasCurrentInFlightSubmission());

        auto const pollResult = scheduler.poll();
        ASSERT_EQ(pollResult.acceptedFrames.size(), 1u);
        EXPECT_EQ(pollResult.acceptedFrames.front().candidate.frameId, currentTask->requestId);
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }

    TEST(NeutralRenderScheduler,
         MultipleCameraChangesWhileSubmissionIsPending_SubmitOnlyLatestReplacement)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        RealtimeRaymarchConfig config{};
        config.mode = RealtimeRaymarchMode::Force;
        scheduler.workflow().configureRealtime(config);
        ASSERT_FALSE(scheduler.workflow().configureViewport(9u, 7u).commands.empty());

        ControlledRenderer renderer;
        auto const initialDecision = scheduler.workflow().startDisplayTask(
          RenderTaskType::RealtimeFullFrame);
        auto const initialTask = findStartedTask(initialDecision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(initialTask.has_value());
        ASSERT_TRUE(scheduler.submit(*initialTask, renderer));
        ASSERT_EQ(renderer.submissionCount(), 1u);

        auto const firstCameraDecision = scheduler.workflow().notifyCameraChanged(true);
        auto const firstCameraTask = findStartedTask(firstCameraDecision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(firstCameraTask.has_value());
        scheduler.requestCancellationForStale();

        auto const secondCameraDecision = scheduler.workflow().notifyCameraChanged(true);
        auto const secondCameraTask = findStartedTask(secondCameraDecision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(secondCameraTask.has_value());
        scheduler.requestCancellationForStale();

        auto const latestCameraDecision = scheduler.workflow().notifyCameraChanged(true);
        auto const latestCameraTask = findStartedTask(latestCameraDecision, RenderTaskType::RealtimeFullFrame);
        ASSERT_TRUE(latestCameraTask.has_value());
        scheduler.requestCancellationForStale();

        EXPECT_NE(initialTask->requestId, firstCameraTask->requestId);
        EXPECT_NE(firstCameraTask->requestId, secondCameraTask->requestId);
        EXPECT_NE(secondCameraTask->requestId, latestCameraTask->requestId);
        EXPECT_EQ(latestCameraTask->stamp.viewEpoch,
                  scheduler.workflow().latestStamp().viewEpoch);
        EXPECT_TRUE(scheduler.hasInFlightSubmissions());

        renderer.complete(0u);
        auto const oldCompletion = scheduler.poll();
        ASSERT_EQ(oldCompletion.acceptedFrames.size(), 1u);
        EXPECT_EQ(oldCompletion.acceptedFrames.front().candidate.stamp.viewEpoch,
                  initialTask->stamp.viewEpoch);
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());

        ASSERT_TRUE(scheduler.submit(*latestCameraTask, renderer));
        EXPECT_EQ(renderer.submissionCount(), 2u);

        renderer.complete(1u);
        auto const latestCompletion = scheduler.poll();
        ASSERT_EQ(latestCompletion.acceptedFrames.size(), 1u);
        EXPECT_EQ(latestCompletion.acceptedFrames.front().candidate.stamp.viewEpoch,
                  latestCameraTask->stamp.viewEpoch);
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }
}
