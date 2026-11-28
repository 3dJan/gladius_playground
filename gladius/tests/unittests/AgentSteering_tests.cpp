#include "agent/SteeringQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace gladius::agent::tests
{
    TEST(SteeringQueue, Push_UntilCapacity_RejectsFurtherMessages)
    {
        // Arrange
        SteeringQueue queue(2);

        // Act
        bool const first = queue.push(SteeringMessage{"make it taller", {}, {}});
        bool const second = queue.push(SteeringMessage{"use oak", {}, {}});
        bool const third = queue.push(SteeringMessage{"never mind", {}, {}});

        // Assert
        EXPECT_TRUE(first);
        EXPECT_TRUE(second);
        EXPECT_FALSE(third);
        EXPECT_EQ(queue.size(), 2u);
    }

    TEST(SteeringQueue, Drain_WithTwoMessages_ReturnsSubmissionOrderAndEmpties)
    {
        // Arrange
        SteeringQueue queue;
        (void) queue.push(SteeringMessage{"first", {}, {}});
        (void) queue.push(SteeringMessage{"second", {}, {}});

        // Act
        auto const drained = queue.drain();
        auto const after = queue.drain();

        // Assert
        ASSERT_EQ(drained.size(), 2u);
        EXPECT_EQ(drained[0].text, "first");
        EXPECT_EQ(drained[1].text, "second");
        EXPECT_NE(drained[0].submittedAt, std::chrono::steady_clock::time_point{});
        EXPECT_TRUE(after.empty());
        EXPECT_TRUE(queue.empty());
    }

    TEST(SteeringQueue, Abandon_WithQueuedMessages_LeavesQueueReusable)
    {
        // Arrange
        SteeringQueue queue;
        (void) queue.push(SteeringMessage{"stop this", {}, {}});

        // Act
        queue.abandon();

        // Assert
        EXPECT_TRUE(queue.empty());
        EXPECT_TRUE(queue.push(SteeringMessage{"next run", {}, {}}));
    }

    TEST(SteeringQueue, PushImages_WithReferenceImage_KeepsBytesForInjection)
    {
        // Arrange
        SteeringQueue queue;
        SteeringMessage steer;
        steer.text = "like this";
        steer.images.emplace_back(ImageContent{
          std::vector<std::byte>{std::byte{0x12}, std::byte{0x34}}, "image/png"});

        // Act
        (void) queue.push(std::move(steer));
        auto const drained = queue.drain();

        // Assert
        ASSERT_EQ(drained.size(), 1u);
        ASSERT_EQ(drained[0].images.size(), 1u);
        EXPECT_EQ(drained[0].images[0].mimeType, "image/png");
        EXPECT_EQ(drained[0].images[0].data.size(), 2u);
    }

    TEST(SteeringQueue, ConcurrentPushFromSeveralThreads_KeepsEveryMessageUpToTheCap)
    {
        // Arrange: the UI thread and any number of host threads may submit while the run is going
        SteeringQueue queue(kMaxSteersPerRun);
        std::atomic<int> accepted = 0;
        std::vector<std::thread> writers;
        writers.reserve(4);
        for (int t = 0; t < 4; ++t)
        {
            writers.emplace_back([&queue, &accepted, t]() {
                for (int i = 0; i < 4; ++i)
                {
                    if (queue.push(SteeringMessage{"t" + std::to_string(t) + "-" + std::to_string(i),
                                                   {},
                                                   {}}))
                    {
                        ++accepted;
                    }
                }
            });
        }

        // Act
        for (auto & writer : writers)
        {
            writer.join();
        }

        // Assert: no message is lost or double counted, and the cap is never exceeded
        EXPECT_EQ(static_cast<std::size_t>(accepted.load()), queue.size());
        EXPECT_LE(queue.size(), kMaxSteersPerRun);
        EXPECT_EQ(queue.drain().size(), queue.capacity());
    }
} // namespace gladius::agent::tests
