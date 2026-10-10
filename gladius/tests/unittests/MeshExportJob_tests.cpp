#include "io/MeshExportJob.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

namespace gladius::tests
{
    namespace
    {
        compute::AsyncTask<void> reportingWithYields(io::MeshExportCancellationPredicate, io::MeshExportProgressCallback reportProgress)
        {
            reportProgress(0.25, "Meshing");
            co_await compute::AsyncYield{};
            reportProgress(0.5, "Writing");
            co_await compute::AsyncYield{};
        }

        compute::AsyncTask<void> cancellableLoop(io::MeshExportCancellationPredicate shouldCancel, io::MeshExportProgressCallback)
        {
            while (!shouldCancel())
            {
                co_await compute::AsyncYield{};
            }
            throw io::MeshExportCancelled{};
        }

        compute::AsyncTask<void> throwsAfterYield(io::MeshExportCancellationPredicate, io::MeshExportProgressCallback)
        {
            co_await compute::AsyncYield{};
            throw std::runtime_error("mesh writer failed");
        }

        compute::AsyncTask<void> reportsOutOfRangeProgress(io::MeshExportCancellationPredicate, io::MeshExportProgressCallback reportProgress)
        {
            reportProgress(2.0, "Overshoot");
            co_await compute::AsyncYield{};
            reportProgress(std::numeric_limits<double>::quiet_NaN(), "Invalid");
            co_await compute::AsyncYield{};
        }
    } // namespace

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

    TEST(SteppedMeshExportJob, PollWithYieldingTask_ReportsProgressAndCompletes)
    {
        io::SteppedMeshExportJob job(reportingWithYields);

        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Running);
        EXPECT_DOUBLE_EQ(job.getStatus().progress, 0.0);

        job.poll();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Running);
        EXPECT_DOUBLE_EQ(job.getStatus().progress, 0.25);
        EXPECT_EQ(job.getStatus().phase, "Meshing");

        job.poll();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Running);
        EXPECT_DOUBLE_EQ(job.getStatus().progress, 0.5);
        EXPECT_EQ(job.getStatus().phase, "Writing");

        job.poll();
        auto const completedStatus = job.getStatus();
        EXPECT_EQ(completedStatus.state, io::MeshExportJobState::Completed);
        EXPECT_DOUBLE_EQ(completedStatus.progress, 1.0);
        EXPECT_EQ(completedStatus.phase, "Export complete");
        EXPECT_TRUE(completedStatus.errorMessage.empty());
    }

    TEST(SteppedMeshExportJob, CancellationDuringTask_ReportsCancelled)
    {
        io::SteppedMeshExportJob job(cancellableLoop);

        job.poll();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Running);

        job.requestCancellation();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Cancelling);

        job.poll();
        auto const cancelledStatus = job.getStatus();
        EXPECT_EQ(cancelledStatus.state, io::MeshExportJobState::Cancelled);
        EXPECT_EQ(cancelledStatus.phase, "Export cancelled");
        EXPECT_TRUE(cancelledStatus.errorMessage.empty());
    }

    TEST(SteppedMeshExportJob, TaskThrows_ReportsFailure)
    {
        io::SteppedMeshExportJob job(throwsAfterYield);

        job.poll();
        EXPECT_EQ(job.getStatus().state, io::MeshExportJobState::Running);

        job.poll();
        auto const failedStatus = job.getStatus();
        EXPECT_EQ(failedStatus.state, io::MeshExportJobState::Failed);
        EXPECT_EQ(failedStatus.phase, "Export failed");
        EXPECT_EQ(failedStatus.errorMessage, "mesh writer failed");
    }

    TEST(SteppedMeshExportJob, ProgressOutOfRange_IsClampedAndNonFiniteIgnored)
    {
        io::SteppedMeshExportJob job(reportsOutOfRangeProgress);

        job.poll();
        EXPECT_DOUBLE_EQ(job.getStatus().progress, 1.0);
        EXPECT_EQ(job.getStatus().phase, "Overshoot");

        job.poll();
        EXPECT_DOUBLE_EQ(job.getStatus().progress, 1.0);
        EXPECT_EQ(job.getStatus().phase, "Overshoot");
    }

    TEST(SteppedMeshExportJob, NullFactory_ThrowsInvalidArgument)
    {
        EXPECT_THROW(io::SteppedMeshExportJob job(io::SteppedMeshExportJob::TaskFactory{}), std::invalid_argument);
    }
} // namespace gladius::tests
