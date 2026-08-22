#pragma once

#include "FramePresentationTypes.h"

#include <optional>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Metadata-only ledger for the frame currently considered visible.
     *
     * The ledger is intentionally independent from OpenGL/OpenCL resources. It records the
     * presentation stamp and quality of the visible frame so preview, realtime, and progressive
     * paths can share the same freshness and quality-regression checks.
     */
    class PresentedFrameLedger
    {
      public:
        [[nodiscard]] std::optional<PresentedFrame> const & presentedFrame() const noexcept
        {
            return m_presentedFrame;
        }

        void seedPresentedFrame(PresentedFrame frame)
        {
            m_presentedFrame = frame;
        }

        void clearPresentedFrame() noexcept
        {
            m_presentedFrame.reset();
        }

        [[nodiscard]] bool canPresentCandidate(
          FramePresentationCandidate const & candidate,
          RenderStamp const & requiredStamp,
          RenderStampMask const mask = RenderStampMask::displayFrame()) const noexcept
        {
            if (!matches(candidate.stamp, requiredStamp, mask))
            {
                return false;
            }

            if (!m_presentedFrame.has_value())
            {
                return true;
            }

            auto const & current = *m_presentedFrame;
            if (!matches(current.stamp, requiredStamp, mask))
            {
                return true;
            }

            if (!mask.view && candidate.stamp.viewEpoch < current.stamp.viewEpoch)
            {
                return false;
            }

            return framePresentationQualityRank(candidate.quality) >=
                   framePresentationQualityRank(current.quality);
        }

        [[nodiscard]] bool presentCandidate(
          FramePresentationCandidate const & candidate,
          RenderStamp const & requiredStamp,
          RenderStampMask const mask = RenderStampMask::displayFrame()) noexcept
        {
            if (!canPresentCandidate(candidate, requiredStamp, mask))
            {
                return false;
            }

            m_presentedFrame = PresentedFrame{.frameId = candidate.frameId,
                                              .stamp = candidate.stamp,
                                              .quality = candidate.quality,
                                              .source = candidate.source,
                                              .completedFrame = candidate.completedFrame};
            return true;
        }

      private:
        std::optional<PresentedFrame> m_presentedFrame;
    };
}
