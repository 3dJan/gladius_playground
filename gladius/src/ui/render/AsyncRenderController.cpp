#include "AsyncRenderController.h"

#include <ConfigManager.h>
#include <coro/queue.hpp>
#include <coro/sync_wait.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace gladius::ui::async_rendering
{
    namespace
    {
        constexpr char const * IMPLEMENTATION_KEY = "impl";
        constexpr char const * ENABLED_KEY = "enabled";
        constexpr char const * BUFFERING_KEY = "buffering";
        constexpr char const * SECTION = "asyncRendering";

        struct AsyncDefaults
        {
            static constexpr bool kEnabled = true;
            static constexpr char const * kImplementation = "coroutines";
            static constexpr char const * kBufferingMode = "double";
        };

        struct ControllerStateData
        {
            std::shared_ptr<coro::thread_pool> workerPool;
            coro::queue<RenderJob> jobQueue;
            std::atomic<bool> shutdownRequested{false};
            std::atomic<uint64_t> latestEpoch{0};
            AsyncRenderController::JobExecutor jobExecutor;

            std::mutex resultMutex;
            std::optional<FrameResultMeta> pendingResult;
            std::atomic<bool> hasPendingResult{false};

            std::mutex lifecycleMutex;
            std::condition_variable lifecycleCv;
            bool workerActive{false};

            explicit ControllerStateData(std::shared_ptr<coro::thread_pool> pool)
                : workerPool(std::move(pool))
            {
            }
        };
    }

    struct AsyncRenderController::ControllerState
    {
        explicit ControllerState(std::shared_ptr<coro::thread_pool> pool)
            : data(std::move(pool))
        {
        }

        ControllerStateData data;
    };

    AsyncRenderFeatureConfig
      loadAsyncRenderFeatureConfig(gladius::ConfigManager const * configManager)
    {
        AsyncRenderFeatureConfig config{};
        if (configManager == nullptr)
        {
            return config;
        }

        config.enabled = configManager->getValue<bool>(SECTION, ENABLED_KEY, AsyncDefaults::kEnabled);
        config.implementation = configManager->getValue<std::string>(SECTION,
                                                                     IMPLEMENTATION_KEY,
                                                                     AsyncDefaults::kImplementation);
        config.bufferingMode = configManager->getValue<std::string>(SECTION,
                                                                    BUFFERING_KEY,
                                                                    AsyncDefaults::kBufferingMode);
        return config;
    }

    AsyncRenderController::AsyncRenderController()
        : AsyncRenderController(coro::thread_pool::make_shared(coro::thread_pool::options{.thread_count = 1}))
    {
    }

    AsyncRenderController::AsyncRenderController(std::shared_ptr<coro::thread_pool> workerPool)
    {
        if (workerPool == nullptr)
        {
            workerPool = coro::thread_pool::make_shared(coro::thread_pool::options{.thread_count = 1});
        }

        m_state = std::make_shared<ControllerState>(std::move(workerPool));
    }

    AsyncRenderController::~AsyncRenderController()
    {
        stop();
    }

    void AsyncRenderController::start()
    {
        if (!m_state)
        {
            return;
        }

        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return;
        }

        m_state->data.shutdownRequested.store(false, std::memory_order_release);
        m_state->data.latestEpoch.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> resultLock(m_state->data.resultMutex);
            m_state->data.pendingResult.reset();
            m_state->data.hasPendingResult.store(false, std::memory_order_release);
        }

        auto const spawned = m_state->data.workerPool->spawn(workerLoop(m_state));
        if (!spawned)
        {
            m_running.store(false, std::memory_order_release);
            m_state->data.shutdownRequested.store(true, std::memory_order_release);
        }
    }

    void AsyncRenderController::stop()
    {
        if (!m_state)
        {
            return;
        }

        bool const wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
        if (!wasRunning)
        {
            return;
        }

        m_state->data.shutdownRequested.store(true, std::memory_order_release);
        coro::sync_wait(m_state->data.jobQueue.shutdown());

        std::unique_lock<std::mutex> lock(m_state->data.lifecycleMutex);
        m_state->data.lifecycleCv.wait(lock, [data = &m_state->data]() { return !data->workerActive; });
    }

    void AsyncRenderController::enqueueJob(RenderJob job)
    {
        if (!m_state || !isRunning())
        {
            return;
        }

        setLatestEpoch(job.epoch);
        auto result = coro::sync_wait(m_state->data.jobQueue.push(std::move(job)));
        if (result != coro::queue_produce_result::produced)
        {
            // Queue rejected the job (likely shutting down). There is nothing else to do for now.
        }
    }

    void AsyncRenderController::setLatestEpoch(uint64_t epoch)
    {
        if (!m_state)
        {
            return;
        }

        auto & latestEpoch = m_state->data.latestEpoch;
        auto current = latestEpoch.load(std::memory_order_acquire);
        while (epoch > current &&
               !latestEpoch.compare_exchange_weak(current,
                                                  epoch,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
        {
        }
    }

    void AsyncRenderController::setJobExecutor(JobExecutor executor)
    {
        if (!m_state)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_state->data.resultMutex);
        m_state->data.jobExecutor = std::move(executor);
    }

    std::optional<FrameResultMeta> AsyncRenderController::tryConsumeResult()
    {
        if (!m_state)
        {
            return std::nullopt;
        }

        auto & data = m_state->data;
        if (!data.hasPendingResult.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(data.resultMutex);
        if (!data.pendingResult.has_value())
        {
            data.hasPendingResult.store(false, std::memory_order_release);
            return std::nullopt;
        }

        FrameResultMeta result = std::move(*data.pendingResult);
        data.pendingResult.reset();
        data.hasPendingResult.store(false, std::memory_order_release);
        return result;
    }

    bool AsyncRenderController::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    std::shared_ptr<coro::thread_pool> AsyncRenderController::workerPool() const noexcept
    {
        if (!m_state)
        {
            return nullptr;
        }
        return m_state->data.workerPool;
    }

    auto AsyncRenderController::workerLoop(std::shared_ptr<ControllerState> state) -> coro::task<void>
    {
        if (!state)
        {
            co_return;
        }

        auto * data = &state->data;
        co_await data->workerPool->schedule();

        {
            std::lock_guard<std::mutex> guard(data->lifecycleMutex);
            data->workerActive = true;
            data->lifecycleCv.notify_all();
        }

        while (!data->shutdownRequested.load(std::memory_order_acquire))
        {
            auto jobResult = co_await data->jobQueue.pop();
            if (!jobResult.has_value())
            {
                if (jobResult.error() == coro::queue_consume_result::stopped)
                {
                    break;
                }

                continue;
            }

            RenderJob const & job = jobResult.value();

            auto cancellationCheck = [data, jobEpoch = job.epoch]() -> bool
            {
                return data->shutdownRequested.load(std::memory_order_acquire) ||
                       data->latestEpoch.load(std::memory_order_acquire) > jobEpoch;
            };

            if (cancellationCheck())
            {
                continue;
            }

            if (!data->jobExecutor)
            {
                continue;
            }

            FrameResultMeta result{};
            try
            {
                result = data->jobExecutor(job, cancellationCheck);
            }
            catch (...)
            {
                result.epoch = job.epoch;
                result.frameId = job.frameHint;
                result.cancelled = true;
            }

            {
                std::lock_guard<std::mutex> guard(data->resultMutex);
                data->pendingResult = std::move(result);
                data->hasPendingResult.store(true, std::memory_order_release);
            }
        }

        {
            std::lock_guard<std::mutex> guard(data->lifecycleMutex);
            data->workerActive = false;
            data->lifecycleCv.notify_all();
        }

        co_return;
    }
}
