#include "compute/ApplicationComputeRuntime.h"

#if defined(GLADIUS_ENABLE_OPENCL)
#include "compute/ComputeCore.h"
#include "compute/OpenCLBoundsService.h"
#endif
#include "compute/ComputeRendererFactory.h"
#include "compute/RenderBackendSession.h"
#if defined(GLADIUS_ENABLE_WEBGPU)
#include "webgpu/WebGPUComputeContext.h"
#include "webgpu/WebGPUBoundsService.h"
#include "webgpu/WebGPUComputeRenderer.h"
#endif

#include <stdexcept>
#include <utility>

namespace gladius::compute
{
    namespace
    {
#if defined(GLADIUS_ENABLE_OPENCL)
        class OpenCLRuntime final : public IBackendRuntime
        {
          public:
            explicit OpenCLRuntime(std::shared_ptr<gladius::ComputeCore> core)
                : m_core(std::move(core))
            {
                if (!m_core)
                {
                    m_errorMessage = "OpenCL runtime requires a ComputeCore";
                    return;
                }

                try
                {
                    auto boundsService = std::make_shared<OpenCLBoundsService>(m_core);
                    m_renderBackendSession = std::make_unique<RenderBackendSession>(
                      ComputeRendererFactory::create(ComputeBackendKind::OpenCL,
                                                                                                         m_core->createRenderSession()),
                                            std::move(boundsService));
                }
                catch (std::exception const & exception)
                {
                    m_errorMessage = exception.what();
                }
            }

            [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
            {
                return ComputeBackendKind::OpenCL;
            }

            [[nodiscard]] bool isAvailable() const noexcept override
            {
                return m_core != nullptr && m_errorMessage.empty();
            }

            [[nodiscard]] std::string const & getErrorMessage() const noexcept override
            {
                return m_errorMessage;
            }

            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                if (!m_renderBackendSession)
                {
                    return RendererCapability::None;
                }

                auto capabilities = m_renderBackendSession->getCapabilities();
                if (m_core && m_core->getProgramManager().isVdbSupported())
                {
                    capabilities = capabilities | RendererCapability::VdbSampling;
                }
                return capabilities;
            }

            [[nodiscard]] std::shared_ptr<gladius::ComputeCore>
            getOpenCLCore() const noexcept override
            {
                return m_core;
            }

            [[nodiscard]] RenderBackendSession * getRenderBackendSession() noexcept override
            {
                return m_renderBackendSession.get();
            }

            [[nodiscard]] IBoundsService * getBoundsService() noexcept override
            {
                return m_renderBackendSession ? m_renderBackendSession->getBoundsService() : nullptr;
            }

          private:
            std::shared_ptr<gladius::ComputeCore> m_core;
            std::unique_ptr<RenderBackendSession> m_renderBackendSession;
            std::string m_errorMessage;
        };
#endif

        class WebGPURuntime final : public IBackendRuntime
        {
          public:
            WebGPURuntime()
            {
                try
                {
                    auto context = std::make_shared<webgpu::WebGPUComputeContext>();
                    auto boundsService = std::make_shared<webgpu::WebGPUBoundsService>(context);
                    m_renderBackendSession = std::make_unique<RenderBackendSession>(
                      std::make_unique<webgpu::WebGPUComputeRenderer>(context),
                      std::move(boundsService));
                }
                catch (std::exception const & exception)
                {
                    m_errorMessage = exception.what();
                }
            }

            [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept override
            {
                return ComputeBackendKind::WebGPU;
            }

            [[nodiscard]] bool isAvailable() const noexcept override
            {
                return m_renderBackendSession != nullptr &&
                       m_renderBackendSession->isAvailable() && m_errorMessage.empty();
            }

            [[nodiscard]] std::string const & getErrorMessage() const noexcept override
            {
                if (!m_errorMessage.empty())
                {
                    return m_errorMessage;
                }
                static std::string const emptyMessage;
                return emptyMessage;
            }

            [[nodiscard]] RendererCapability getCapabilities() const noexcept override
            {
                return m_renderBackendSession ? m_renderBackendSession->getCapabilities()
                                              : RendererCapability::None;
            }

            [[nodiscard]] std::shared_ptr<ComputeCore> getOpenCLCore() const noexcept override
            {
                return {};
            }

            [[nodiscard]] RenderBackendSession * getRenderBackendSession() noexcept override
            {
                return m_renderBackendSession.get();
            }

            [[nodiscard]] IBoundsService * getBoundsService() noexcept override
            {
                return m_renderBackendSession ? m_renderBackendSession->getBoundsService() : nullptr;
            }

          private:
            std::unique_ptr<RenderBackendSession> m_renderBackendSession;
            std::string m_errorMessage;
        };
    }

    std::unique_ptr<IBackendRuntime>
    ApplicationComputeRuntime::createOpenCL(std::shared_ptr<ComputeCore> core)
    {
#if defined(GLADIUS_ENABLE_OPENCL)
        return std::make_unique<OpenCLRuntime>(std::move(core));
#else
        (void) core;
        throw std::runtime_error("OpenCL backend not built into this binary");
#endif
    }

    std::unique_ptr<IBackendRuntime> ApplicationComputeRuntime::createWebGPU()
    {
#if defined(GLADIUS_ENABLE_WEBGPU)
        return std::make_unique<WebGPURuntime>();
#else
        throw std::runtime_error("WebGPU backend not built into this binary");
#endif
    }
}
