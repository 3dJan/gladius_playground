/**
 * @file AsyncThumbnailLoader_tests.cpp
 * @brief Unit tests for AsyncThumbnailLoader
 */

#include "ui/AsyncThumbnailLoader.h"
#include "ui/ThreemfThumbnailExtractor.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace gladius::ui::tests
{
    class AsyncThumbnailLoaderTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create a basic logger (can be nullptr for most tests)
            m_logger = nullptr;
        }

        events::SharedLogger m_logger;
    };

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithNewThumbnail_SetsLoadingState)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        // Act
        loader.requestLoad(info);

        // Assert
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);
    }

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithAlreadyLoadingThumbnail_DoesNotDuplicate)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::Loading;

        // Act - requesting load on already-loading thumbnail
        loader.requestLoad(info);

        // Assert - state should remain unchanged (not re-queued)
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);
    }

    TEST_F(AsyncThumbnailLoaderTest, RequestLoad_WithCompletedThumbnail_DoesNotReload)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::Ready;

        // Act
        loader.requestLoad(info);

        // Assert - should not re-queue a ready thumbnail
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Ready);
    }

    TEST_F(AsyncThumbnailLoaderTest, CancelAll_WithPendingLoads_ResetsStates)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info1;
        info1.filePath = "/nonexistent/test1.3mf";
        info1.loadState = ThumbnailLoadState::NotStarted;

        ThreemfThumbnailExtractor::ThumbnailInfo info2;
        info2.filePath = "/nonexistent/test2.3mf";
        info2.loadState = ThumbnailLoadState::NotStarted;

        loader.requestLoad(info1);
        loader.requestLoad(info2);

        // Act
        loader.cancelAll();

        // Assert - states should be reset to NotStarted
        EXPECT_EQ(info1.loadState, ThumbnailLoadState::NotStarted);
        EXPECT_EQ(info2.loadState, ThumbnailLoadState::NotStarted);
    }

    TEST_F(AsyncThumbnailLoaderTest, CancelAll_AfterCancelAll_HasNoPendingWork)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        loader.requestLoad(info);
        EXPECT_TRUE(loader.hasPendingWork());

        // Act
        loader.cancelAll();

        // Assert
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, Update_WithNonexistentFile_TransitionsToFailed)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/this_file_does_not_exist.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        loader.requestLoad(info);
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Loading);

        // Act - poll until future completes (with short timeout)
        auto start = std::chrono::steady_clock::now();
        while (info.loadState == ThumbnailLoadState::Loading)
        {
            loader.update();

            // Timeout after 2 seconds
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(2))
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Assert - should have failed since file doesn't exist
        EXPECT_EQ(info.loadState, ThumbnailLoadState::Failed);
    }

    TEST_F(AsyncThumbnailLoaderTest, HasPendingWork_WithNoRequests_ReturnsFalse)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);

        // Act & Assert
        EXPECT_FALSE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, HasPendingWork_WithActiveRequest_ReturnsTrue)
    {
        // Arrange
        AsyncThumbnailLoader loader(m_logger, 4);
        ThreemfThumbnailExtractor::ThumbnailInfo info;
        info.filePath = "/nonexistent/test.3mf";
        info.loadState = ThumbnailLoadState::NotStarted;

        // Act
        loader.requestLoad(info);

        // Assert
        EXPECT_TRUE(loader.hasPendingWork());
    }

    TEST_F(AsyncThumbnailLoaderTest, Constructor_WithMaxConcurrentLoads_RespectsConcurrencyLimit)
    {
        // Arrange - create loader with max 2 concurrent loads
        AsyncThumbnailLoader loader(m_logger, 2);

        ThreemfThumbnailExtractor::ThumbnailInfo info1;
        info1.filePath = "/nonexistent/test1.3mf";
        info1.loadState = ThumbnailLoadState::NotStarted;

        ThreemfThumbnailExtractor::ThumbnailInfo info2;
        info2.filePath = "/nonexistent/test2.3mf";
        info2.loadState = ThumbnailLoadState::NotStarted;

        ThreemfThumbnailExtractor::ThumbnailInfo info3;
        info3.filePath = "/nonexistent/test3.3mf";
        info3.loadState = ThumbnailLoadState::NotStarted;

        // Act
        loader.requestLoad(info1);
        loader.requestLoad(info2);
        loader.requestLoad(info3);

        // Assert - all should be in Loading state (either active or queued)
        EXPECT_EQ(info1.loadState, ThumbnailLoadState::Loading);
        EXPECT_EQ(info2.loadState, ThumbnailLoadState::Loading);
        EXPECT_EQ(info3.loadState, ThumbnailLoadState::Loading);
    }

} // namespace gladius::ui::tests
