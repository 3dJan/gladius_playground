#pragma once

#include "NeutralFrameSubmissionTracker.h"
#include "RenderWorkflowController.h"
#include "compute/RenderBackendSession.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace gladius::ui::async_rendering
{
  struct NeutralRenderSubmissionTimings
  {
    double requestFactoryMs{0.0};
    double backendSubmitMs{0.0};
  };

    /**
     * @brief Accepted backend-neutral frame ready for a UI-thread presenter upload.
     */
    struct AcceptedNeutralFrame
    {
        compute::RenderFrame frame;
        FramePresentationCandidate candidate;
    };

    /**
     * @brief Terminal submission data and workflow commands produced by one non-blocking poll.
     */
    struct NeutralRenderPollResult
    {
        std::vector<AcceptedNeutralFrame> acceptedFrames;
        std::vector<NeutralFrameSubmissionResult> completions;
        std::vector<RenderCommand> commands;
    };

    /**
     * @brief Connects coordinator display tasks to an IComputeRenderer without owning UI policy.
     *
     * The factory snapshots the selected backend's immutable request data when a coordinator
     * StartTask is translated. This facade retains submissions through NeutralFrameSubmissionTracker,
     * feeds terminal task metadata to RenderWorkflowController, and returns only frames accepted by
     * that controller's existing freshness/quality ledger.
     */
    class NeutralRenderScheduler
    {
      public:
        static constexpr std::size_t MAX_PROGRESSIVE_CHUNK_PIXELS = 2u * 1024u;

        using RequestFactory = std::function<std::optional<compute::RenderRequest>(RenderTaskRequest const &)>;

        explicit NeutralRenderScheduler(RequestFactory requestFactory);

        [[nodiscard]] bool submit(RenderTaskRequest const & task, compute::IComputeRenderer & renderer);
        [[nodiscard]] bool submit(RenderTaskRequest const & task,
                 compute::IComputeRenderer & renderer,
                 compute::IRenderScene const & scene);
        [[nodiscard]] bool submit(RenderTaskRequest const & task,
                                  compute::RenderBackendSession & session,
                                  NeutralRenderSubmissionTimings * timings = nullptr);
        void requestCancellationForStale() noexcept;
        void requestCancellationForAll() noexcept;
        [[nodiscard]] NeutralRenderPollResult poll(bool scheduleFollowUp = true);
        [[nodiscard]] NeutralRenderPollResult drain(bool scheduleFollowUp = true);
        void resetWorkflow(RealtimeRaymarchConfig config);

        [[nodiscard]] RenderWorkflowController & workflow() noexcept;
        [[nodiscard]] RenderWorkflowController const & workflow() const noexcept;
        [[nodiscard]] bool hasCurrentInFlightSubmission() const noexcept;
        [[nodiscard]] bool hasInFlightSubmissions() const noexcept;

      private:
        [[nodiscard]] static bool isSupportedDisplayTask(RenderTaskType type) noexcept;
        [[nodiscard]] static std::optional<FramePresentationCandidate> findCandidate(
          RenderWorkflowDecision const & decision,
          std::uint64_t requestId) noexcept;
        [[nodiscard]] RenderTaskRequest prepareTaskForSubmission(RenderTaskRequest const & task);
        void resetProgressiveSequence() noexcept;

        RequestFactory m_requestFactory;
        NeutralFrameSubmissionTracker m_submissions;
        RenderWorkflowController m_workflow;
        std::optional<RenderStamp> m_progressiveStamp;
        std::uint32_t m_progressiveWidth{};
        std::uint32_t m_progressiveHeight{};
        std::uint32_t m_progressiveNextRow{};
        bool m_progressiveSequenceActive{false};
    };
}
