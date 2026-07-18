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

        class ImmediateRenderer final : public compute::IComputeRenderer
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

        auto accepted = scheduler.poll();

        ASSERT_EQ(accepted.size(), 1u);
        EXPECT_EQ(accepted.front().candidate.frameId, task.requestId);
        EXPECT_EQ(accepted.front().candidate.quality, FramePresentationQuality::ProgressivePartial);
        EXPECT_EQ(accepted.front().frame.pixels.size(), task.width * task.lineCount);
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
    }

    TEST(NeutralRenderScheduler, Submit_WithLowResolutionTask_ReturnsFalse)
    {
        NeutralRenderScheduler scheduler{makeRequest};
        ImmediateRenderer renderer;

        EXPECT_FALSE(scheduler.submit(makeTask(RenderTaskType::LowResolutionPreview), renderer));
        EXPECT_FALSE(scheduler.hasInFlightSubmissions());
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

        EXPECT_TRUE(scheduler.poll().empty());
        ASSERT_TRUE(scheduler.workflow().presentedFrame().has_value());
        EXPECT_EQ(scheduler.workflow().presentedFrame()->frameId, 1u);
    }
}
