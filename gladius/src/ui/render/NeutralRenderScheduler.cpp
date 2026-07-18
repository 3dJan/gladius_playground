#include "ui/render/NeutralRenderScheduler.h"

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
        if (!isSupportedDisplayTask(task.type) || !renderer.isAvailable())
        {
            return false;
        }

        auto request = m_requestFactory(task);
        if (!request.has_value() || !request->isValid())
        {
            return false;
        }

        try
        {
            return m_submissions.track(task, renderer.submitFrame(std::move(*request)));
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

    std::vector<AcceptedNeutralFrame> NeutralRenderScheduler::poll()
    {
        std::vector<AcceptedNeutralFrame> acceptedFrames;
        for (auto & completion : m_submissions.poll())
        {
            auto decision = m_workflow.completeTask(completion.taskResult);
            if (!completion.frame.has_value())
            {
                continue;
            }

            auto candidate = findCandidate(decision, completion.taskResult.requestId);
            if (candidate.has_value())
            {
                acceptedFrames.push_back(
                  AcceptedNeutralFrame{.frame = std::move(*completion.frame), .candidate = *candidate});
            }
        }
        return acceptedFrames;
    }

    RenderWorkflowController & NeutralRenderScheduler::workflow() noexcept
    {
        return m_workflow;
    }

    RenderWorkflowController const & NeutralRenderScheduler::workflow() const noexcept
    {
        return m_workflow;
    }

    bool NeutralRenderScheduler::hasInFlightSubmissions() const noexcept
    {
        return !m_submissions.empty();
    }

    bool NeutralRenderScheduler::isSupportedDisplayTask(RenderTaskType const type) noexcept
    {
        return type == RenderTaskType::RealtimeFullFrame || type == RenderTaskType::StaticFullFrameProbe ||
               type == RenderTaskType::ProgressiveHighQualityChunk;
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
