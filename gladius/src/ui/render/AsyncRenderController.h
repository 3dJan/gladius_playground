#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <coro/coro.hpp>

#include "AsyncRenderTypes.h"

namespace gladius
{
    class ConfigManager;
}

namespace gladius::ui::async_rendering
{
    /**
     * @brief Feature flag configuration for the async rendering backend.
     */
    struct AsyncRenderFeatureConfig
    {
        bool enabled{false};
        std::string implementation{"legacy"};
        std::string bufferingMode{"double"};

        [[nodiscard]] bool wantsCoroutineBackend() const noexcept
        {
            return enabled && implementation == "coroutines";
        }
    };

    [[nodiscard]] AsyncRenderFeatureConfig
      loadAsyncRenderFeatureConfig(gladius::ConfigManager const * configManager);

    /**
     * @brief Coordinates coroutine-based async rendering worker lifecycle.
     *
     * This initial scaffold focuses on providing the foundational job queue and worker management
     * that later stages (C2+) will extend with actual rendering logic.
     */
    class AsyncRenderController
    {
      public:
        using CancelCheck = std::function<bool()>;
        using JobExecutor =
          std::function<FrameResultMeta(RenderJob const &, CancelCheck const & cancellationCheck)>;

        AsyncRenderController();
        explicit AsyncRenderController(std::shared_ptr<coro::thread_pool> workerPool);
        ~AsyncRenderController();

        AsyncRenderController(AsyncRenderController const &) = delete;
        AsyncRenderController & operator=(AsyncRenderController const &) = delete;
        AsyncRenderController(AsyncRenderController &&) = delete;
        AsyncRenderController & operator=(AsyncRenderController &&) = delete;

        void start();
        void stop();

        void enqueueJob(RenderJob job);
        void setLatestEpoch(uint64_t epoch);
        void setJobExecutor(JobExecutor executor);
        [[nodiscard]] std::optional<FrameResultMeta> tryConsumeResult();

        [[nodiscard]] bool isRunning() const noexcept;
        [[nodiscard]] std::shared_ptr<coro::thread_pool> workerPool() const noexcept;

      private:
        struct ControllerState;

        [[nodiscard]] static auto workerLoop(std::shared_ptr<ControllerState> state)
          -> coro::task<void>;

        std::shared_ptr<ControllerState> m_state;
        std::atomic<bool> m_running{false};
    };
}
