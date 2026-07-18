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

namespace gladius::webgpu
{
    /**
     * @brief Owns a surfaceless Dawn instance, adapter, device, and queue.
     */
    class WebGPUComputeContext
    {
      public:
        WebGPUComputeContext();

        [[nodiscard]] bool isValid() const noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;

        void processEvents() const;

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
    };
}
