#pragma once

#include "compute/IComputeRenderer.h"
#include "render/RenderUpdateTypes.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Terminal backend submission paired with the coordinator task that created it.
     */
    struct NeutralFrameSubmissionResult
    {
        RenderTaskResult taskResult;
        std::optional<compute::RenderFrame> frame;
        std::string errorMessage;
    };

    /**
     * @brief UI-thread poller translating backend-neutral frame submissions into coordinator results.
     *
     * It retains a submission until it reaches a terminal state. Completed frames must exactly
     * match the submitted coordinator task's scene, view, parameter, viewport, and row-range
     * identity before they are marked displayable. Stale-frame policy remains owned by
     * RenderUpdateCoordinator after this translation.
     */
    class NeutralFrameSubmissionTracker
    {
      public:
        ~NeutralFrameSubmissionTracker();

        [[nodiscard]] bool track(RenderTaskRequest task, std::unique_ptr<compute::IRenderSubmission> submission);
        void requestCancellationForStale(RenderStamp const & currentStamp) noexcept;
        void requestCancellationForAll() noexcept;
        [[nodiscard]] std::vector<NeutralFrameSubmissionResult> poll();
        [[nodiscard]] std::vector<NeutralFrameSubmissionResult> drain();
        [[nodiscard]] bool empty() const noexcept;

      private:
        struct TrackedSubmission
        {
            RenderTaskRequest task;
            std::unique_ptr<compute::IRenderSubmission> submission;
            std::chrono::steady_clock::time_point submittedAt;
        };

        [[nodiscard]] static bool matchesTask(compute::RenderFrame const & frame,
                                              RenderTaskRequest const & task) noexcept;
        [[nodiscard]] static RenderTaskResult makeTerminalResult(
          TrackedSubmission const & tracked,
          RenderTaskStatus status,
          bool producedDisplayFrame,
          bool completedFrame) noexcept;

        std::vector<TrackedSubmission> m_submissions;
    };
}
