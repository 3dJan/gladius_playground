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

    struct ProgressiveRenderProgress
    {
        bool active{false};
        std::uint32_t nextRow{0u};
        std::uint32_t totalRows{0u};
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
        static constexpr std::size_t MIN_PROGRESSIVE_CHUNK_PIXELS = 2u * 1024u;
        /// Smallest chunk a settled static view starts with; only slower devices go below this.
        static constexpr std::size_t INITIAL_PROGRESSIVE_CHUNK_PIXELS = 16u * 1024u;
        /// Ceiling for one adaptive progressive chunk; equals a full 512x512 frame.
        static constexpr std::size_t MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS = 256u * 1024u;
        /// Target GPU pass duration of a progressive chunk when no GPU timestamps exist.
        static constexpr float TARGET_PROGRESSIVE_CHUNK_MS = 20.0f;
        /**
         * @brief Number of progressive chunks a settled static view is filled in by default.
         *
         * The application starts at most one display submission per UI frame, so the chunk count
         * bounds time-to-full-quality by `chunks * frameInterval`. Measured on the WebGPU backend
         * (specs/031-webgpu-raymarch-performance): per-chunk CPU submission cost is ~0.1 ms, so
         * the frame interval, not the submission, dominates. Fewer chunks therefore reach full
         * quality proportionally faster.
         */
        static constexpr std::uint32_t TARGET_PROGRESSIVE_CHUNKS = 8u;
        /// GPU pass time a single chunk may take before the budget is reduced again.
        static constexpr float MAX_PROGRESSIVE_CHUNK_GPU_MS = 33.0f;

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
        [[nodiscard]] ProgressiveRenderProgress progressiveProgress() const noexcept;
        [[nodiscard]] std::size_t currentProgressiveChunkPixels() const noexcept;
        void adaptProgressiveChunkBudget(RenderTaskResult const & result) noexcept;
        /// @brief Chunk budget (pixels) the scheduler starts a progressive sequence with.
        [[nodiscard]] static std::size_t budgetForResolution(std::size_t width, std::size_t height) noexcept;

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
        std::size_t m_progressiveChunkPixels{MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS};
    };
}
