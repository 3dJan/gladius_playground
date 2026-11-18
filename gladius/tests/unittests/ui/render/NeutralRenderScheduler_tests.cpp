#include "ui/render/NeutralRenderScheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
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

        /**
         * @brief Models a callback driven backend (WebGPU/Dawn): completion surfaces only after the
         * UI thread has pumped the backend once per frame. Blocking in `wait()` is recorded so tests
         * can prove the progressive loop never blocks the UI thread.
         */
        class PumpDrivenSubmission final : public compute::IRenderSubmission
        {
          public:
            PumpDrivenSubmission(compute::RenderRequest request, std::shared_ptr<std::size_t> waitCount)
                : m_request{std::move(request)}
                , m_waitCount{std::move(waitCount)}
            {
            }

            void progress() noexcept override
            {
                m_status = compute::RenderSubmissionStatus::Succeeded;
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                return m_status;
            }

            void requestCancellation() noexcept override
            {
            }

            void wait() override
            {
                ++(*m_waitCount);
                m_status = compute::RenderSubmissionStatus::Succeeded;
            }

            [[nodiscard]] std::optional<compute::RenderFrame> takeFrame() override
            {
                if (m_frameTaken)
                {
                    return std::nullopt;
                }
                m_frameTaken = true;
                auto const & viewport = m_request.viewport;
                return compute::RenderFrame{.width = viewport.width,
                                            .height = viewport.height,
                                            .firstRow = viewport.firstRow,
                                            .endRow = viewport.endRow,
                                            .freshness = m_request.freshness,
                                            .pixels = std::vector<std::uint32_t>(viewport.pixelCount())};
            }

            [[nodiscard]] std::string getErrorMessage() const override
            {
                return {};
            }

          private:
            compute::RenderRequest m_request;
            std::shared_ptr<std::size_t> m_waitCount;
            compute::RenderSubmissionStatus m_status{compute::RenderSubmissionStatus::Pending};
            bool m_frameTaken{};
        };

        class PumpDrivenRenderer final : public ImmediateRenderer
        {
          public:
            explicit PumpDrivenRenderer(std::shared_ptr<std::size_t> waitCount)
                : m_waitCount{std::move(waitCount)}
            {
            }

            [[nodiscard]] std::unique_ptr<compute::IRenderSubmission>
            submitFrame(compute::RenderRequest request) override
            {
                return std::make_unique<PumpDrivenSubmission>(std::move(request), m_waitCount);
            }

          private:
            std::shared_ptr<std::size_t> m_waitCount;
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

    TEST(NeutralRenderScheduler, FullHeightProgressiveTask_SubmitsContiguousBoundedChunks)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;
        auto task = makeTask();
        task.width = 1920u;
        task.height = 137u;
        task.startLine = 0u;
        task.lineCount = task.height;

        std::uint32_t completedRow{};
        std::size_t chunkCount{};
        while (completedRow < task.height)
        {
            ASSERT_TRUE(scheduler.submit(task, renderer));
            auto result = scheduler.poll();
            ASSERT_EQ(result.acceptedFrames.size(), 1u);
            ASSERT_EQ(result.completions.size(), 1u);
            auto const & frame = result.acceptedFrames.front().frame;
            EXPECT_EQ(frame.firstRow, completedRow);
            EXPECT_GT(frame.endRow, frame.firstRow);
            EXPECT_LE(frame.endRow, task.height);
            EXPECT_LE(static_cast<std::size_t>(frame.endRow - frame.firstRow) * task.width,
                      NeutralRenderScheduler::MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS);
            EXPECT_EQ(result.completions.front().taskResult.completedFrame,
                      frame.endRow == task.height);
            completedRow = frame.endRow;
            EXPECT_TRUE(scheduler.workflow().presentCandidate(result.acceptedFrames.front().candidate));
            ++task.requestId;
            ++chunkCount;
        }

        EXPECT_GT(chunkCount, 1u);
        EXPECT_TRUE(scheduler.workflow().isHighQualityFrameCurrent());
    }

    TEST(NeutralRenderScheduler, AdaptiveProgressiveBudget_ShrinksOnSlowChunkAndRecoversOnFastChunk)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        auto const initialBudget = scheduler.currentProgressiveChunkPixels();
        EXPECT_EQ(initialBudget, NeutralRenderScheduler::MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS);

        // Slow chunk (200 ms GPU >> 33 ms ceiling) must shrink the budget.
        scheduler.adaptProgressiveChunkBudget(RenderTaskResult{
          .durationNs = 200'000'000ull,
          .gpuDurationNs = 200'000'000ull,
          .gpuTimeAvailable = true});
        auto const shrunkBudget = scheduler.currentProgressiveChunkPixels();
        EXPECT_LT(shrunkBudget, initialBudget);
        EXPECT_GE(shrunkBudget, NeutralRenderScheduler::MIN_PROGRESSIVE_CHUNK_PIXELS);

        // Fast chunk must recover towards a whole-frame chunk.
        scheduler.adaptProgressiveChunkBudget(RenderTaskResult{
          .durationNs = 1'000'000ull,
          .gpuDurationNs = 1'000'000ull,
          .gpuTimeAvailable = true});
        EXPECT_GT(scheduler.currentProgressiveChunkPixels(), shrunkBudget);
    }

    TEST(NeutralRenderScheduler, AdaptiveProgressiveBudget_UsesGpuTimeInsteadOfWallTime)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        auto const initialBudget = scheduler.currentProgressiveChunkPixels();

        // Long wall time but a cheap GPU pass must not shrink the budget.
        scheduler.adaptProgressiveChunkBudget(RenderTaskResult{
          .durationNs = 200'000'000ull,
          .gpuDurationNs = 1'000'000ull,
          .gpuTimeAvailable = true});
        EXPECT_EQ(scheduler.currentProgressiveChunkPixels(), initialBudget);
    }

    TEST(NeutralRenderScheduler, FullHeightProgressiveTask_FillsFrameInBoundedChunks)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;
        auto task = makeTask();
        task.width = 1920u;
        task.height = 1080u;
        task.startLine = 0u;
        task.lineCount = task.height;

        std::uint32_t completedRow{};
        std::size_t chunkCount{};
        while (completedRow < task.height)
        {
            ASSERT_TRUE(scheduler.submit(task, renderer));
            auto const result = scheduler.poll();
            ASSERT_EQ(result.acceptedFrames.size(), 1u);
            completedRow = result.acceptedFrames.front().frame.endRow;
            EXPECT_TRUE(scheduler.workflow().presentCandidate(result.acceptedFrames.front().candidate));
            ++task.requestId;
            ++chunkCount;
        }

        EXPECT_EQ(chunkCount, NeutralRenderScheduler::TARGET_PROGRESSIVE_CHUNKS);
    }

    TEST(NeutralRenderScheduler, FullHeightProgressiveTask_WithCallbackDrivenBackend_ReachesHighQualityWithoutBlocking)
    {
        auto const waitCount = std::make_shared<std::size_t>();
        PumpDrivenRenderer renderer{waitCount};
        NeutralRenderScheduler scheduler{makeRequest};
        auto task = makeTask();
        task.width = 512u;
        task.height = 512u;
        task.startLine = 0u;
        task.lineCount = task.height;

        std::size_t uiFrames{};
        while (!scheduler.workflow().isHighQualityFrameCurrent())
        {
            // One UI frame: poll the in-flight submission, then start at most one new task.
            auto const polled = scheduler.poll(false);
            for (auto const & accepted : polled.acceptedFrames)
            {
                EXPECT_TRUE(scheduler.workflow().presentCandidate(accepted.candidate));
            }
            if (scheduler.workflow().isHighQualityFrameCurrent())
            {
                break;
            }
            ASSERT_FALSE(scheduler.hasInFlightSubmissions());
            ASSERT_TRUE(scheduler.submit(task, renderer));
            ++task.requestId;
            ++uiFrames;
        }

        EXPECT_TRUE(scheduler.workflow().isHighQualityFrameCurrent());
        // Reaching full quality never blocked the UI thread on a backend callback.
        EXPECT_EQ(*waitCount, 0u);
        // The resolution-aware budget bounds the fill; the adaptive controller may only shorten it.
        EXPECT_LE(uiFrames, NeutralRenderScheduler::TARGET_PROGRESSIVE_CHUNKS);
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
        auto const firstCameraTask = findStartedTask(firstCameraDecision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(firstCameraTask.has_value());
        scheduler.requestCancellationForStale();

        auto const secondCameraDecision = scheduler.workflow().notifyCameraChanged(true);
        auto const secondCameraTask = findStartedTask(secondCameraDecision, RenderTaskType::LowResolutionPreview);
        ASSERT_TRUE(secondCameraTask.has_value());
        scheduler.requestCancellationForStale();

        auto const latestCameraDecision = scheduler.workflow().notifyCameraChanged(true);
        auto const latestCameraTask = findStartedTask(latestCameraDecision, RenderTaskType::LowResolutionPreview);
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
