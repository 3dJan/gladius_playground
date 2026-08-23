#pragma once

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
      public:
        WebGPUComputeContext();

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;

        void processEvents() const;

        /// RAII guard serializing device/queue access across threads.
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
            std::unique_lock<std::recursive_mutex> m_lock;
        };

        [[nodiscard]] wgpu::Device const & getDevice() const noexcept;
        [[nodiscard]] wgpu::Queue const & getQueue() const noexcept;

      private:
        void initialize();
        void setDeviceLost(wgpu::DeviceLostReason reason, wgpu::StringView message);
        void setUncapturedError(wgpu::ErrorType type, wgpu::StringView message);

        wgpu::Instance m_instance;
        wgpu::Adapter m_adapter;
        wgpu::Device m_device;
        wgpu::Queue m_queue;
        bool m_isValid{};
        std::string m_errorMessage;
        mutable std::recursive_mutex m_deviceMutex;
    };
}
