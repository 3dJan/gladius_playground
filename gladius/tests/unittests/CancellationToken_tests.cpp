#include <gtest/gtest.h>

#include "io/CancellationToken.h"

#include <future>
#include <thread>

namespace gladius::io::tests
{

    TEST(CancellationToken, InitiallyNotCancelled)
    {
        CancellationToken token;
        EXPECT_FALSE(token.isCancelled());
    }

    TEST(CancellationToken, RequestCancellation_SetsCancelledFlag)
    {
        CancellationToken token;
        token.requestCancellation();
        EXPECT_TRUE(token.isCancelled());
    }

    TEST(CancellationToken, Reset_ClearsCancelledFlag)
    {
        CancellationToken token;
        token.requestCancellation();
        EXPECT_TRUE(token.isCancelled());
        token.reset();
        EXPECT_FALSE(token.isCancelled());
    }

    TEST(CancellationToken, MultipleRequestCancellation_RemainsTrue)
    {
        CancellationToken token;
        token.requestCancellation();
        token.requestCancellation();
        token.requestCancellation();
        EXPECT_TRUE(token.isCancelled());
    }

    TEST(CancellationToken, ThreadSafety_CancellationVisibleAcrossThreads)
    {
        CancellationToken token;

        // Start a worker thread that waits for cancellation
        auto future = std::async(std::launch::async, [&token]() {
            while (!token.isCancelled())
            {
                std::this_thread::yield();
            }
            return true;
        });

        // Give the worker a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Request cancellation from the main thread
        token.requestCancellation();

        // Worker should see the cancellation and return
        auto status = future.wait_for(std::chrono::seconds(1));
        EXPECT_EQ(status, std::future_status::ready);
        EXPECT_TRUE(future.get());
    }

} // namespace gladius::io::tests
