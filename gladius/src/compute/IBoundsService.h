#pragma once

#include "compute/BoundingBoxContracts.h"
#include "compute/RenderSceneSnapshot.h"

#include <memory>
#include <optional>
#include <string>

namespace gladius::compute
{
    /**
     * @brief Owns one physical bounds calculation until it reaches a terminal state.
     *
     * Cancellation is a request to suppress or invalidate work. Implementations must retain
     * backend resources until submitted work and any readback callback are terminal.
     */
    class IBoundsSubmission
    {
      public:
        virtual ~IBoundsSubmission() = default;

        /// @brief Advance backend callbacks without blocking the calling thread.
        virtual void progress() noexcept
        {
        }

        [[nodiscard]] virtual BoundsSubmissionStatus getStatus() const noexcept = 0;
        virtual void requestCancellation() noexcept = 0;
        virtual void wait() = 0;
        [[nodiscard]] virtual std::optional<BoundsResult> takeResult() = 0;
        [[nodiscard]] virtual std::string getErrorMessage() const = 0;
    };

    /**
     * @brief Backend-neutral model-bounds service owned by one selected runtime.
     */
    class IBoundsService
    {
      public:
        virtual ~IBoundsService() = default;

        [[nodiscard]] virtual RendererCapability getCapabilities() const noexcept = 0;
        [[nodiscard]] virtual bool isAvailable() const noexcept = 0;

        /// @brief Publish the immutable snapshot used by subsequent bounds submissions.
        ///
        /// Implementations retain snapshots referenced by in-flight work until that work reaches
        /// a terminal state. The default keeps older service implementations source-compatible.
        virtual void setSceneSnapshot(std::shared_ptr<const RenderSceneSnapshot> snapshot) noexcept
        {
          (void) snapshot;
        }

        /// @brief Return a result only when a cached result exists for the requested model state.
        [[nodiscard]] virtual std::optional<BoundsResult>
        getCachedResult(RenderFreshnessStamp const & freshness) const noexcept = 0;

        /// @brief Submit a non-blocking bounds calculation for an immutable model generation.
        [[nodiscard]] virtual std::unique_ptr<IBoundsSubmission> submit(BoundsRequest request) = 0;
    };
}