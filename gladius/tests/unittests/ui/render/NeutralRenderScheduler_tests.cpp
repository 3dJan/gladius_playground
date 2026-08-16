#include "ui/render/NeutralRenderScheduler.h"

#include <gtest/gtest.h>

#include <utility>

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
}
