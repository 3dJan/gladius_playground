#include "webgpu/WebGPUComputeContext.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace gladius::webgpu
{
    struct WebGPUComputeContext::CallbackState
    {
        explicit CallbackState(WebGPUComputeContext * context)
            : owner(context)
        {
        }

        std::recursive_mutex gate;
        WebGPUComputeContext * owner;
    };

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
        : m_callbackState(std::make_shared<CallbackState>(this))
    {
        DeviceLock const lock(*this);
        try
        {
            initialize();
        }
        catch (std::exception const & ex)
        {
            setErrorMessage(ex.what());
        }
        catch (...)
        {
            setErrorMessage("Unknown error while initialising the WebGPU context");
        }
    }

    WebGPUComputeContext::~WebGPUComputeContext()
    {
        DeviceLock const lock(*this);
        m_callbackState->owner = nullptr;
        m_isValid = false;
        if (m_device)
        {
            m_device.Destroy();
        }
        m_queue = nullptr;
        m_device = nullptr;
        m_adapter = nullptr;
        m_instance = nullptr;
    }

    bool WebGPUComputeContext::isValid() const noexcept
    {
        return m_isValid;
    }

    std::string WebGPUComputeContext::getErrorMessage() const
    {
        std::lock_guard<std::mutex> const lock(m_errorMutex);
        return m_errorMessage;
    }

    bool WebGPUComputeContext::isInitializing() const
    {
        if (m_isValid)
        {
            return false;
        }
        std::lock_guard<std::mutex> const lock(m_errorMutex);
        return !m_isValid && m_errorMessage.empty();
    }

    WebGPUComputeContext::DeviceLock::DeviceLock(WebGPUComputeContext const & context)
        : m_callbackState(context.m_callbackState)
        , m_lock(m_callbackState->gate)
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
        DeviceLock const lock(*this);
        if (m_isValid)
        {
            return true;
        }

        if (!m_device || !getErrorMessage().empty())
        {
            return false;
        }

        m_queue = m_device.GetQueue();
        if (!m_queue)
        {
            setErrorMessage("Unable to acquire the WebGPU device queue");
            return false;
        }

        m_isValid = true;
        return true;
    }
#endif

    void WebGPUComputeContext::initialize()
    {
        DeviceLock const lock(*this);
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
          [state = m_callbackState](wgpu::RequestAdapterStatus const status,
              wgpu::Adapter adapter,
              wgpu::StringView const message)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->handleAdapterRequest(status, std::move(adapter), message);
              }
          });
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
            setErrorMessage(adapterError.empty() ? "Unable to acquire a WebGPU adapter" : adapterError);
            throw std::runtime_error(getErrorMessage());
        }

        wgpu::DeviceDescriptor deviceDescriptor;
        deviceDescriptor.SetDeviceLostCallback(
          wgpu::CallbackMode::AllowProcessEvents,
          [state = m_callbackState](wgpu::Device const &,
             wgpu::DeviceLostReason const reason,
             wgpu::StringView const message)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->setDeviceLost(reason, message);
              }
          });
        deviceDescriptor.SetUncapturedErrorCallback(
             [](wgpu::Device const &,
             wgpu::ErrorType const type,
                 wgpu::StringView const message,
                 CallbackState * const state)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->setUncapturedError(type, message);
              }
          },
          m_callbackState.get());

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
            setErrorMessage(deviceError.empty() ? "Unable to create a WebGPU device" : deviceError);
            throw std::runtime_error(getErrorMessage());
        }

        m_queue = m_device.GetQueue();
        if (!m_queue)
        {
            setErrorMessage("Unable to acquire the WebGPU device queue");
            throw std::runtime_error(getErrorMessage());
        }

        m_isValid = true;
#endif
    }

#ifdef __EMSCRIPTEN__
    void WebGPUComputeContext::requestDevice()
    {
        DeviceLock const lock(*this);
        wgpu::DeviceDescriptor deviceDescriptor;
        deviceDescriptor.SetDeviceLostCallback(
          wgpu::CallbackMode::AllowSpontaneous,
          [state = m_callbackState](wgpu::Device const &,
             wgpu::DeviceLostReason const reason,
             wgpu::StringView const message)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->setDeviceLost(reason, message);
              }
          });
        deviceDescriptor.SetUncapturedErrorCallback(
          [](wgpu::Device const &,
             wgpu::ErrorType const type,
             wgpu::StringView const message,
             CallbackState * const state)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->setUncapturedError(type, message);
              }
          },
          m_callbackState.get());

        m_adapter.RequestDevice(
          &deviceDescriptor,
          wgpu::CallbackMode::AllowSpontaneous,
          [state = m_callbackState](wgpu::RequestDeviceStatus const status,
             wgpu::Device device,
             wgpu::StringView const message)
          {
              std::lock_guard<std::recursive_mutex> const callbackLock(state->gate);
              if (state->owner)
              {
                  state->owner->handleDeviceRequest(status, std::move(device), message);
              }
          });
    }

    void WebGPUComputeContext::handleAdapterRequest(wgpu::RequestAdapterStatus const status,
                                                    wgpu::Adapter adapter,
                                                    wgpu::StringView const message)
    {
        DeviceLock const lock(*this);
        if (status != wgpu::RequestAdapterStatus::Success || !adapter)
        {
            std::string const error = toString(message);
            setErrorMessage(error.empty() ? "Unable to acquire a WebGPU adapter" : error);
            return;
        }

        m_adapter = std::move(adapter);
        requestDevice();
    }

    void WebGPUComputeContext::handleDeviceRequest(wgpu::RequestDeviceStatus const status,
                                                   wgpu::Device device,
                                                   wgpu::StringView const message)
    {
        DeviceLock const lock(*this);
        if (status != wgpu::RequestDeviceStatus::Success || !device)
        {
            std::string const error = toString(message);
            setErrorMessage(error.empty() ? "Unable to create a WebGPU device" : error);
            return;
        }

        m_device = std::move(device);
    }
#endif

    void WebGPUComputeContext::setDeviceLost(wgpu::DeviceLostReason const reason,
                                              wgpu::StringView const message)
    {
        DeviceLock const lock(*this);
        setErrorMessage("WebGPU device lost (" + std::to_string(static_cast<unsigned int>(reason)) + "): " +
                toString(message));
        m_isValid = false;
    }

    void WebGPUComputeContext::setUncapturedError(wgpu::ErrorType const type,
                                                   wgpu::StringView const message)
    {
        DeviceLock const lock(*this);
        std::lock_guard<std::mutex> const errorLock(m_errorMutex);
        if (!m_errorMessage.empty())
        {
            m_errorMessage += '\n';
        }
        m_errorMessage += "WebGPU error (" + std::to_string(static_cast<unsigned int>(type)) + "): " +
                          toString(message);
    }

    void WebGPUComputeContext::setErrorMessage(std::string message)
    {
        std::lock_guard<std::mutex> const lock(m_errorMutex);
        m_errorMessage = std::move(message);
    }
}
