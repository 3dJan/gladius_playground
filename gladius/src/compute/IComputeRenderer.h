#pragma once

#include "compute/ComputeBackend.h"
#include "compute/RenderContracts.h"
#include "compute/RenderSceneSnapshot.h"

#include <memory>
#include <optional>
#include <stdexcept>
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

        /// @brief Advance backend callbacks without blocking the calling thread.
        virtual void progress() noexcept
        {
        }

        [[nodiscard]] virtual RenderSubmissionStatus getStatus() const noexcept = 0;
        virtual void requestCancellation() noexcept = 0;
        virtual void wait() = 0;
        [[nodiscard]] virtual std::optional<RenderFrame> takeFrame() = 0;
        [[nodiscard]] virtual std::string getErrorMessage() const = 0;
    };

    /**
     * @brief Opaque backend-owned realization of an immutable render scene snapshot.
     */
    class IRenderScene
    {
      public:
        virtual ~IRenderScene() = default;

        [[nodiscard]] virtual ComputeBackendKind getBackendKind() const noexcept = 0;
        [[nodiscard]] virtual std::uint64_t getSceneGeneration() const noexcept = 0;
        [[nodiscard]] virtual RendererCapability getCapabilities() const noexcept = 0;
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

        /// @brief Materialize an immutable CPU scene payload into backend-owned resources.
        [[nodiscard]] virtual std::unique_ptr<IRenderScene> materializeScene(RenderSceneSnapshot snapshot)
        {
          throw std::runtime_error("This renderer does not materialize scene snapshots");
        }

        /// @brief Materialize a shared immutable snapshot without copying its neutral payload.
        ///
        /// Renderers that need to retain the exact snapshot object can override this overload.
        /// The default delegates to the established by-value entry point for compatibility with
        /// existing renderer implementations and tests.
        [[nodiscard]] virtual std::unique_ptr<IRenderScene>
        materializeScene(std::shared_ptr<const RenderSceneSnapshot> snapshot)
        {
            if (!snapshot)
            {
                throw std::invalid_argument("Render scene snapshot must not be null");
            }
            return materializeScene(*snapshot);
        }

        /// @brief Submit a frame against a materialized scene.
        [[nodiscard]] virtual std::unique_ptr<IRenderSubmission>
        submitFrame(IRenderScene const & scene, RenderRequest request)
        {
          throw std::runtime_error("This renderer does not submit materialized scenes");
        }

        /// @brief Transitional entry point for renderers that retain their scene externally.
        [[nodiscard]] virtual std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest request) = 0;
    };
}
