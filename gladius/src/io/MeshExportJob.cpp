#include "MeshExportJob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gladius::io
{
    struct AsyncMeshExportJob::SharedState
    {
        std::atomic<MeshExportJobState> state{MeshExportJobState::Running};
        std::atomic<double> progress{0.0};
        mutable std::mutex mutex;
        std::string phase{"Starting export"};
        std::string errorMessage;
    };

    AsyncMeshExportJob::AsyncMeshExportJob(Work work)
        : m_sharedState(std::make_shared<SharedState>())
    {
        if (!work)
        {
            throw std::invalid_argument("Mesh export job requires a work function");
        }

        auto const state = m_sharedState;
        m_worker = std::async(std::launch::async, [state, work = std::move(work)]() mutable {
            CancellationPredicate const shouldCancel = [state]() {
                return state->state.load(std::memory_order_acquire) ==
                       MeshExportJobState::Cancelling;
            };
            ProgressCallback const reportProgress = [state](double progress,
                                                              std::string_view phase) {
                if (!std::isfinite(progress))
                {
                    return;
                }
                state->progress.store(std::clamp(progress, 0.0, 1.0),
                                      std::memory_order_release);
                std::lock_guard<std::mutex> lock(state->mutex);
                state->phase.assign(phase);
            };

            work(shouldCancel, reportProgress);
        });
    }

    AsyncMeshExportJob::~AsyncMeshExportJob()
    {
        requestCancellation();
        wait();
    }

    void AsyncMeshExportJob::poll()
    {
        if (!m_worker.valid() ||
            m_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }

        std::string errorMessage;
        bool wasCancelled = false;
        try
        {
            m_worker.get();
        }
        catch (MeshExportCancelled const&)
        {
            wasCancelled = true;
        }
        catch (std::exception const& error)
        {
            errorMessage = error.what();
        }
        catch (...)
        {
            errorMessage = "Unknown error while exporting mesh";
        }

        MeshExportJobState finalState = MeshExportJobState::Completed;
        std::string phase{"Export complete"};
        if (wasCancelled)
        {
            finalState = MeshExportJobState::Cancelled;
            phase = "Export cancelled";
        }
        else if (!errorMessage.empty())
        {
            finalState = MeshExportJobState::Failed;
            phase = "Export failed";
        }
        else
        {
            m_sharedState->progress.store(1.0, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(m_sharedState->mutex);
            m_sharedState->phase = std::move(phase);
            m_sharedState->errorMessage = std::move(errorMessage);
        }
        m_sharedState->state.store(finalState, std::memory_order_release);
    }

    void AsyncMeshExportJob::requestCancellation()
    {
        MeshExportJobState expected = MeshExportJobState::Running;
        (void)m_sharedState->state.compare_exchange_strong(
            expected, MeshExportJobState::Cancelling, std::memory_order_acq_rel);
    }

    MeshExportJobStatus AsyncMeshExportJob::getStatus() const
    {
        MeshExportJobStatus result;
        result.state = m_sharedState->state.load(std::memory_order_acquire);
        result.progress = m_sharedState->progress.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(m_sharedState->mutex);
            result.phase = m_sharedState->phase;
            result.errorMessage = m_sharedState->errorMessage;
        }
        return result;
    }

    void AsyncMeshExportJob::wait()
    {
        if (m_worker.valid())
        {
            m_worker.wait();
            poll();
        }
    }
} // namespace gladius::io
