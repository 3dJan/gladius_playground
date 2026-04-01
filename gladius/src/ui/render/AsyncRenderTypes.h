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
        LowResPreview,
        StreamingPreview,     // Continuous preview loop during parameter drag
        BoundingBoxUpdate,
        ParameterUpdate,      // Fast path: parameter values changed without structure change
        SDFPrecomputation,    // Async SDF generation after model/bbox update
        ProgramCompilation    // Async OpenCL program compilation
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
        bool precomputedSdf{false};  // True if SDF should be precomputed before rendering
    };

    /**
     * @brief Metadata for completed frames shared between worker and UI threads.
     */
    struct FrameResultMeta
    {
        uint64_t frameId{0};
        uint64_t epoch{0};
        RenderJobType jobType{RenderJobType::HighQuality};
        uint32_t width{0};
        uint32_t height{0};
        bool cancelled{false};
        bool completedFrame{false};
        bool precomputedSdfUpdated{false};
        size_t completedLine{0};
        uint64_t computeDurationNs{0};
        float compilationProgress{0.0f};  // 0.0 - 1.0 for compilation jobs
        bool compilationSucceeded{false};
        std::string compilationError;     // Empty if no error
    };

    /**
     * @brief Describes a low-resolution preview render job for async execution.
     * 
     * Preview jobs are high-priority, latency-sensitive jobs that execute
     * during camera movement to provide visual feedback to the user.
     */
    struct PreviewRenderJob
    {
        uint64_t epoch{0};           ///< Cancellation token (job cancelled if epoch < current)
        uint64_t frameId{0};         ///< Unique frame identifier for tracking
        uint32_t width{0};           ///< Preview resolution width
        uint32_t height{0};          ///< Preview resolution height
        CameraSnapshot camera{};     ///< Camera state at job creation time
        bool requiresSdfValid{true}; ///< True if SDF must be precomputed before rendering
    };

    /**
     * @brief Metadata for completed preview frames shared between worker and UI threads.
     */
    struct PreviewResultMeta
    {
        uint64_t frameId{0};         ///< Matches PreviewRenderJob::frameId
        uint64_t epoch{0};           ///< Matches PreviewRenderJob::epoch
        uint64_t latencyNs{0};       ///< Time from job enqueue to completion (nanoseconds)
        bool cancelled{false};       ///< True if job was cancelled due to epoch change
        bool sdfWasValid{false};     ///< True if SDF was available during rendering
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
