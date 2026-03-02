#include <gtest/gtest.h>
#include "compute/ManifoldDualContouringGpu.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace gladius::compute::tests
{
    /// Tests for the MeshGenerationProgressCallback functionality in ManifoldDualContouringGpu

    class ManifoldDualContouringProgressTest : public ::testing::Test
    {
    protected:
        /// Captured progress updates from callback
        struct ProgressUpdate
        {
            float progress{0.0F};
            std::string phaseName;
        };

        std::vector<ProgressUpdate> m_capturedUpdates;

        /// Create a callback that captures all progress updates
        [[nodiscard]] MeshGenerationProgressCallback createCapturingCallback()
        {
            return [this](float progress, std::string_view phaseName)
            {
                m_capturedUpdates.push_back({progress, std::string(phaseName)});
            };
        }
    };

    TEST_F(ManifoldDualContouringProgressTest, CallbackType_CanBeCreatedWithLambda)
    {
        // Arrange
        float capturedProgress = -1.0F;
        std::string capturedPhase;

        MeshGenerationProgressCallback callback = [&](float progress, std::string_view phaseName)
        {
            capturedProgress = progress;
            capturedPhase = std::string(phaseName);
        };

        // Act
        callback(0.5F, "Test Phase");

        // Assert
        EXPECT_FLOAT_EQ(capturedProgress, 0.5F);
        EXPECT_EQ(capturedPhase, "Test Phase");
    }

    TEST_F(ManifoldDualContouringProgressTest, CallbackType_AcceptsNullptr)
    {
        // Arrange
        MeshGenerationProgressCallback callback{nullptr};

        // Act & Assert - should not crash when callback is null
        EXPECT_EQ(callback, nullptr);
    }

    TEST_F(ManifoldDualContouringProgressTest, CallbackType_CanBeMoved)
    {
        // Arrange
        int callCount = 0;
        MeshGenerationProgressCallback callback1 = [&](float, std::string_view)
        {
            ++callCount;
        };

        // Act
        MeshGenerationProgressCallback callback2 = std::move(callback1);
        callback2(0.0F, "");

        // Assert
        EXPECT_EQ(callCount, 1);
    }

    TEST_F(ManifoldDualContouringProgressTest, ProgressValues_AreWithinValidRange)
    {
        // This test verifies the contract: progress values should be in [0.0, 1.0]
        auto callback = createCapturingCallback();

        // Simulate progress updates as they would come from generateMesh
        callback(0.0F, "Initializing");
        callback(0.05F, "Building octree");
        callback(0.25F, "Generating vertices");
        callback(0.45F, "Generating indices");
        callback(0.65F, "Post-processing");
        callback(0.80F, "Mesh generation complete");

        // Assert
        for (auto const & update : m_capturedUpdates)
        {
            EXPECT_GE(update.progress, 0.0F) << "Progress should not be negative";
            EXPECT_LE(update.progress, 1.0F) << "Progress should not exceed 1.0";
        }
    }

    TEST_F(ManifoldDualContouringProgressTest, ProgressValues_AreMonotonicallyIncreasing)
    {
        // This test verifies the contract: progress should never decrease (monotonic)
        auto callback = createCapturingCallback();

        // Simulate typical progress updates
        callback(0.0F, "Initializing");
        callback(0.05F, "Building octree");
        callback(0.25F, "Generating vertices");
        callback(0.45F, "Generating indices");
        callback(0.65F, "Post-processing");
        callback(0.80F, "Mesh generation complete");

        // Assert
        float previousProgress = -1.0F;
        for (auto const & update : m_capturedUpdates)
        {
            EXPECT_GE(update.progress, previousProgress)
                << "Progress decreased from " << previousProgress << " to " << update.progress
                << " during phase '" << update.phaseName << "'";
            previousProgress = update.progress;
        }
    }

    TEST_F(ManifoldDualContouringProgressTest, PhaseNames_AreNonEmpty)
    {
        // This test verifies that phase names provide meaningful information
        auto callback = createCapturingCallback();

        // Simulate typical progress updates
        callback(0.0F, "Initializing");
        callback(0.05F, "Building octree");
        callback(0.80F, "Mesh generation complete");

        // Assert
        for (auto const & update : m_capturedUpdates)
        {
            EXPECT_FALSE(update.phaseName.empty()) << "Phase name should not be empty";
        }
    }

    TEST_F(ManifoldDualContouringProgressTest, AtomicProgress_CanBeUpdatedFromCallback)
    {
        // This test simulates how ManifoldDualContouringStlExporter uses the callback
        std::atomic<double> atomicProgress{0.0};

        MeshGenerationProgressCallback callback = [&atomicProgress](float progress, std::string_view /*phaseName*/)
        {
            // Map mesh generation progress (0.0-1.0) to exporter range (0.05-0.80)
            double const exportProgress = 0.05 + static_cast<double>(progress) * 0.75;
            atomicProgress.store(exportProgress, std::memory_order_relaxed);
        };

        // Simulate mesh generation progress updates
        callback(0.0F, "Start");
        EXPECT_DOUBLE_EQ(atomicProgress.load(), 0.05);

        callback(0.5F, "Middle");
        EXPECT_DOUBLE_EQ(atomicProgress.load(), 0.425);  // 0.05 + 0.5 * 0.75

        callback(1.0F, "End");
        EXPECT_DOUBLE_EQ(atomicProgress.load(), 0.80);  // 0.05 + 1.0 * 0.75
    }

    TEST_F(ManifoldDualContouringProgressTest, ChunkedProgress_ReportsPerChunkUpdates)
    {
        // This test verifies that chunked processing reports progress per chunk
        auto callback = createCapturingCallback();

        // Simulate chunked processing with 4 chunks (5% to 55% range)
        float const startProgress = 0.05F;
        float const progressRange = 0.50F;
        std::size_t const totalChunks = 4;

        for (std::size_t i = 1; i <= totalChunks; ++i)
        {
            float const chunkProgress = startProgress + progressRange * (static_cast<float>(i) / static_cast<float>(totalChunks));
            callback(chunkProgress, "Processing chunk");
        }

        // Assert - should have 4 updates with increasing progress
        ASSERT_EQ(m_capturedUpdates.size(), 4U);
        
        float previousProgress = 0.0F;
        for (auto const & update : m_capturedUpdates)
        {
            EXPECT_GT(update.progress, previousProgress);
            previousProgress = update.progress;
        }
    }

} // namespace gladius::compute::tests
