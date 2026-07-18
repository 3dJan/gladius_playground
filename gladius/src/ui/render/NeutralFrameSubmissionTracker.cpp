#include "ui/render/NeutralFrameSubmissionTracker.h"

#include <algorithm>

namespace gladius::ui::async_rendering
{
  NeutralFrameSubmissionTracker::~NeutralFrameSubmissionTracker()
  {
    try
    {
      (void) drain();
    }
    catch (...)
    {
    }
  }

    bool NeutralFrameSubmissionTracker::track(RenderTaskRequest task,
                                              std::unique_ptr<compute::IRenderSubmission> submission)
    {
        if (!submission || task.requestId == 0u ||
            std::any_of(m_submissions.begin(),
                        m_submissions.end(),
                        [requestId = task.requestId](TrackedSubmission const & tracked)
                        { return tracked.task.requestId == requestId; }))
        {
            return false;
        }

        m_submissions.push_back(
          TrackedSubmission{.task = task, .submission = std::move(submission), .submittedAt = std::chrono::steady_clock::now()});
        return true;
    }

    void NeutralFrameSubmissionTracker::requestCancellationForStale(RenderStamp const & currentStamp) noexcept
    {
        for (auto const & tracked : m_submissions)
        {
            if (!matches(tracked.task.stamp, currentStamp, RenderStampMask::displayFrame()))
            {
                tracked.submission->requestCancellation();
            }
        }
    }

      void NeutralFrameSubmissionTracker::requestCancellationForAll() noexcept
      {
        for (auto const & tracked : m_submissions)
        {
          tracked.submission->requestCancellation();
        }
      }

    std::vector<NeutralFrameSubmissionResult> NeutralFrameSubmissionTracker::poll()
    {
        std::vector<NeutralFrameSubmissionResult> results;
        auto const retainedEnd = std::remove_if(m_submissions.begin(),
                                                m_submissions.end(),
                                                [&results](TrackedSubmission & tracked)
                                                {
                                                  tracked.submission->progress();
                                                    auto const status = tracked.submission->getStatus();
                                                    if (status == compute::RenderSubmissionStatus::Pending)
                                                    {
                                                        return false;
                                                    }

                                                    if (status == compute::RenderSubmissionStatus::Succeeded)
                                                    {
                                                        auto frame = tracked.submission->takeFrame();
                                                        if (frame.has_value() && matchesTask(*frame, tracked.task))
                                                        {
                                                            auto const completedFrame = frame->firstRow == 0u &&
                                                                                        frame->endRow == frame->height;
                                                            results.push_back(
                                                              NeutralFrameSubmissionResult{
                                                                .taskResult = makeTerminalResult(
                                                                  tracked, RenderTaskStatus::Completed, true, completedFrame),
                                                                .frame = std::move(frame)});
                                                        }
                                                        else
                                                        {
                                                            results.push_back(
                                                              NeutralFrameSubmissionResult{
                                                                .taskResult = makeTerminalResult(
                                                                  tracked, RenderTaskStatus::Failed, false, false),
                                                                .errorMessage = "Backend frame did not match its render task"});
                                                        }
                                                    }
                                                    else
                                                    {
                                                        auto const taskStatus = status == compute::RenderSubmissionStatus::Cancelled
                                                                                  ? RenderTaskStatus::Cancelled
                                                                                  : RenderTaskStatus::Failed;
                                                        results.push_back(
                                                          NeutralFrameSubmissionResult{
                                                            .taskResult = makeTerminalResult(tracked, taskStatus, false, false),
                                                            .errorMessage = tracked.submission->getErrorMessage()});
                                                    }
                                                    return true;
                                                });
        m_submissions.erase(retainedEnd, m_submissions.end());
        return results;
    }

      std::vector<NeutralFrameSubmissionResult> NeutralFrameSubmissionTracker::drain()
      {
        requestCancellationForAll();
        for (auto const & tracked : m_submissions)
        {
          tracked.submission->wait();
        }
        return poll();
      }

    bool NeutralFrameSubmissionTracker::empty() const noexcept
    {
        return m_submissions.empty();
    }

    bool NeutralFrameSubmissionTracker::matchesTask(compute::RenderFrame const & frame,
                                                    RenderTaskRequest const & task) noexcept
    {
        auto const expectedEndRow = task.startLine + task.lineCount;
        return frame.isValid() && frame.width == task.width && frame.height == task.height &&
               frame.firstRow == task.startLine && frame.endRow == expectedEndRow &&
               frame.freshness.sceneGeneration == task.stamp.sceneEpoch &&
               frame.freshness.viewGeneration == task.stamp.viewEpoch &&
               frame.freshness.parameterGeneration == task.stamp.parameterEpoch;
    }

    RenderTaskResult NeutralFrameSubmissionTracker::makeTerminalResult(
      TrackedSubmission const & tracked,
      RenderTaskStatus const status,
      bool const producedDisplayFrame,
      bool const completedFrame) noexcept
    {
        auto const elapsed = std::chrono::steady_clock::now() - tracked.submittedAt;
        auto const duration = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        return RenderTaskResult{.requestId = tracked.task.requestId,
                                .type = tracked.task.type,
                                .stamp = tracked.task.stamp,
                                .status = status,
                                .durationNs = static_cast<std::uint64_t>(duration),
                                .producedDisplayFrame = producedDisplayFrame,
                                .completedFrame = completedFrame};
    }
}
