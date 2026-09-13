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
        try
        {
            initialize();
        }
        catch (std::exception const & ex)
        {
            m_errorMessage = ex.what();
        }
        catch (...)
        {
            m_errorMessage = "Unknown error while initialising the WebGPU context";
        }
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
    }

    wgpu::Device const & WebGPUComputeContext::getDevice() const noexcept
    {
        return m_device;
    }

    wgpu::Queue const & WebGPUComputeContext::getQueue() const noexcept
    {
        return m_queue;
    }

    wgpu::Instance const & WebGPUComputeContext::getInstance() const noexcept
    {
        return m_instance;
    }

    wgpu::Adapter const & WebGPUComputeContext::getAdapter() const noexcept
    {
        return m_adapter;
    }

#ifdef __EMSCRIPTEN__
    bool WebGPUComputeContext::completeDeviceInitialization()
    {
        if (m_isValid)
        {
            return true;
        }

        if (!m_device)
        {
            return false;
        }

        m_queue = m_device.GetQueue();
        if (!m_queue)
        {
            m_errorMessage = "Unable to acquire the WebGPU device queue";
            return false;
        }

        m_isValid = true;
        return true;
    }
#endif

    void WebGPUComputeContext::initialize()
    {
        m_instance = wgpu::CreateInstance();
        if (!m_instance)
        {
            throw std::runtime_error("Unable to create the WebGPU instance");
        }

#ifdef __EMSCRIPTEN__
        // Browser adapter and device requests complete through JavaScript
        // promises.  Returning to the browser event loop is required before
        // their callbacks can run; polling here would prevent that progress.
        wgpu::RequestAdapterOptions options{};
        options.powerPreference = wgpu::PowerPreference::HighPerformance;
        m_instance.RequestAdapter(
          &options,
          wgpu::CallbackMode::AllowSpontaneous,
          [](wgpu::RequestAdapterStatus const status,
              wgpu::Adapter adapter,
              wgpu::StringView const message,
              WebGPUComputeContext * const context)
          { context->handleAdapterRequest(status, std::move(adapter), message); },
          this);
        return;
#else
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
             WebGPUComputeContext * const context)
          { context->setDeviceLost(reason, message); },
          this);
        deviceDescriptor.SetUncapturedErrorCallback(
          [](wgpu::Device const &,
             wgpu::ErrorType const type,
             wgpu::StringView const message,
             WebGPUComputeContext * const context)
          { context->setUncapturedError(type, message); },
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
#endif
    }

#ifdef __EMSCRIPTEN__
    void WebGPUComputeContext::requestDevice()
    {
        wgpu::DeviceDescriptor deviceDescriptor;
        deviceDescriptor.SetUncapturedErrorCallback(
          [](wgpu::Device const &,
             wgpu::ErrorType const type,
             wgpu::StringView const message,
             WebGPUComputeContext * const context)
          { context->setUncapturedError(type, message); },
          this);

        m_adapter.RequestDevice(
          &deviceDescriptor,
          wgpu::CallbackMode::AllowSpontaneous,
          [](wgpu::RequestDeviceStatus const status,
             wgpu::Device device,
             wgpu::StringView const message,
             WebGPUComputeContext * const context)
          { context->handleDeviceRequest(status, std::move(device), message); },
          this);
    }

    void WebGPUComputeContext::handleAdapterRequest(wgpu::RequestAdapterStatus const status,
                                                    wgpu::Adapter adapter,
                                                    wgpu::StringView const message)
    {
        if (status != wgpu::RequestAdapterStatus::Success || !adapter)
        {
            std::string const error = toString(message);
            m_errorMessage = error.empty() ? "Unable to acquire a WebGPU adapter" : error;
            return;
        }

        m_adapter = std::move(adapter);
        requestDevice();
    }

    void WebGPUComputeContext::handleDeviceRequest(wgpu::RequestDeviceStatus const status,
                                                   wgpu::Device device,
                                                   wgpu::StringView const message)
    {
        if (status != wgpu::RequestDeviceStatus::Success || !device)
        {
            std::string const error = toString(message);
            m_errorMessage = error.empty() ? "Unable to create a WebGPU device" : error;
            return;
        }

        m_device = std::move(device);
    }
#endif

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
        if (!m_errorMessage.empty())
        {
            m_errorMessage += '\n';
        }
        m_errorMessage += "WebGPU error (" + std::to_string(static_cast<unsigned int>(type)) + "): " +
                          toString(message);
    }
}
