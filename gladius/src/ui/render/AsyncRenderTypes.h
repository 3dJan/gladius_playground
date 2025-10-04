#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../types.h"

namespace gladius
{
    class GLImageBuffer;
}

namespace gladius::ui::async_rendering
{
    /**
     * @brief Lifecycle states for a frame buffer in the async rendering pipeline.
     */
    enum class FrameState
    {
        Idle,
        Writing,
        Ready,
        Resampling,
        Front
    };

    /**
     * @brief Snapshot of the camera state required for render jobs.
     */
    struct CameraSnapshot
    {
        Vector3 position{0.0f, 0.0f, 0.0f};
        Vector3 target{0.0f, 0.0f, 0.0f};
        Vector3 up{0.0f, 1.0f, 0.0f};
        float distance{0.0f};
        bool isPerspective{true};
    };

    enum class RenderJobType
    {
        HighQuality,
        LowResPreview
    };

    /**
     * @brief Describes a rendering job request enqueued by the UI thread.
     */
    struct RenderJob
    {
        uint64_t epoch{0};
        uint64_t frameHint{0};
        RenderJobType type{RenderJobType::HighQuality};
        uint32_t width{0};
        uint32_t height{0};
        size_t startLine{0};
        size_t stepSize{1};
        bool precomputeSdf{false};
        bool enableHighQuality{true};
    };

    /**
     * @brief Metadata for completed frames shared between worker and UI threads.
     */
    struct FrameResultMeta
    {
        uint64_t frameId{0};
        uint64_t epoch{0};
        uint32_t width{0};
        uint32_t height{0};
        bool cancelled{false};
        bool completedFrame{false};
        bool precomputedSdfUpdated{false};
        size_t completedLine{0};
        uint64_t computeDurationNs{0};
    };

    /**
     * @brief Represents a single buffered frame used in the async pipeline.
     */
    struct FrameBuffer
    {
        std::shared_ptr<GLImageBuffer> image;
        std::atomic<FrameState> state{FrameState::Idle};
        std::atomic<uint64_t> frameId{0};
        std::atomic<uint64_t> epoch{0};
        std::atomic<uint64_t> readyTimestampNs{0};
        std::atomic<uint64_t> publishTimestampNs{0};

        FrameBuffer() = default;
        explicit FrameBuffer(std::shared_ptr<GLImageBuffer> buffer)
            : image(std::move(buffer))
        {
        }
    };
}
