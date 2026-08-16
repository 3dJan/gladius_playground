#include "ui/render/NeutralRenderScheduler.h"

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

        auto request = m_requestFactory(task);
        if (!request.has_value() || !request->isValid())
        {
            return false;
        }

        try
        {
            return m_submissions.track(task, renderer.submitFrame(scene, std::move(*request)));
        }
        catch (...)
        {
            return false;
        }
    }

    bool NeutralRenderScheduler::submit(RenderTaskRequest const & task,
                                        compute::RenderBackendSession & session)
    {
        if (!isSupportedDisplayTask(task.type) || !session.isAvailable() ||
            !session.hasMaterializedScene() ||
            !matches(task.stamp, m_workflow.latestStamp(), RenderStampMask::displayFrame()) ||
            task.stamp.sceneEpoch != session.getSceneGeneration())
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
            return m_submissions.track(task, session.submitFrame(std::move(*request)));
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
            auto decision = m_workflow.completeTask(completion.taskResult, scheduleFollowUp);
            result.commands.insert(result.commands.end(),
                                   std::make_move_iterator(decision.commands.begin()),
                                   std::make_move_iterator(decision.commands.end()));

            if (completion.frame.has_value())
            {
                auto candidate = findCandidate(decision, completion.taskResult.requestId);
                if (candidate.has_value())
                {
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
