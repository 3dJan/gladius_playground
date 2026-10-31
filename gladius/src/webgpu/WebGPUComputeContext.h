#pragma once

#include <atomic>
#include <memory>
#include <string>

#ifdef Always
#undef Always
#endif

#ifdef True
#undef True
#endif

#ifdef False
#undef False
#endif

#include <webgpu/webgpu_cpp.h>

#include <mutex>

namespace gladius::webgpu
{
    /**
     * @brief Owns a surfaceless Dawn instance, adapter, device, and queue.
     *
     * All device/queue access must be serialized: Dawn's instance processing and queue
     * submission are not safe to call concurrently from multiple threads on all backends.
     */
    class WebGPUComputeContext
    {
      private:
        struct CallbackState;

      public:
        WebGPUComputeContext();
        ~WebGPUComputeContext();

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::string getErrorMessage() const;

        void processEvents() const;

        /// RAII guard for a synchronous device transaction, including callback mapping and handle release.
        /// Never retain this guard across coroutine suspension or CPU batch generation/analysis.
        class DeviceLock
        {
          public:
            explicit DeviceLock(WebGPUComputeContext const & context);
            ~DeviceLock();
            DeviceLock(DeviceLock const &) = delete;
            DeviceLock & operator=(DeviceLock const &) = delete;
            DeviceLock(DeviceLock &&) = delete;
            DeviceLock & operator=(DeviceLock &&) = delete;

          private:
            std::shared_ptr<CallbackState> m_callbackState;
            std::unique_lock<std::recursive_mutex> m_lock;
        };

        [[nodiscard]] wgpu::Device const & getDevice() const noexcept;
        [[nodiscard]] wgpu::Queue const & getQueue() const noexcept;
        [[nodiscard]] wgpu::Instance const & getInstance() const noexcept;
        [[nodiscard]] wgpu::Adapter const & getAdapter() const noexcept;
        // Returns true until initialize() completes or fails.  Polled by the
        // UI so that the browser can show a status while the WebGPU adapter
        // and device are still being requested.
        [[nodiscard]] bool isInitializing() const;

      #ifdef __EMSCRIPTEN__
        /// Complete browser-side device setup after the spontaneous callback has returned.
        /// Emscripten's WebGPU shim can stall the browser callback if the queue is acquired from
        /// inside the device-request callback, so queue acquisition is deliberately deferred to
        /// the next UI turn.
        [[nodiscard]] bool completeDeviceInitialization();
      #endif

      private:
        void initialize();
      #ifdef __EMSCRIPTEN__
        void requestDevice();
        void handleAdapterRequest(wgpu::RequestAdapterStatus status,
                                  wgpu::Adapter adapter,
                                  wgpu::StringView message);
        void handleDeviceRequest(wgpu::RequestDeviceStatus status,
                                 wgpu::Device device,
                                 wgpu::StringView message);
      #endif
        void setDeviceLost(wgpu::DeviceLostReason reason, wgpu::StringView message);
        void setUncapturedError(wgpu::ErrorType type, wgpu::StringView message);
        void setErrorMessage(std::string message);

        std::shared_ptr<CallbackState> m_callbackState;
        wgpu::Instance m_instance;
        wgpu::Adapter m_adapter;
        wgpu::Device m_device;
        wgpu::Queue m_queue;
        std::atomic<bool> m_isValid{false};
        mutable std::mutex m_errorMutex;
        std::string m_errorMessage;
    };
}
