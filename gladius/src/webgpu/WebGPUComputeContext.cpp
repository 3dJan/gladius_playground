#include "webgpu/WebGPUComputeContext.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    namespace
    {
        std::string toString(wgpu::StringView const value)
        {
            if (value.data == nullptr)
            {
                return {};
            }

            if (value.length == wgpu::kStrlen)
            {
                return std::string(value.data);
            }

            return std::string(value.data, value.length);
        }
    }

    WebGPUComputeContext::WebGPUComputeContext()
    {
        initialize();
    }

    bool WebGPUComputeContext::isValid() const noexcept
    {
        return m_isValid;
    }

    std::string const & WebGPUComputeContext::getErrorMessage() const noexcept
    {
        return m_errorMessage;
    }

    WebGPUComputeContext::DeviceLock::DeviceLock(WebGPUComputeContext const & context)
        : m_lock(context.m_deviceMutex)
    {
    }

    WebGPUComputeContext::DeviceLock::~DeviceLock() = default;

    void WebGPUComputeContext::processEvents() const
    {
        DeviceLock const lock(*this);
        if (m_instance)
        {
            m_instance.ProcessEvents();
        }

        if (m_device)
        {
            m_device.Tick();
        }
    }

    wgpu::Device const & WebGPUComputeContext::getDevice() const noexcept
    {
        return m_device;
    }

    wgpu::Queue const & WebGPUComputeContext::getQueue() const noexcept
    {
        return m_queue;
    }

    void WebGPUComputeContext::initialize()
    {
        m_instance = wgpu::CreateInstance();
        if (!m_instance)
        {
            throw std::runtime_error("Unable to create the WebGPU instance");
        }

        bool adapterRequestCompleted = false;
        wgpu::RequestAdapterStatus adapterStatus = wgpu::RequestAdapterStatus::Error;
        std::string adapterError;
        m_instance.RequestAdapter(
          nullptr,
          wgpu::CallbackMode::AllowProcessEvents,
                    [&](wgpu::RequestAdapterStatus const status,
                            wgpu::Adapter adapter,
                            wgpu::StringView const message)
          {
              adapterStatus = status;
              m_adapter = std::move(adapter);
              adapterError = toString(message);
              adapterRequestCompleted = true;
          });

        while (!adapterRequestCompleted)
        {
            m_instance.ProcessEvents();
        }

        if (adapterStatus != wgpu::RequestAdapterStatus::Success || !m_adapter)
        {
            m_errorMessage = adapterError.empty() ? "Unable to acquire a WebGPU adapter" : adapterError;
            throw std::runtime_error(m_errorMessage);
        }

        wgpu::DeviceDescriptor deviceDescriptor;
        deviceDescriptor.SetDeviceLostCallback(
          wgpu::CallbackMode::AllowProcessEvents,
             [](wgpu::Device const &,
                 wgpu::DeviceLostReason const reason,
                 wgpu::StringView const message,
                 WebGPUComputeContext * const context) { context->setDeviceLost(reason, message); },
             this);
        deviceDescriptor.SetUncapturedErrorCallback(
             [](wgpu::Device const &,
                 wgpu::ErrorType const type,
                 wgpu::StringView const message,
                 WebGPUComputeContext * const context) { context->setUncapturedError(type, message); },
             this);

        bool deviceRequestCompleted = false;
        wgpu::RequestDeviceStatus deviceStatus = wgpu::RequestDeviceStatus::Error;
        std::string deviceError;
        m_adapter.RequestDevice(
          &deviceDescriptor,
          wgpu::CallbackMode::AllowProcessEvents,
          [&](wgpu::RequestDeviceStatus const status, wgpu::Device device, wgpu::StringView const message)
          {
              deviceStatus = status;
              m_device = std::move(device);
              deviceError = toString(message);
              deviceRequestCompleted = true;
          });

        while (!deviceRequestCompleted)
        {
            m_instance.ProcessEvents();
        }

        if (deviceStatus != wgpu::RequestDeviceStatus::Success || !m_device)
        {
            m_errorMessage = deviceError.empty() ? "Unable to create a WebGPU device" : deviceError;
            throw std::runtime_error(m_errorMessage);
        }

        m_queue = m_device.GetQueue();
        if (!m_queue)
        {
            m_errorMessage = "Unable to acquire the WebGPU device queue";
            throw std::runtime_error(m_errorMessage);
        }

        m_isValid = true;
    }

    void WebGPUComputeContext::setDeviceLost(wgpu::DeviceLostReason const reason,
                                              wgpu::StringView const message)
    {
        m_isValid = false;
        m_errorMessage = "WebGPU device lost (" + std::to_string(static_cast<unsigned int>(reason)) + "): " +
                         toString(message);
    }

    void WebGPUComputeContext::setUncapturedError(wgpu::ErrorType const type,
                                                   wgpu::StringView const message)
    {
        m_errorMessage = "WebGPU error (" + std::to_string(static_cast<unsigned int>(type)) + "): " +
                         toString(message);
    }
}
