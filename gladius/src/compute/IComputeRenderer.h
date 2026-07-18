#pragma once

#include "compute/ComputeBackend.h"
#include "compute/RenderContracts.h"

#include <memory>
#include <optional>
#include <string>

namespace gladius::compute
{
    /**
     * @brief Lifecycle status of a backend-neutral render submission.
     */
    enum class RenderSubmissionStatus
    {
        Pending,
        Succeeded,
        Cancelled,
        Failed,
    };

    /**
     * @brief Owns a submitted frame render and its backend resources until completion.
     *
     * requestCancellation() is best-effort. A backend may only be able to prevent a dispatch
     * that has not started; completed stale frames are rejected by their freshness stamp.
     */
    class IRenderSubmission
    {
      public:
        virtual ~IRenderSubmission() = default;

        [[nodiscard]] virtual RenderSubmissionStatus getStatus() const noexcept = 0;
        virtual void requestCancellation() noexcept = 0;
        virtual void wait() = 0;
        [[nodiscard]] virtual std::optional<RenderFrame> takeFrame() = 0;
        [[nodiscard]] virtual std::string getErrorMessage() const = 0;
    };

    /**
     * @brief Physical renderer for API-neutral frame rendering.
     *
     * Public consumers depend only on RenderRequest, RenderFrame, and capability flags. Scene
     * materialization remains backend-owned and will be added once the shared CPU scene payload
     * contract is introduced.
     */
    class IComputeRenderer
    {
      public:
        virtual ~IComputeRenderer() = default;

        [[nodiscard]] virtual ComputeBackendKind getBackendKind() const noexcept = 0;
        [[nodiscard]] virtual RendererCapability getCapabilities() const noexcept = 0;
        [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
        [[nodiscard]] virtual std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest request) = 0;
    };
}
