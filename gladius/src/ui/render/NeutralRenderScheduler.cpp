#include "ui/render/NeutralRenderScheduler.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gladius::ui::async_rendering
{
    NeutralRenderScheduler::NeutralRenderScheduler(RequestFactory requestFactory)
        : m_requestFactory{std::move(requestFactory)}
    {
        if (!m_requestFactory)
        {
            throw std::invalid_argument("Neutral renderer scheduler requires a request factory");
        }
    }

    bool NeutralRenderScheduler::submit(RenderTaskRequest const & task, compute::IComputeRenderer & renderer)
    {
        if (!isSupportedDisplayTask(task.type) || !renderer.isAvailable() ||
            !matches(task.stamp, m_workflow.latestStamp(), RenderStampMask::displayFrame()))
        {
            return false;
        }

        auto const submissionTask = prepareTaskForSubmission(task);
        auto request = m_requestFactory(submissionTask);
        if (!request.has_value() || !request->isValid())
        {
            return false;
        }

        try
        {
            return m_submissions.track(submissionTask, renderer.submitFrame(std::move(*request)));
        }
        catch (...)
        {
            return false;
        }
    }

    bool NeutralRenderScheduler::submit(RenderTaskRequest const & task,
                                        compute::IComputeRenderer & renderer,
                                        compute::IRenderScene const & scene)
    {
        if (!isSupportedDisplayTask(task.type) || !renderer.isAvailable() ||
            scene.getBackendKind() != renderer.getBackendKind() ||
            !matches(task.stamp, m_workflow.latestStamp(), RenderStampMask::displayFrame()))
        {
            return false;
        }

        auto const submissionTask = prepareTaskForSubmission(task);
        auto request = m_requestFactory(submissionTask);
        if (!request.has_value() || !request->isValid())
        {
            return false;
        }

        try
        {
            return m_submissions.track(submissionTask, renderer.submitFrame(scene, std::move(*request)));
        }
        catch (...)
        {
            return false;
        }
    }

    bool NeutralRenderScheduler::submit(RenderTaskRequest const & task,
                                        compute::RenderBackendSession & session,
                                        NeutralRenderSubmissionTimings * timings)
    {
        if (timings != nullptr)
        {
            *timings = {};
        }
        if (!isSupportedDisplayTask(task.type) || !session.isAvailable() ||
            !session.hasMaterializedScene() ||
            !matches(task.stamp, m_workflow.latestStamp(), RenderStampMask::displayFrame()) ||
            task.stamp.sceneEpoch != session.getSceneGeneration())
        {
            return false;
        }

        auto const submissionTask = prepareTaskForSubmission(task);
        auto const requestFactoryStart = std::chrono::steady_clock::now();
        auto request = m_requestFactory(submissionTask);
        if (timings != nullptr)
        {
            timings->requestFactoryMs = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - requestFactoryStart)
                                          .count();
        }
        if (!request.has_value() || !request->isValid())
        {
            return false;
        }

        try
        {
            auto const backendSubmitStart = std::chrono::steady_clock::now();
            auto submission = session.submitFrame(std::move(*request));
            if (timings != nullptr)
            {
                timings->backendSubmitMs = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - backendSubmitStart)
                                             .count();
            }
            return m_submissions.track(submissionTask, std::move(submission));
        }
        catch (...)
        {
            return false;
        }
    }

    void NeutralRenderScheduler::requestCancellationForStale() noexcept
    {
        m_submissions.requestCancellationForStale(m_workflow.latestStamp());
    }

    void NeutralRenderScheduler::requestCancellationForAll() noexcept
    {
        m_submissions.requestCancellationForAll();
    }

    NeutralRenderPollResult NeutralRenderScheduler::poll(bool const scheduleFollowUp)
    {
        NeutralRenderPollResult result;
        for (auto & completion : m_submissions.poll())
        {
            if (completion.frame.has_value() &&
                completion.taskResult.type == RenderTaskType::ProgressiveHighQualityChunk &&
                completion.taskResult.succeeded() && m_progressiveSequenceActive &&
                matches(completion.taskResult.stamp,
                        m_workflow.latestStamp(),
                        RenderStampMask::displayFrame()))
            {
                auto const & frame = *completion.frame;
                bool const continuesSequence =
                  m_progressiveStamp.has_value() &&
                  matches(*m_progressiveStamp,
                          completion.taskResult.stamp,
                          RenderStampMask::displayFrame()) &&
                  frame.width == m_progressiveWidth && frame.height == m_progressiveHeight &&
                  frame.firstRow == m_progressiveNextRow && frame.endRow <= frame.height;
                if (continuesSequence)
                {
                    completion.taskResult.completedFrame = frame.endRow == frame.height;
                }
                else
                {
                    completion.taskResult.status = RenderTaskStatus::Failed;
                    completion.taskResult.producedDisplayFrame = false;
                    completion.taskResult.completedFrame = false;
                    completion.frame.reset();
                    resetProgressiveSequence();
                }
            }

            auto decision = m_workflow.completeTaskForPresentation(completion.taskResult, scheduleFollowUp);
            result.commands.insert(result.commands.end(),
                                   std::make_move_iterator(decision.commands.begin()),
                                   std::make_move_iterator(decision.commands.end()));

            if (completion.frame.has_value())
            {
                auto candidate = findCandidate(decision, completion.taskResult.requestId);
                if (candidate.has_value())
                {
                    if (m_progressiveSequenceActive &&
                        completion.taskResult.type == RenderTaskType::ProgressiveHighQualityChunk)
                    {
                        m_progressiveNextRow = completion.frame->endRow;
                        adaptProgressiveChunkBudget(completion.taskResult);
                        if (completion.taskResult.completedFrame)
                        {
                            resetProgressiveSequence();
                        }
                    }
                    result.acceptedFrames.push_back(
                      AcceptedNeutralFrame{.frame = std::move(*completion.frame), .candidate = *candidate});
                }
            }

            result.completions.push_back(std::move(completion));
        }
        return result;
    }

    NeutralRenderPollResult NeutralRenderScheduler::drain(bool const scheduleFollowUp)
    {
        NeutralRenderPollResult result;
        for (auto & completion : m_submissions.drain())
        {
            auto decision = m_workflow.completeTask(completion.taskResult, scheduleFollowUp);
            result.commands.insert(result.commands.end(),
                                   std::make_move_iterator(decision.commands.begin()),
                                   std::make_move_iterator(decision.commands.end()));
            result.completions.push_back(std::move(completion));
        }
        return result;
    }

    void NeutralRenderScheduler::resetWorkflow(RealtimeRaymarchConfig config)
    {
        if (hasInFlightSubmissions())
        {
            throw std::logic_error("Cannot reset render workflow while submissions are in flight");
        }
        m_workflow = RenderWorkflowController{};
        m_workflow.configureRealtime(config);
        m_progressiveChunkPixels = MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS;
        resetProgressiveSequence();
    }

    RenderTaskRequest NeutralRenderScheduler::prepareTaskForSubmission(RenderTaskRequest const & task)
    {
        auto submissionTask = task;
        if (task.type != RenderTaskType::ProgressiveHighQualityChunk || task.width == 0u ||
            task.height == 0u || task.startLine != 0u || task.lineCount < task.height)
        {
            return submissionTask;
        }

        bool const sameSequence =
          m_progressiveSequenceActive && m_progressiveStamp.has_value() &&
          matches(*m_progressiveStamp, task.stamp, RenderStampMask::displayFrame()) &&
          m_progressiveWidth == task.width && m_progressiveHeight == task.height;
        if (!sameSequence)
        {
            m_progressiveStamp = task.stamp;
            m_progressiveWidth = task.width;
            m_progressiveHeight = task.height;
            m_progressiveNextRow = 0u;
            m_progressiveSequenceActive = true;
            m_progressiveChunkPixels = budgetForResolution(task.width, task.height);
        }

        if (m_progressiveNextRow >= task.height)
        {
            m_progressiveNextRow = 0u;
        }

        auto const remainingRows = task.height - m_progressiveNextRow;
        // Round rows up so a resolution-aware budget never yields more chunks than requested.
        auto const rowsPerChunk = std::max<std::size_t>(
          1u,
          (m_progressiveChunkPixels + static_cast<std::size_t>(task.width) - 1u) /
            static_cast<std::size_t>(task.width));
        submissionTask.startLine = m_progressiveNextRow;
        submissionTask.lineCount = std::min<std::size_t>(remainingRows, rowsPerChunk);
        return submissionTask;
    }

    std::size_t NeutralRenderScheduler::budgetForResolution(std::size_t const width,
                                                            std::size_t const height) noexcept
    {
        auto const framePixels = width * height;
        if (framePixels == 0u)
        {
            return MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS;
        }
        auto const evenSplit = (framePixels + TARGET_PROGRESSIVE_CHUNKS - 1u) / TARGET_PROGRESSIVE_CHUNKS;
        return std::clamp(evenSplit, INITIAL_PROGRESSIVE_CHUNK_PIXELS, MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS);
    }

    void NeutralRenderScheduler::adaptProgressiveChunkBudget(RenderTaskResult const & result) noexcept
    {
        // GPU timestamps isolate device work from submission overhead; wall time is only a fallback.
        double const observedMs = result.gpuTimeAvailable
                                    ? static_cast<double>(result.gpuDurationNs) / 1'000'000.0
                                    : static_cast<double>(result.durationNs) / 1'000'000.0;
        if (observedMs <= 0.0)
        {
            return;
        }

        float const targetMs = result.gpuTimeAvailable ? MAX_PROGRESSIVE_CHUNK_GPU_MS : TARGET_PROGRESSIVE_CHUNK_MS;
        if (observedMs > static_cast<double>(targetMs))
        {
            // Shrink aggressively: one over-budget chunk already costs the user a dropped frame.
            double const ratio = std::clamp(static_cast<double>(targetMs) / observedMs, 0.25, 1.0);
            auto const scaled = static_cast<std::size_t>(static_cast<double>(m_progressiveChunkPixels) * ratio);
            m_progressiveChunkPixels = std::max(MIN_PROGRESSIVE_CHUNK_PIXELS, scaled);
            return;
        }

        if (m_progressiveChunkPixels < MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS)
        {
            // Recover towards a whole-frame chunk while the device stays under the time budget.
            m_progressiveChunkPixels =
              std::min(MAX_ADAPTIVE_PROGRESSIVE_CHUNK_PIXELS, m_progressiveChunkPixels * 2u);
        }
    }

    ProgressiveRenderProgress NeutralRenderScheduler::progressiveProgress() const noexcept
    {
        return ProgressiveRenderProgress{.active = m_progressiveSequenceActive,
                                         .nextRow = m_progressiveNextRow,
                                         .totalRows = m_progressiveHeight};
    }

    std::size_t NeutralRenderScheduler::currentProgressiveChunkPixels() const noexcept
    {
        return m_progressiveChunkPixels;
    }

    void NeutralRenderScheduler::resetProgressiveSequence() noexcept
    {
        m_progressiveStamp.reset();
        m_progressiveWidth = 0u;
        m_progressiveHeight = 0u;
        m_progressiveNextRow = 0u;
        m_progressiveSequenceActive = false;
    }

    RenderWorkflowController & NeutralRenderScheduler::workflow() noexcept
    {
        return m_workflow;
    }

    RenderWorkflowController const & NeutralRenderScheduler::workflow() const noexcept
    {
        return m_workflow;
    }

    bool NeutralRenderScheduler::hasCurrentInFlightSubmission() const noexcept
    {
        return m_submissions.hasInFlightSubmission(m_workflow.latestStamp());
    }

    bool NeutralRenderScheduler::hasInFlightSubmissions() const noexcept
    {
        return !m_submissions.empty();
    }

    bool NeutralRenderScheduler::isSupportedDisplayTask(RenderTaskType const type) noexcept
    {
        return type == RenderTaskType::RealtimeFullFrame || type == RenderTaskType::StaticFullFrameProbe ||
               type == RenderTaskType::ProgressiveHighQualityChunk ||
               type == RenderTaskType::LowResolutionPreview;
    }

    std::optional<FramePresentationCandidate> NeutralRenderScheduler::findCandidate(
      RenderWorkflowDecision const & decision,
      std::uint64_t const requestId) noexcept
    {
        for (auto const & candidate : decision.acceptedFrames)
        {
            if (candidate.frameId == requestId)
            {
                return candidate;
            }
        }
        return std::nullopt;
    }
}
