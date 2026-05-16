#include "ui/render/AsyncRenderController.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        [[nodiscard]] RenderJob makeHighQualityJob(uint64_t epoch, uint64_t viewEpoch)
        {
            RenderStamp stamp{};
            stamp.viewEpoch = viewEpoch;
            return RenderJob{.epoch = epoch,
                             .viewEpoch = viewEpoch,
                             .frameHint = 42,
                             .type = RenderJobType::HighQuality,
                             .width = 640,
                             .height = 480,
                             .startLine = 16,
                             .stepSize = 32,
                             .coordinatorRequestId = 7,
                             .coordinatorStamp = stamp};
        }

        [[nodiscard]] std::optional<FrameResultMeta> waitForResult(AsyncRenderController & controller)
        {
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (auto result = controller.tryConsumeResult())
                {
                    return result;
                }
                std::this_thread::yield();
            }
            return std::nullopt;
        }
    }

    TEST(AsyncRenderController, Worker_WithStaleViewProgressiveJob_CancelsBeforeExecutor)
    {
        AsyncRenderController controller;
        std::atomic<int> executions{0};
        controller.setJobExecutor(
                    [&executions](RenderJob const & job,
                                                AsyncRenderController::CancelCheck const &) -> coro::task<FrameResultMeta>
          {
              executions.fetch_add(1, std::memory_order_relaxed);
              co_return FrameResultMeta{.frameId = job.frameHint,
                                        .epoch = job.epoch,
                                        .viewEpoch = job.viewEpoch,
                                        .jobType = job.type,
                                        .width = job.width,
                                        .height = job.height,
                                        .startLine = job.startLine,
                                        .completedLine = job.startLine + job.stepSize,
                                        .coordinatorRequestId = job.coordinatorRequestId,
                                        .coordinatorStamp = job.coordinatorStamp};
          });
        controller.start();
        controller.setLatestViewEpoch(2);

        controller.enqueueJob(makeHighQualityJob(1, 1));

        auto const result = waitForResult(controller);
        controller.stop();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->cancelled);
        EXPECT_EQ(result->jobType, RenderJobType::HighQuality);
        EXPECT_EQ(result->viewEpoch, 1u);
        EXPECT_EQ(result->coordinatorRequestId, 7u);
        EXPECT_EQ(executions.load(std::memory_order_relaxed), 0);
    }

    TEST(AsyncRenderController, Worker_WithStaleEpochJob_CancelsBeforeExecutor)
    {
        AsyncRenderController controller;
        std::atomic<int> executions{0};
        controller.setJobExecutor(
                    [&executions](RenderJob const & job,
                                                AsyncRenderController::CancelCheck const &) -> coro::task<FrameResultMeta>
          {
              executions.fetch_add(1, std::memory_order_relaxed);
              co_return FrameResultMeta{.frameId = job.frameHint,
                                        .epoch = job.epoch,
                                        .viewEpoch = job.viewEpoch,
                                        .jobType = job.type};
          });
        controller.start();
        controller.setLatestEpoch(2);

        controller.enqueueJob(makeHighQualityJob(1, 1));

        auto const result = waitForResult(controller);
        controller.stop();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->cancelled);
        EXPECT_EQ(result->epoch, 1u);
        EXPECT_EQ(executions.load(std::memory_order_relaxed), 0);
    }

    TEST(AsyncRenderController, Worker_WithBackToBackStaleJobs_PublishesAllCancelledResults)
    {
        AsyncRenderController controller;
        std::atomic<int> executions{0};
        controller.setJobExecutor(
                    [&executions](RenderJob const & job,
                                                AsyncRenderController::CancelCheck const &) -> coro::task<FrameResultMeta>
          {
              executions.fetch_add(1, std::memory_order_relaxed);
              co_return FrameResultMeta{.frameId = job.frameHint,
                                        .epoch = job.epoch,
                                        .viewEpoch = job.viewEpoch,
                                        .jobType = job.type};
          });
        controller.start();
        controller.setLatestViewEpoch(3);

        controller.enqueueJob(makeHighQualityJob(1, 1));
        controller.enqueueJob(makeHighQualityJob(1, 2));

        auto const firstResult = waitForResult(controller);
        auto const secondResult = waitForResult(controller);
        controller.stop();

        ASSERT_TRUE(firstResult.has_value());
        ASSERT_TRUE(secondResult.has_value());
        EXPECT_TRUE(firstResult->cancelled);
        EXPECT_TRUE(secondResult->cancelled);
        EXPECT_EQ(firstResult->viewEpoch, 1u);
        EXPECT_EQ(secondResult->viewEpoch, 2u);
        EXPECT_EQ(executions.load(std::memory_order_relaxed), 0);
    }

    TEST(AsyncRenderController, Worker_WithCurrentProgressiveJob_Executes)
    {
        AsyncRenderController controller;
        std::atomic<int> executions{0};
        controller.setJobExecutor(
                    [&executions](RenderJob const & job,
                                                AsyncRenderController::CancelCheck const &) -> coro::task<FrameResultMeta>
          {
              executions.fetch_add(1, std::memory_order_relaxed);
              co_return FrameResultMeta{.frameId = job.frameHint,
                                        .epoch = job.epoch,
                                        .viewEpoch = job.viewEpoch,
                                        .jobType = job.type,
                                        .width = job.width,
                                        .height = job.height,
                                        .startLine = job.startLine,
                                        .completedLine = job.startLine + job.stepSize,
                                        .coordinatorRequestId = job.coordinatorRequestId,
                                        .coordinatorStamp = job.coordinatorStamp};
          });
        controller.start();
        controller.setLatestViewEpoch(1);

        controller.enqueueJob(makeHighQualityJob(1, 1));

        auto const result = waitForResult(controller);
        controller.stop();

        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->cancelled);
        EXPECT_EQ(result->jobType, RenderJobType::HighQuality);
        EXPECT_EQ(executions.load(std::memory_order_relaxed), 1);
    }
}