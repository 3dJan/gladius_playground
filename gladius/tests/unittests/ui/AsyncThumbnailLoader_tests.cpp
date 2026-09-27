/**
 * @file AsyncThumbnailLoader_tests.cpp
 * @brief Unit tests for AsyncThumbnailLoader
 */

#include "ui/AsyncThumbnailLoader.h"
#include "ui/ThreemfThumbnailExtractor.h"
#include "io/3mf/Lib3mfLoader.h"

#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace gladius::ui::tests
{
    class AsyncThumbnailLoaderTest : public ::testing::Test
    {
      protected:
        struct WorkerGate
        {
            std::mutex mutex;
            std::condition_variable condition;
            std::vector<std::filesystem::path> started;
            bool releaseFirst = false;
        };

        void SetUp() override
        {
            m_logger = nullptr;
        }

        static ThumbnailLoadResult makeResult(bool success)
        {
            ThumbnailLoadResult result;
            result.success = success;
            if (success)
            {
                result.decodedPixels = {0u, 0u, 0u, 255u};
                result.width = 1;
                result.height = 1;
            }
            return result;
        }

        static bool waitForStarted(const std::shared_ptr<WorkerGate> & gate, size_t count)
        {
            std::unique_lock lock(gate->mutex);
            return gate->condition.wait_for(lock,
                                            std::chrono::seconds(2),
                                            [&gate, count] { return gate->started.size() >= count; });
        }

        static void releaseFirst(const std::shared_ptr<WorkerGate> & gate)
        {
            {
                std::lock_guard lock(gate->mutex);
                gate->releaseFirst = true;
            }
            gate->condition.notify_all();
        }

        static std::vector<ThumbnailLoadCompletion>
        waitForCompletions(AsyncThumbnailLoader & loader, size_t count)
        {
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::vector<ThumbnailLoadCompletion> completions;
            while (completions.size() < count && std::chrono::steady_clock::now() < deadline)
            {
                auto batch = loader.update();
                if (batch.empty())
                {
                    std::this_thread::yield();
                    continue;
                }
                for (auto & completion : batch)
                {
                    completions.push_back(std::move(completion));
                }
            }
            return completions;
        }

        events::SharedLogger m_logger;
    };

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithNewThumbnail_SetsLoadingState)
    {
        // Arrange
                AsyncThumbnailLoader loader(
                    m_logger, 1, [](std::filesystem::path const &) { return ThumbnailLoadResult{}; });
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        // Act
        auto const requestId = loader.requestLoad(info);

        // Assert
        EXPECT_NE(requestId, 0u);
        EXPECT_EQ(info.loadRequestId, requestId);
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);
    }

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithAlreadyLoadingThumbnail_DoesNotDuplicate)
    {
        // Arrange
                AsyncThumbnailLoader loader(
                    m_logger, 1, [](std::filesystem::path const &) { return ThumbnailLoadResult{}; });
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::Loading;
        info.loadRequestId = 42;

        // Act - requesting load on already-loading thumbnail
        auto const requestId = loader.requestLoad(info);

        // Assert - state should remain unchanged (not re-queued)
        EXPECT_EQ(requestId, 42u);
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithCompletedThumbnail_DoesNotReload)
    {
        // Arrange
        AsyncThumbnailLoader loader(
          m_logger, 1, [](std::filesystem::path const &) { return ThumbnailLoadResult{}; });
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::Ready;

        // Act
        auto const requestId = loader.requestLoad(info);

        // Assert - should not re-queue a ready thumbnail
        EXPECT_EQ(requestId, 0u);
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Ready);
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, SequentialLoads_PrioritizeRecentsAndContinueAfterFailure)
    {
        auto gate = std::make_shared<WorkerGate>();
        auto const firstPath = std::filesystem::path("first.3mf");
        auto const examplePath = std::filesystem::path("example.3mf");
        auto const firstRecentPath = std::filesystem::path("recent-one.3mf");
        auto const secondRecentPath = std::filesystem::path("recent-two.3mf");
        ThumbnailLoadFunction loadFunction = [gate, firstPath](std::filesystem::path const & path)
        {
            {
                std::unique_lock lock(gate->mutex);
                gate->started.push_back(path);
                gate->condition.notify_all();
                if (path == firstPath)
                {
                    gate->condition.wait(lock, [&gate] { return gate->releaseFirst; });
                }
            }
            return makeResult(path != firstPath);
        };
        AsyncThumbnailLoader loader(m_logger, 1, std::move(loadFunction));

        ThreemfThumbnailExtractor::ThumbnailInfo first;
        first.filePath = firstPath;
        ThreemfThumbnailExtractor::ThumbnailInfo example;
        example.filePath = examplePath;
        ThreemfThumbnailExtractor::ThumbnailInfo firstRecent;
        firstRecent.filePath = firstRecentPath;
        ThreemfThumbnailExtractor::ThumbnailInfo secondRecent;
        secondRecent.filePath = secondRecentPath;

        auto const firstId = loader.requestLoad(first);
        loader.requestLoad(example);
        auto const firstRecentId = loader.requestLoad(firstRecent, ThumbnailLoadPriority::High);
        auto const secondRecentId = loader.requestLoad(secondRecent, ThumbnailLoadPriority::High);

        bool const firstStarted = waitForStarted(gate, 1);
        if (!firstStarted)
        {
            releaseFirst(gate);
        }
        ASSERT_TRUE(firstStarted);

        std::vector<std::filesystem::path> started;
        {
            std::lock_guard lock(gate->mutex);
            started = gate->started;
        }

        releaseFirst(gate);
        EXPECT_EQ(started, (std::vector<std::filesystem::path>{firstPath}));
        auto firstCompletion = waitForCompletions(loader, 1);
        ASSERT_EQ(firstCompletion.size(), 1u);
        EXPECT_EQ(firstCompletion.front().requestId, firstId);
        EXPECT_FALSE(firstCompletion.front().result.success);

        ASSERT_TRUE(waitForStarted(gate, 2));
        {
            std::lock_guard lock(gate->mutex);
            EXPECT_EQ(gate->started[1], firstRecentPath);
        }
        auto firstRecentCompletion = waitForCompletions(loader, 1);
        ASSERT_EQ(firstRecentCompletion.size(), 1u);
        EXPECT_EQ(firstRecentCompletion.front().requestId, firstRecentId);
        EXPECT_TRUE(firstRecentCompletion.front().result.success);

        ASSERT_TRUE(waitForStarted(gate, 3));
        {
            std::lock_guard lock(gate->mutex);
            EXPECT_EQ(gate->started[2], secondRecentPath);
        }
        auto secondRecentCompletion = waitForCompletions(loader, 1);
        ASSERT_EQ(secondRecentCompletion.size(), 1u);
        EXPECT_EQ(secondRecentCompletion.front().requestId, secondRecentId);

        ASSERT_TRUE(waitForStarted(gate, 4));
        {
            std::lock_guard lock(gate->mutex);
            EXPECT_EQ(gate->started[3], examplePath);
        }
        EXPECT_EQ(waitForCompletions(loader, 1).size(), 1u);
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, CancelRequest_WithActiveAndQueuedWork_DiscardsStaleResult)
    {
        auto gate = std::make_shared<WorkerGate>();
        auto const activePath = std::filesystem::path("active.3mf");
        auto const queuedPath = std::filesystem::path("queued.3mf");
        ThumbnailLoadFunction loadFunction = [gate, activePath](std::filesystem::path const & path)
        {
            {
                std::unique_lock lock(gate->mutex);
                gate->started.push_back(path);
                gate->condition.notify_all();
                if (path == activePath)
                {
                    gate->condition.wait(lock, [&gate] { return gate->releaseFirst; });
                }
            }
            return makeResult(true);
        };
        AsyncThumbnailLoader loader(m_logger, 1, std::move(loadFunction));

        ThreemfThumbnailExtractor::ThumbnailInfo active;
        active.filePath = activePath;
        ThreemfThumbnailExtractor::ThumbnailInfo queued;
        queued.filePath = queuedPath;
        auto const activeId = loader.requestLoad(active);
        auto const queuedId = loader.requestLoad(queued);

        bool const activeStarted = waitForStarted(gate, 1);
        if (!activeStarted)
        {
            releaseFirst(gate);
        }
        ASSERT_TRUE(activeStarted);

        auto cancelFuture = std::async(std::launch::async,
                                       [&loader, activeId] { loader.cancelRequest(activeId); });
        auto const cancelStatus = cancelFuture.wait_for(std::chrono::milliseconds(100));
        releaseFirst(gate);
        cancelFuture.wait();
        cancelFuture.get();
        EXPECT_EQ(cancelStatus, std::future_status::ready);

        auto completions = waitForCompletions(loader, 1);
        ASSERT_EQ(completions.size(), 1u);
        EXPECT_EQ(completions.front().requestId, queuedId);
        EXPECT_NE(completions.front().requestId, activeId);
    }

    TEST_F(AsyncThumbnailLoaderTest, CancelAll_WithActiveWorker_ReturnsWithoutWaiting)
    {
        auto gate = std::make_shared<WorkerGate>();
        ThumbnailLoadFunction loadFunction = [gate](std::filesystem::path const &)
        {
            std::unique_lock lock(gate->mutex);
            gate->started.emplace_back("blocked.3mf");
            gate->condition.notify_all();
            gate->condition.wait(lock, [&gate] { return gate->releaseFirst; });
            return makeResult(true);
        };
        AsyncThumbnailLoader loader(m_logger, 1, std::move(loadFunction));
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "blocked.3mf";
        loader.requestLoad(info);

        bool const workerStarted = waitForStarted(gate, 1);
        if (!workerStarted)
        {
            releaseFirst(gate);
        }
        ASSERT_TRUE(workerStarted);

        auto cancelFuture = std::async(std::launch::async, [&loader] { loader.cancelAll(); });
        auto const cancelStatus = cancelFuture.wait_for(std::chrono::milliseconds(100));
        releaseFirst(gate);
        cancelFuture.wait();
        cancelFuture.get();
        EXPECT_EQ(cancelStatus, std::future_status::ready);
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, HasPendingWork_WithNoRequests_ReturnsFalse)
    {
        // Arrange
        AsyncThumbnailLoader loader(
          m_logger, 1, [](std::filesystem::path const &) { return ThumbnailLoadResult{}; });

        // Act & Assert
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, HasPendingWork_WithActiveRequest_ReturnsTrue)
    {
        // Arrange
                AsyncThumbnailLoader loader(
                    m_logger, 1, [](std::filesystem::path const &) { return ThumbnailLoadResult{}; });
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        // Act
        loader.requestLoad(info);

        // Assert
        EXPECT_TRUE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, Update_WithFailedExtraction_ReturnsValueWithoutTouchingUiState)
    {
        AsyncThumbnailLoader loader(
          m_logger,
          1,
          [](std::filesystem::path const &)
          {
              ThumbnailLoadResult result;
              result.errorMessage = "expected failure";
              return result;
          });
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "missing.3mf";
        auto const requestId = loader.requestLoad(info);

        auto completions = waitForCompletions(loader, 1);

        ASSERT_EQ(completions.size(), 1u);
        EXPECT_EQ(completions.front().requestId, requestId);
        EXPECT_FALSE(completions.front().result.success);
        EXPECT_EQ(completions.front().result.errorMessage, "expected failure");
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);
    }

    TEST_F(AsyncThumbnailLoaderTest, ApplyAsyncLoadResult_WithDecodedPixels_TransitionsToPendingTexture)
    {
        ThreemfThumbnailExtractor extractor(m_logger);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.loadState = ThumbnailLoadState::Loading;
        info.loadRequestId = 17;

        auto result = makeResult(true);
        result.fileSize = 128;
        result.metadata.emplace_back("Title", "Test thumbnail");

        extractor.applyAsyncLoadResult(info, std::move(result));

        EXPECT_EQ(info.loadState, ThumbnailLoadState::DecodedPending);
        EXPECT_EQ(info.loadRequestId, 0u);
        EXPECT_TRUE(info.thumbnailLoaded);
        EXPECT_TRUE(info.hasThumbnail);
        EXPECT_EQ(info.decodedPixels.size(), 4u);
        EXPECT_EQ(info.fileInfo.fileSize, 128u);
        EXPECT_EQ(info.fileInfo.getMetadata("Title"), "Test thumbnail");
    }

    TEST_F(AsyncThumbnailLoaderTest, LoadLib3mfScoped_ConcurrentCallsPreserveCurrentDirectory)
    {
        auto const originalDirectory = std::filesystem::current_path();
        std::vector<std::future<Lib3MF::PWrapper>> wrapperLoads;
        for (size_t index = 0; index < 4; ++index)
        {
            wrapperLoads.push_back(std::async(std::launch::async,
                                              [] { return gladius::io::loadLib3mfScoped(); }));
        }

        for (auto & wrapperLoad : wrapperLoads)
        {
            EXPECT_NE(wrapperLoad.get(), nullptr);
        }
        EXPECT_EQ(std::filesystem::current_path(), originalDirectory);
    }

} // namespace gladius::ui::tests
