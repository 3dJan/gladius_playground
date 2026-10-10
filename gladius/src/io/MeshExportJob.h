#pragma once

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>

namespace gladius::io
{
  using MeshExportCancellationPredicate = std::function<bool()>;
  using MeshExportProgressCallback = std::function<void(double, std::string_view)>;

    enum class MeshExportJobState
    {
        Running,
        Cancelling,
        Completed,
        Cancelled,
        Failed
    };

    struct MeshExportJobStatus
    {
        MeshExportJobState state{MeshExportJobState::Running};
        double progress{0.0};
        std::string phase{"Starting export"};
        std::string errorMessage;
    };

    class MeshExportCancelled final : public std::exception
    {
      public:
        [[nodiscard]] char const* what() const noexcept override
        {
            return "Mesh export was cancelled";
        }
    };

    /// Backend-neutral polling and cancellation contract for mesh export jobs.
    class MeshExportJob
    {
      public:
        virtual ~MeshExportJob() = default;

        virtual void poll() = 0;
        virtual void requestCancellation() = 0;
        [[nodiscard]] virtual MeshExportJobStatus getStatus() const = 0;
    };

    /// Future-backed job implementation for exporters that execute as one worker task.
    class AsyncMeshExportJob final : public MeshExportJob
    {
      public:
        using CancellationPredicate = MeshExportCancellationPredicate;
        using ProgressCallback = MeshExportProgressCallback;
        using Work = std::function<void(CancellationPredicate const&, ProgressCallback const&)>;

        explicit AsyncMeshExportJob(Work work);
        ~AsyncMeshExportJob() override;

        AsyncMeshExportJob(AsyncMeshExportJob const&) = delete;
        AsyncMeshExportJob& operator=(AsyncMeshExportJob const&) = delete;
        AsyncMeshExportJob(AsyncMeshExportJob&&) = delete;
        AsyncMeshExportJob& operator=(AsyncMeshExportJob&&) = delete;

        void poll() override;
        void requestCancellation() override;
        [[nodiscard]] MeshExportJobStatus getStatus() const override;

        /// Wait for the worker and collect its final state. Intended for teardown and tests.
        void wait();

      private:
        struct SharedState;

        std::shared_ptr<SharedState> m_sharedState;
        std::future<void> m_worker;
    };
} // namespace gladius::io
