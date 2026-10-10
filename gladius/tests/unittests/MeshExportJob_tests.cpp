#include "io/MeshExportJob.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

namespace gladius::tests
{
    TEST(AsyncMeshExportJob, WorkReportsProgressAndCompletes)
    {
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::promise<void> continueWork;
        auto continueFuture = continueWork.get_future().share();

        io::AsyncMeshExportJob job([&](auto const&, auto const& reportProgress) {
            reportProgress(0.4, "Meshing");
            started.set_value();
            continueFuture.wait();
            reportProgress(0.8, "Writing");
        });

        startedFuture.wait();
        auto const runningStatus = job.getStatus();
        EXPECT_EQ(runningStatus.state, io::MeshExportJobState::Running);
        EXPECT_DOUBLE_EQ(runningStatus.progress, 0.4);
        EXPECT_EQ(runningStatus.phase, "Meshing");

        continueWork.set_value();
        job.wait();

        auto const completedStatus = job.getStatus();
        EXPECT_EQ(completedStatus.state, io::MeshExportJobState::Completed);
        EXPECT_DOUBLE_EQ(completedStatus.progress, 1.0);
        EXPECT_EQ(completedStatus.phase, "Export complete");
        EXPECT_TRUE(completedStatus.errorMessage.empty());
    }

    TEST(AsyncMeshExportJob, CancellationObservedByWork_ReportsCancelled)
    {
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::atomic<bool> cancellationObserved{false};

        io::AsyncMeshExportJob job([&](auto const& shouldCancel, auto const&) {
            started.set_value();
            while (!shouldCancel())
            {
                std::this_thread::yield();
            }
            cancellationObserved.store(true, std::memory_order_release);
            throw io::MeshExportCancelled{};
        });

        startedFuture.wait();
        job.requestCancellation();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Cancelling);
        job.wait();

        auto const cancelledStatus = job.getStatus();
        EXPECT_TRUE(cancellationObserved.load(std::memory_order_acquire));
        EXPECT_EQ(cancelledStatus.state, io::MeshExportJobState::Cancelled);
        EXPECT_EQ(cancelledStatus.phase, "Export cancelled");
    }

    TEST(AsyncMeshExportJob, CancellationIgnoredByWork_ReportsCompleted)
    {
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::promise<void> continueWork;
        auto continueFuture = continueWork.get_future().share();

        io::AsyncMeshExportJob job([&](auto const&, auto const&) {
            started.set_value();
            continueFuture.wait();
        });

        startedFuture.wait();
        job.requestCancellation();
        continueWork.set_value();
        job.wait();

        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Completed);
    }

    TEST(AsyncMeshExportJob, WorkThrows_ReportsFailure)
    {
        io::AsyncMeshExportJob job([](auto const&, auto const&) {
            throw std::runtime_error("mesh writer failed");
        });

        job.wait();

        auto const failedStatus = job.getStatus();
        EXPECT_EQ(failedStatus.state, io::MeshExportJobState::Failed);
        EXPECT_EQ(failedStatus.phase, "Export failed");
        EXPECT_EQ(failedStatus.errorMessage, "mesh writer failed");
    }
} // namespace gladius::tests
