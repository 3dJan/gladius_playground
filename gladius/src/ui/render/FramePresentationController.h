#pragma once

#include "AsyncRenderTypes.h"
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
                return candidate.id;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool publishFrame(size_t bufferId,
                                        uint64_t frameId,
                                        RenderStamp const & stamp) noexcept
        {
            auto * target = mutableBuffer(bufferId);
            if (target == nullptr || target->state != FrameState::Writing)
            {
                return false;
            }

            target->frameId = frameId;
            target->stamp = stamp;
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

        std::vector<PresentationBuffer> m_buffers;
        std::optional<size_t> m_frontBufferId;
    };
}
