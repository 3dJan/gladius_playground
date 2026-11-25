/**
 * @file AgentRunControl_tests.cpp
 * @brief Unit tests for the cooperative pause/resume signal used by the Design Agent loop.
 */

#include "agent/AgentRunControl.h"
#include "io/CancellationToken.h"

#include <coro/coro.hpp>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace gladius::agent::tests
{
    namespace
    {
        coro::task<void> parkUntilResumed(AgentRunControl & control,
                                          io::CancellationToken const & cancellation)
        {
            co_await control.waitForResume(cancellation);
            co_return;
        }
    } // namespace

    TEST(AgentRunControl, NotPaused_AwaitIsImmediatelyReady)
    {
        AgentRunControl control;
        io::CancellationToken cancellation;

        EXPECT_FALSE(control.isPaused());
        coro::sync_wait(parkUntilResumed(control, cancellation));
        EXPECT_FALSE(control.isPaused());
    }

    TEST(AgentRunControl, PausedRun_StaysParkedUntilResumed)
    {
        // Arrange
        AgentRunControl control;
        io::CancellationToken cancellation;
        control.requestPause();
        std::atomic<bool> released{false};

        // Act
        std::thread runner(
          [&]()
          {
              coro::sync_wait(parkUntilResumed(control, cancellation));
              released.store(true);
          });

        // Assert: pause is cooperative, so the run waits rather than finishing.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        EXPECT_TRUE(control.isPaused());
        EXPECT_FALSE(released.load());

        control.resume();
        runner.join();
        EXPECT_TRUE(released.load());
        EXPECT_FALSE(control.isPaused());
    }

    TEST(AgentRunControl, CancelledWhilePaused_LoopCanStillBeReleased)
    {
        // Arrange
        AgentRunControl control;
        io::CancellationToken cancellation;
        control.requestPause();
        std::atomic<bool> released{false};
        std::thread runner(
          [&]()
          {
              coro::sync_wait(parkUntilResumed(control, cancellation));
              released.store(true);
          });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ASSERT_FALSE(released.load());

        // Act: stopping a paused run cancels and then releases the parked loop.
        cancellation.requestCancellation();
        control.resume();
        runner.join();

        // Assert
        EXPECT_TRUE(released.load());
        EXPECT_TRUE(cancellation.isCancelled());
    }

    TEST(AgentRunControl, Reset_ClearsPauseSoTheNextRunStartsRunning)
    {
        AgentRunControl control;
        control.requestPause();
        ASSERT_TRUE(control.isPaused());

        control.reset();

        EXPECT_FALSE(control.isPaused());
        io::CancellationToken cancellation;
        coro::sync_wait(parkUntilResumed(control, cancellation));
    }
} // namespace gladius::agent::tests
