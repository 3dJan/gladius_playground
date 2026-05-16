#pragma once

#include "AsyncRenderTypes.h"
#include "FramePresentationTypes.h"
#include "RenderUpdateTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gladius::ui::async_rendering
{
    /**
     * @brief Metadata-only buffer used by FramePresentationController.
     */
    struct PresentationBuffer
    {
        size_t id{0};
        FrameState state{FrameState::Idle};
        uint64_t frameId{0};
        RenderStamp stamp{};
        FramePresentationQuality quality{FramePresentationQuality::Unknown};
    };

    /**
     * @brief Pure state machine for selecting, publishing, and promoting rendered frames.
     *
     * This class intentionally knows nothing about OpenGL/OpenCL resources. Production buffer
     * owners can use it as the policy layer while keeping actual image allocation elsewhere.
     */
    class FramePresentationController
    {
      public:
        explicit FramePresentationController(size_t bufferCount = 3)
        {
            reset(bufferCount);
        }

        void reset(size_t bufferCount)
        {
            m_buffers.clear();
            m_buffers.reserve(bufferCount);
            for (size_t index = 0; index < bufferCount; ++index)
            {
                m_buffers.push_back(PresentationBuffer{.id = index});
            }

            if (!m_buffers.empty())
            {
                m_buffers.front().state = FrameState::Front;
                m_frontBufferId = m_buffers.front().id;
            }
            else
            {
                m_frontBufferId.reset();
            }
        }

        [[nodiscard]] std::optional<size_t> frontBufferId() const noexcept
        {
            return m_frontBufferId;
        }

        [[nodiscard]] PresentationBuffer const * buffer(size_t bufferId) const noexcept
        {
            if (bufferId >= m_buffers.size())
            {
                return nullptr;
            }
            return &m_buffers[bufferId];
        }

        [[nodiscard]] std::optional<size_t> acquireWriteBuffer(RenderStamp const & stamp) noexcept
        {
            for (auto & candidate : m_buffers)
            {
                if (candidate.state != FrameState::Idle)
                {
                    continue;
                }

                candidate.state = FrameState::Writing;
                candidate.frameId = 0;
                candidate.stamp = stamp;
                candidate.quality = FramePresentationQuality::Unknown;
                return candidate.id;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool publishFrame(size_t bufferId,
                                        uint64_t frameId,
                                        RenderStamp const & stamp) noexcept
        {
            return publishFrame(
              bufferId, frameId, stamp, FramePresentationQuality::Unknown);
        }

        [[nodiscard]] bool publishFrame(size_t bufferId,
                                        uint64_t frameId,
                                        RenderStamp const & stamp,
                                        FramePresentationQuality quality) noexcept
        {
            auto * target = mutableBuffer(bufferId);
            if (target == nullptr || target->state != FrameState::Writing)
            {
                return false;
            }

            target->frameId = frameId;
            target->stamp = stamp;
            target->quality = quality;
            target->state = FrameState::Ready;
            return true;
        }

        [[nodiscard]] bool tryTransitionBuffer(size_t bufferId,
                                               FrameState expectedState,
                                               FrameState newState) noexcept
        {
            auto * target = mutableBuffer(bufferId);
            if (target == nullptr || target->state != expectedState)
            {
                return false;
            }

            target->state = newState;
            if (newState == FrameState::Idle)
            {
                target->frameId = 0;
                target->quality = FramePresentationQuality::Unknown;
            }
            if (newState == FrameState::Front)
            {
                m_frontBufferId = target->id;
            }
            return true;
        }

        [[nodiscard]] std::optional<size_t> selectNewestReady() noexcept
        {
            return selectNewestReady(RenderStamp{}, RenderStampMask::none());
        }

        [[nodiscard]] std::optional<size_t> selectNewestReady(RenderStamp const & required,
                                                              RenderStampMask const mask) noexcept
        {
            PresentationBuffer * newest = nullptr;
            for (auto & candidate : m_buffers)
            {
                if (candidate.state != FrameState::Ready || !matches(candidate.stamp, required, mask))
                {
                    continue;
                }

                if (newest == nullptr || candidate.frameId > newest->frameId)
                {
                    newest = &candidate;
                }
            }

            if (newest == nullptr)
            {
                return std::nullopt;
            }

            newest->state = FrameState::Resampling;
            return newest->id;
        }

        [[nodiscard]] std::optional<size_t> selectBestReadyForPresentation(
          RenderStamp const & required,
          RenderStampMask const mask) noexcept
        {
            PresentationBuffer * best = nullptr;
            for (auto & candidate : m_buffers)
            {
                if (candidate.state != FrameState::Ready ||
                    !matches(candidate.stamp, required, mask))
                {
                    continue;
                }

                if (!canPresentCandidate(FramePresentationCandidate{.frameId = candidate.frameId,
                                                                    .stamp = candidate.stamp,
                                                                    .quality = candidate.quality},
                                         required,
                                         mask))
                {
                    continue;
                }

                if (best == nullptr)
                {
                    best = &candidate;
                    continue;
                }

                auto const candidateRank = framePresentationQualityRank(candidate.quality);
                auto const bestRank = framePresentationQualityRank(best->quality);
                if (candidateRank > bestRank ||
                    (candidateRank == bestRank && candidate.frameId > best->frameId))
                {
                    best = &candidate;
                }
            }

            if (best == nullptr)
            {
                return std::nullopt;
            }

            best->state = FrameState::Resampling;
            return best->id;
        }

        [[nodiscard]] bool canPresentCandidate(FramePresentationCandidate const & candidate,
                                               RenderStamp const & required,
                                               RenderStampMask const mask) const noexcept
        {
            if (!matches(candidate.stamp, required, mask))
            {
                return false;
            }

            auto const * currentFront = currentFrontMatching(required, mask);
            if (currentFront == nullptr)
            {
                return true;
            }

            return framePresentationQualityRank(candidate.quality) >=
                   framePresentationQualityRank(currentFront->quality);
        }

        [[nodiscard]] bool finalizeFrontPromotion(size_t bufferId) noexcept
        {
            auto * target = mutableBuffer(bufferId);
            if (target == nullptr || target->state != FrameState::Resampling)
            {
                return false;
            }

            if (m_frontBufferId.has_value())
            {
                auto * previousFront = mutableBuffer(*m_frontBufferId);
                if (previousFront != nullptr && previousFront->id != target->id &&
                    previousFront->state == FrameState::Front)
                {
                    previousFront->state = FrameState::Idle;
                    previousFront->frameId = 0;
                    previousFront->quality = FramePresentationQuality::Unknown;
                }
            }

            target->state = FrameState::Front;
            m_frontBufferId = target->id;
            return true;
        }

        [[nodiscard]] bool discardReadyFrame(size_t bufferId) noexcept
        {
            auto * target = mutableBuffer(bufferId);
            if (target == nullptr || target->state != FrameState::Ready)
            {
                return false;
            }

            target->state = FrameState::Idle;
            target->frameId = 0;
            target->quality = FramePresentationQuality::Unknown;
            return true;
        }

        [[nodiscard]] size_t releaseStaleWritingBuffers(RenderStamp const & required,
                                                        RenderStampMask const mask) noexcept
        {
            size_t releasedCount = 0;
            for (auto & candidate : m_buffers)
            {
                if (candidate.state != FrameState::Writing || !isOlderThan(candidate.stamp, required, mask))
                {
                    continue;
                }

                candidate.state = FrameState::Idle;
                candidate.frameId = 0;
                candidate.quality = FramePresentationQuality::Unknown;
                ++releasedCount;
            }
            return releasedCount;
        }

      private:
        [[nodiscard]] PresentationBuffer * mutableBuffer(size_t bufferId) noexcept
        {
            if (bufferId >= m_buffers.size())
            {
                return nullptr;
            }
            return &m_buffers[bufferId];
        }

        [[nodiscard]] PresentationBuffer const * currentFrontMatching(
          RenderStamp const & required,
          RenderStampMask const mask) const noexcept
        {
            if (!m_frontBufferId.has_value())
            {
                return nullptr;
            }

            auto const * front = buffer(*m_frontBufferId);
            if (front == nullptr || front->state != FrameState::Front ||
                !matches(front->stamp, required, mask))
            {
                return nullptr;
            }
            return front;
        }

        std::vector<PresentationBuffer> m_buffers;
        std::optional<size_t> m_frontBufferId;
    };
}
