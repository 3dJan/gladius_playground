#include "compute/ComputeBackend.h"
#include "compute/ComputeBackendSettings.h"
#include "compute/ComputeRendererFactory.h"

#include "compute/AnalyticRenderSceneSnapshotFactory.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "compute/OpenCLComputeRenderer.h"
#include "compute/RenderSession.h"
#endif

#if defined(GLADIUS_ENABLE_WEBGPU)
#  include "webgpu/WebGPUComputeRenderer.h"
#endif

#include "ConfigManager.h"

#include <stdexcept>
#include <utility>

namespace gladius::compute
{
    std::unique_ptr<IComputeRenderer> ComputeRendererFactory::create(ComputeBackendKind kind)
    {
        switch (kind)
        {
            case ComputeBackendKind::OpenCL: {
#if defined(GLADIUS_ENABLE_OPENCL)
                throw std::runtime_error(
                    "OpenCL renderer requires a RenderSession; use create(kind, session)");
#else
                throw std::runtime_error("OpenCL backend not built into this binary");
#endif
            }

#if defined(GLADIUS_ENABLE_WEBGPU)
            case ComputeBackendKind::WebGPU: {
                return std::make_unique<webgpu::WebGPUComputeRenderer>();
            }
#endif

            default:
                throw std::runtime_error("Unknown compute backend kind");
        }
    }

#if defined(GLADIUS_ENABLE_OPENCL)
    std::unique_ptr<IComputeRenderer> ComputeRendererFactory::create(ComputeBackendKind kind,
                                                                     SharedRenderSession session)
    {
        switch (kind)
        {
            case ComputeBackendKind::OpenCL: {
#if defined(GLADIUS_ENABLE_OPENCL)
                if (!session)
                {
                    throw std::invalid_argument("OpenCL renderer requires a valid render session");
                }
                return std::make_unique<OpenCLComputeRenderer>(std::move(session));
#else
                throw std::runtime_error("OpenCL backend not built into this binary");
#endif
            }

#if defined(GLADIUS_ENABLE_WEBGPU)
            case ComputeBackendKind::WebGPU: {
                // WebGPU does not use an external session; ignore the session parameter.
                return std::make_unique<webgpu::WebGPUComputeRenderer>();
            }
#endif

            default:
                throw std::runtime_error("Unknown compute backend kind");
        }
    }
#endif

    std::unique_ptr<RenderBackendSession> ComputeRendererFactory::createRenderBackendSession(
        ConfigManager const & configManager,
        ComputeBackendKind preferredBackend)
    {
        ComputeBackendKind configured = getConfiguredComputeBackend(configManager);

        // If the configured backend differs from preferred and is not built, fall back.
        if (configured != preferredBackend && !isComputeBackendBuilt(configured))
        {
            configured = preferredBackend;
        }

        if (!isComputeBackendBuilt(configured))
        {
            return nullptr;
        }

#if defined(GLADIUS_ENABLE_WEBGPU)
        if (configured == ComputeBackendKind::WebGPU)
        {
            auto renderer = std::make_unique<webgpu::WebGPUComputeRenderer>();
            return std::make_unique<RenderBackendSession>(std::move(renderer));
        }
#endif

#if defined(GLADIUS_ENABLE_OPENCL)
        if (configured == ComputeBackendKind::OpenCL)
        {
            throw std::runtime_error(
                "OpenCL renderer requires a RenderSession; use createRenderBackendSession(config, session, preferred)");
        }
#else
        return nullptr;
#endif

        return nullptr;
    }

#if defined(GLADIUS_ENABLE_OPENCL)
    std::unique_ptr<RenderBackendSession> ComputeRendererFactory::createRenderBackendSession(
        ConfigManager const & configManager,
        SharedRenderSession openclSession,
        ComputeBackendKind preferredBackend)
    {
        ComputeBackendKind configured = getConfiguredComputeBackend(configManager);

        // If the configured backend differs from preferred and is not built, fall back.
        if (configured != preferredBackend && !isComputeBackendBuilt(configured))
        {
            configured = preferredBackend;
        }

        if (!isComputeBackendBuilt(configured))
        {
            return nullptr;
        }

#if defined(GLADIUS_ENABLE_WEBGPU)
        if (configured == ComputeBackendKind::WebGPU)
        {
            auto renderer = std::make_unique<webgpu::WebGPUComputeRenderer>();
            return std::make_unique<RenderBackendSession>(std::move(renderer));
        }
#endif

#if defined(GLADIUS_ENABLE_OPENCL)
        if (configured == ComputeBackendKind::OpenCL)
        {
            auto renderer = create(configured, openclSession);
            return std::make_unique<RenderBackendSession>(std::move(renderer));
        }
#else
        return nullptr;
#endif

        return nullptr;
    }
#endif

    RenderSceneSnapshot ComputeRendererFactory::materializeScene(
        nodes::Assembly const * assembly,
        nodes::Model & model,
        std::uint64_t generation)
    {
        try
        {
            if (assembly != nullptr)
            {
                return AnalyticRenderSceneSnapshotFactory::create(*assembly, generation);
            }
            return AnalyticRenderSceneSnapshotFactory::create(model, generation);
        }
        catch (...)
        {
            return RenderSceneSnapshot{}; // Invalid snapshot on failure
        }
    }
}
