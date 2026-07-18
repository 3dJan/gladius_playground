#include "ui/render/NeutralFrameSubmissionTracker.h"

#include <gtest/gtest.h>

#include <utility>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        class TestSubmission final : public compute::IRenderSubmission
        {
          public:
            TestSubmission(compute::RenderSubmissionStatus status,
                           std::optional<compute::RenderFrame> frame = std::nullopt)
                : m_status{status}
                , m_frame{std::move(frame)}
            {
            }

            [[nodiscard]] compute::RenderSubmissionStatus getStatus() const noexcept override
            {
                return m_status;
            }

            void requestCancellation() noexcept override
            {
                m_cancellationRequested = true;
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
                return "test failure";
            }

            [[nodiscard]] bool cancellationRequested() const noexcept
            {
                return m_cancellationRequested;
            }

          private:
            compute::RenderSubmissionStatus m_status;
            std::optional<compute::RenderFrame> m_frame;
            bool m_cancellationRequested{};
        };

        [[nodiscard]] RenderTaskRequest makeTask()
        {
            return RenderTaskRequest{.requestId = 42u,
                                     .type = RenderTaskType::ProgressiveHighQualityChunk,
                                     .stamp = {.sceneEpoch = 3u,
                                               .parameterEpoch = 4u,
                                               .viewEpoch = 5u,
                                               .viewportEpoch = 6u,
                                               .qualityEpoch = 7u},
                                     .width = 11u,
                                     .height = 13u,
                                     .startLine = 2u,
                                     .lineCount = 3u};
        }

        [[nodiscard]] compute::RenderFrame makeFrame(RenderTaskRequest const & task)
        {
            return compute::RenderFrame{
              .width = task.width,
              .height = task.height,
              .firstRow = static_cast<std::uint32_t>(task.startLine),
              .endRow = static_cast<std::uint32_t>(task.startLine + task.lineCount),
              .freshness = {.sceneGeneration = task.stamp.sceneEpoch,
                            .viewGeneration = task.stamp.viewEpoch,
                            .parameterGeneration = task.stamp.parameterEpoch},
              .pixels = std::vector<std::uint32_t>(task.width * task.lineCount)};
        }
    }

    TEST(NeutralFrameSubmissionTracker, Poll_WithMatchingProgressiveFrame_ReturnsDisplayableResult)
    {
        NeutralFrameSubmissionTracker tracker;
        auto const task = makeTask();

        ASSERT_TRUE(tracker.track(
          task,
          std::make_unique<TestSubmission>(compute::RenderSubmissionStatus::Succeeded, makeFrame(task))));

        auto results = tracker.poll();

        ASSERT_EQ(results.size(), 1u);
        EXPECT_TRUE(results.front().taskResult.succeeded());
        EXPECT_TRUE(results.front().taskResult.producedDisplayFrame);
        EXPECT_FALSE(results.front().taskResult.completedFrame);
        ASSERT_TRUE(results.front().frame.has_value());
        EXPECT_EQ(results.front().frame->freshness.parameterGeneration, task.stamp.parameterEpoch);
        EXPECT_TRUE(tracker.empty());
    }

    TEST(NeutralFrameSubmissionTracker, Poll_WithMismatchedFreshness_ReturnsFailedResult)
    {
        NeutralFrameSubmissionTracker tracker;
        auto const task = makeTask();
        auto frame = makeFrame(task);
        ++frame.freshness.viewGeneration;

        ASSERT_TRUE(tracker.track(
          task,
          std::make_unique<TestSubmission>(compute::RenderSubmissionStatus::Succeeded, std::move(frame))));

        auto results = tracker.poll();

        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results.front().taskResult.status, RenderTaskStatus::Failed);
        EXPECT_FALSE(results.front().taskResult.producedDisplayFrame);
        EXPECT_FALSE(results.front().frame.has_value());
    }

    TEST(NeutralFrameSubmissionTracker, RequestCancellationForStale_RequestsCancellationAndRetainsSubmission)
    {
        NeutralFrameSubmissionTracker tracker;
        auto const task = makeTask();
        auto submission = std::make_unique<TestSubmission>(compute::RenderSubmissionStatus::Pending);
        auto * submissionPtr = submission.get();
        ASSERT_TRUE(tracker.track(task, std::move(submission)));

        auto currentStamp = task.stamp;
        ++currentStamp.viewEpoch;
        tracker.requestCancellationForStale(currentStamp);

        EXPECT_TRUE(submissionPtr->cancellationRequested());
        EXPECT_FALSE(tracker.empty());
        EXPECT_TRUE(tracker.poll().empty());
    }
}
