#pragma once

#include "compute/ComputeBackend.h"
#include "compute/RenderContracts.h"

#include <memory>
#include <string>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::compute
{
  class RenderBackendSession;
  class IBoundsService;

    /**
     * @brief Owns the services selected for one application compute backend.
     *
     * The legacy OpenCL core is exposed only as a migration seam. New consumers must use the
     * backend-neutral render session instead of constructing or selecting a different backend.
     */
    class IBackendRuntime
    {
      public:
        virtual ~IBackendRuntime() = default;

        [[nodiscard]] virtual ComputeBackendKind getBackendKind() const noexcept = 0;
        [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
        [[nodiscard]] virtual std::string const & getErrorMessage() const noexcept = 0;
        [[nodiscard]] virtual RendererCapability getCapabilities() const noexcept = 0;

        /// @brief Transitional OpenCL adapter; null for a pure WebGPU runtime.
        [[nodiscard]] virtual std::shared_ptr<ComputeCore> getOpenCLCore() const noexcept = 0;

        /// @brief Returns the runtime-owned neutral render session, if the backend exposes one.
        [[nodiscard]] virtual RenderBackendSession * getRenderBackendSession() noexcept = 0;

        /// @brief Returns the bounds service owned by the selected backend runtime.
        [[nodiscard]] virtual IBoundsService * getBoundsService() noexcept = 0;
    };

    /**
     * @brief Creates the selected application runtime without backend fallback.
     */
    class ApplicationComputeRuntime
    {
      public:
        [[nodiscard]] static std::unique_ptr<IBackendRuntime>
        createOpenCL(std::shared_ptr<ComputeCore> core);

        [[nodiscard]] static std::unique_ptr<IBackendRuntime> createWebGPU();
    };
}
