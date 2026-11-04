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
        }

        if (m_progressiveNextRow >= task.height)
        {
            m_progressiveNextRow = 0u;
        }

        auto const remainingRows = task.height - m_progressiveNextRow;
        auto const rowsPerChunk = std::max<std::size_t>(
          1u,
          MAX_PROGRESSIVE_CHUNK_PIXELS / static_cast<std::size_t>(task.width));
        submissionTask.startLine = m_progressiveNextRow;
        submissionTask.lineCount = std::min<std::size_t>(remainingRows, rowsPerChunk);
        return submissionTask;
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
