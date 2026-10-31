#include "compute/AsyncTask.h"

#include <gtest/gtest.h>

#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        compute::AsyncTask<int> returnValueTask(int value)
        {
            co_return value;
        }

        compute::AsyncTask<void> voidTask(int& counter)
        {
            ++counter;
            co_return;
        }

        compute::AsyncTask<int> throwingTask()
        {
            throw std::runtime_error("boom");
            co_return 0;
        }

        compute::AsyncTask<int> addAwaitedTask(int value)
        {
            int const child = co_await returnValueTask(value);
            co_return child + 1;
        }

        compute::AsyncTask<int> yieldingTask(int& steps)
        {
            for (int i = 0; i < 3; ++i)
            {
                ++steps;
                co_await compute::AsyncYield{};
            }
            co_return steps;
        }

        compute::AsyncTask<int> waitingTask(bool& flag)
        {
            co_await compute::WaitUntil([&flag] { return flag; });
            co_return 7;
        }

        compute::AsyncTask<int> propagatingTask()
        {
            int const value = co_await throwingTask();
            co_return value;
        }

        bool canAcquireFromOtherThread(std::recursive_mutex& deviceMutex)
        {
            bool acquired = false;
            std::thread probe([&]
            {
                std::unique_lock<std::recursive_mutex> lock(deviceMutex, std::try_to_lock);
                acquired = lock.owns_lock();
            });
            probe.join();
            return acquired;
        }

        compute::AsyncTask<int> scopedDeviceTask(std::recursive_mutex& deviceMutex,
                                                bool& readbackReady,
                                                int& submissions,
                                                bool& analysisHadUnlockedDevice)
        {
            for (int batch = 0; batch < 2; ++batch)
            {
                {
                    std::unique_lock<std::recursive_mutex> const lock(deviceMutex);
                    ++submissions;
                }
                co_await compute::WaitUntil([&readbackReady] { return readbackReady; });
                analysisHadUnlockedDevice = canAcquireFromOtherThread(deviceMutex);
                if (batch == 0)
                {
                    co_await compute::AsyncYield{};
                }
            }
            co_return submissions;
        }
    } // namespace

    TEST(AsyncTask, RunBlocking_ReturnsValue_WhenTaskCompletesWithoutSuspension)
    {
        auto const result = compute::runBlocking(returnValueTask(42), [] {});

        EXPECT_EQ(result, 42);
    }

    TEST(AsyncTask, RunBlocking_RunsVoidTask_WhenStarted)
    {
        int counter = 0;

        compute::runBlocking(voidTask(counter), [] {});

        EXPECT_EQ(counter, 1);
    }

    TEST(AsyncTask, RunBlocking_ResumesNestedTask_WhenChildAwaited)
    {
        auto const result = compute::runBlocking(addAwaitedTask(5), [] {});

        EXPECT_EQ(result, 6);
    }

    TEST(AsyncTask, Step_ReturnsControlAfterEachYield_WhenTaskYields)
    {
        int steps = 0;
        auto task = yieldingTask(steps);

        task.step();
        EXPECT_FALSE(task.done());
        EXPECT_EQ(steps, 1);

        task.step();
        task.step();
        EXPECT_EQ(steps, 3);
        EXPECT_FALSE(task.done());

        task.step();
        EXPECT_TRUE(task.done());
        EXPECT_EQ(task.take(), 3);
    }

    TEST(AsyncTask, Step_KeepsSuspended_UntilWaitPredicateHolds)
    {
        bool flag = false;
        auto task = waitingTask(flag);

        task.step();
        task.step();
        task.step();
        EXPECT_FALSE(task.done());

        flag = true;
        task.step();
        EXPECT_TRUE(task.done());
        EXPECT_EQ(task.take(), 7);
    }

    TEST(AsyncTask, Take_RethrowsException_WhenTaskThrows)
    {
        EXPECT_THROW(compute::runBlocking(throwingTask(), [] {}), std::runtime_error);
    }

    TEST(AsyncTask, Take_PropagatesException_WhenNestedTaskThrows)
    {
        EXPECT_THROW(compute::runBlocking(propagatingTask(), [] {}), std::runtime_error);
    }

    TEST(AsyncTask, RunBlocking_CallsPump_BetweenSuspensions)
    {
        int steps = 0;
        int pumps = 0;

        auto const result = compute::runBlocking(yieldingTask(steps), [&pumps] { ++pumps; });

        EXPECT_EQ(result, 3);
        EXPECT_EQ(pumps, 3);
    }

    TEST(AsyncTask, Step_ScopedDeviceTransaction_ReleasesGateBeforeWaitAnalysisAndYield)
    {
        std::recursive_mutex deviceMutex;
        bool readbackReady = false;
        int submissions = 0;
        bool analysisHadUnlockedDevice = false;
        auto task = scopedDeviceTask(deviceMutex, readbackReady, submissions, analysisHadUnlockedDevice);

        EXPECT_EQ(submissions, 0);
        task.step();
        EXPECT_EQ(submissions, 1);
        EXPECT_FALSE(task.done());
        EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));

        task.step();
        EXPECT_EQ(submissions, 1);
        EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));

        readbackReady = true;
        task.step();
        EXPECT_TRUE(analysisHadUnlockedDevice);
        EXPECT_EQ(submissions, 1);
        EXPECT_FALSE(task.done());
        EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));

        task.step();
        EXPECT_TRUE(task.done());
        EXPECT_EQ(task.take(), 2);
        EXPECT_TRUE(analysisHadUnlockedDevice);
        EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));
    }

    TEST(AsyncTask, Destroy_SuspendedScopedDeviceTransaction_LeavesGateAvailable)
    {
        std::recursive_mutex deviceMutex;
        bool readbackReady = false;
        int submissions = 0;
        bool analysisHadUnlockedDevice = false;
        {
            auto task = scopedDeviceTask(deviceMutex, readbackReady, submissions, analysisHadUnlockedDevice);
            task.step();
            EXPECT_FALSE(task.done());
            EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));
        }

        EXPECT_EQ(submissions, 1);
        EXPECT_FALSE(analysisHadUnlockedDevice);
        EXPECT_TRUE(canAcquireFromOtherThread(deviceMutex));
    }
} // namespace gladius::tests
