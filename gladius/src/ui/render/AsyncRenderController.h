#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <coro/coro.hpp>

#include "AsyncRenderTypes.h"
#include "FramePresentationController.h"

#include "../../ComputeTypes.h"

// Forward declarations for OpenCL types
namespace cl
{
    class CommandQueue;
    class Image2D;
}

namespace gladius
{
    class ConfigManager;
    class ComputeContext;
}

namespace gladius::ui::async_rendering
{
    /**
     * @brief Feature flag configuration for the async rendering backend.
     */
    struct AsyncRenderFeatureConfig
    {
        bool enabled{true};
        std::string implementation{"legacy"};
        std::string bufferingMode{"double"};

        [[nodiscard]] bool wantsCoroutineBackend() const noexcept
        {
            return enabled && implementation == "coroutines";
        }
    };

    [[nodiscard]] AsyncRenderFeatureConfig
      loadAsyncRenderFeatureConfig(gladius::ConfigManager const * configManager);

    /**
     * @brief Coordinates coroutine-based async rendering worker lifecycle.
     *
     * This initial scaffold focuses on providing the foundational job queue and worker management
     * that later stages (C2+) will extend with actual rendering logic.
     *
     * For Option A (OpenCL-Only Async), this controller owns:
     * - A dedicated worker command queue (separate from UI thread's queue)
     * - A staging buffer (CL-only, no GL interop) for async rendering
     * - CPU-side pixel buffer for async pixel transfers
     */
    class AsyncRenderController
    {
      public:
        using CancelCheck = std::function<bool()>;
        using JobExecutor = std::function<coro::task<FrameResultMeta>(RenderJob const &,
                                                                      CancelCheck const &)>;

        AsyncRenderController();
        explicit AsyncRenderController(std::shared_ptr<coro::thread_pool> workerPool);
        ~AsyncRenderController();

        AsyncRenderController(AsyncRenderController const &) = delete;
        AsyncRenderController & operator=(AsyncRenderController const &) = delete;
        AsyncRenderController(AsyncRenderController &&) = delete;
        AsyncRenderController & operator=(AsyncRenderController &&) = delete;

        void start();
        void stop();

        void enqueueJob(RenderJob job);
        void setLatestEpoch(uint64_t epoch);
        void setLatestViewEpoch(uint64_t viewEpoch);
        void setJobExecutor(JobExecutor executor);
        [[nodiscard]] std::optional<FrameResultMeta> tryConsumeResult();

        [[nodiscard]] bool isRunning() const noexcept;
        [[nodiscard]] std::shared_ptr<coro::thread_pool> workerPool() const noexcept;

        /// Initialize OpenCL resources for async rendering (worker queue + staging buffer)
        void initializeAsyncResources(ComputeContext & context, size_t width, size_t height);

        /// Get worker-specific command queue (used by worker thread for rendering)
        [[nodiscard]] cl::CommandQueue * workerQueue() noexcept;
        /// Thread-safe snapshot — safe to call from worker threads concurrently with
        /// initializeAsyncResources(). Holds the queue alive for the caller's lifetime.
        [[nodiscard]] std::shared_ptr<cl::CommandQueue> workerQueueShared() const noexcept;

        /// Get staging buffer (used by worker thread for rendering output)
        [[nodiscard]] cl::Image2D * stagingBuffer() noexcept;

        /// Download pixels from staging buffer to CPU memory (called from UI thread)
        void downloadStagingBufferAsync(std::vector<cl_float4> & outPixels);

        /// Get a buffer in a specific state (for buffer management)
        [[nodiscard]] FrameBuffer * findBufferInState(FrameState state) noexcept;

        /// Try to transition a buffer from one state to another atomically
        [[nodiscard]] bool tryTransitionBuffer(FrameBuffer * buffer,
                                              FrameState expectedState,
                                              FrameState newState) noexcept;

        /// Get the current front buffer (displayed frame)
        [[nodiscard]] FrameBuffer * frontBuffer() noexcept;

        /// Get a writable buffer for the worker (finds Idle buffer)
        [[nodiscard]] FrameBuffer * acquireWriteBuffer(uint64_t epoch) noexcept;

        /// Publish a finished frame (Writing → Ready)
        void publishFrame(FrameBuffer * buffer,
                          uint64_t frameId,
                          uint64_t epoch,
                          uint64_t viewEpoch = 0,
                          FramePresentationQuality quality =
                            FramePresentationQuality::Unknown) noexcept;

        /// Publish a finished frame with the coordinator stamp used for presentation policy.
        void publishFrame(FrameBuffer * buffer,
                          uint64_t frameId,
                          uint64_t epoch,
                          uint64_t viewEpoch,
                          RenderStamp const & stamp,
                          FramePresentationQuality quality) noexcept;

        /// Snapshot mirrored presentation metadata for a specific HQ buffer.
        [[nodiscard]] std::optional<PresentationBuffer>
          mirroredPresentationBuffer(FrameBuffer const * buffer) const noexcept;

        /// Snapshot mirrored presentation metadata for the currently presented front buffer.
        [[nodiscard]] std::optional<PresentationBuffer> mirroredFrontPresentationBuffer() const noexcept;

        /// Promote a Ready buffer to Front (for UI display)
        [[nodiscard]] FrameBuffer * promoteReadyToFront() noexcept;

        /// Promote the best Ready buffer that is acceptable for the requested display stamp.
        [[nodiscard]] FrameBuffer * promoteReadyToFront(RenderStamp const & required,
                                                        RenderStampMask mask) noexcept;

        /// Check whether an external/non-HQ candidate may be presented against the current front.
        [[nodiscard]] bool canPresentFrame(RenderStamp const & candidateStamp,
                                           FramePresentationQuality candidateQuality,
                                           RenderStamp const & required,
                                           RenderStampMask mask) const noexcept;

          /// Release a completed Ready frame that should no longer be displayed.
          void discardReadyFrame(uint64_t frameId, uint64_t epoch, uint64_t viewEpoch) noexcept;

          /// Finalize promotion by transitioning Resampling → Front and updating indices
          [[nodiscard]] bool finalizeFrontPromotion(FrameBuffer * buffer) noexcept;

        /// Release any Writing buffers from old epochs back to Idle
        /// (used when epoch changes to clean up cancelled jobs)
        void releaseStaleBuffers(uint64_t oldEpoch) noexcept;

        // ============== Preview Buffer Management ==============
        // Separate from HQ triple buffer to avoid contention

        /// Acquire a preview buffer for writing (returns nullptr if none available)
        [[nodiscard]] FrameBuffer * acquirePreviewBuffer(uint64_t epoch) noexcept;

        /// Publish completed preview frame (Writing → Ready)
        void publishPreviewFrame(FrameBuffer * buffer, PreviewResultMeta const & meta) noexcept;

        /// Try to consume a completed preview result (non-blocking)
        [[nodiscard]] std::optional<PreviewResultMeta> tryConsumePreviewResult() noexcept;

        /// Get the current front preview buffer (may be nullptr if no preview rendered yet)
        [[nodiscard]] FrameBuffer * frontPreviewBuffer() noexcept;

        /// Promote Ready preview buffer to Front (for UI display)
        [[nodiscard]] FrameBuffer * promotePreviewReadyToFront() noexcept;

        /// Release stale preview buffers when epoch changes
        void releaseStalePreviewBuffers(uint64_t oldEpoch) noexcept;

        /// Directly store a preview result for UI thread consumption
        void setLatestPreviewResult(PreviewResultMeta const & meta) noexcept;

      private:
        struct ControllerState;

        [[nodiscard]] static auto workerLoop(std::shared_ptr<ControllerState> state)
          -> coro::task<void>;

        [[nodiscard]] std::optional<size_t>
          frameBufferIndex(FrameBuffer const * buffer) const noexcept;
        [[nodiscard]] bool tryTransitionBufferLocked(FrameBuffer * buffer,
                                                     FrameState expectedState,
                                                     FrameState newState) noexcept;

        std::shared_ptr<ControllerState> m_state;
        std::atomic<bool> m_running{false};

      #if defined(GLADIUS_ENABLE_OPENCL)
        // OpenCL resources for async rendering (Option A: separate CL queue, no GL interop)
        mutable std::mutex m_workerQueueMutex;
        std::shared_ptr<cl::CommandQueue> m_workerQueue;
        std::unique_ptr<cl::Image2D> m_stagingBuffer;
        size_t m_stagingWidth{0};
        size_t m_stagingHeight{0};
      #endif

        // Triple buffer state machine (for HQ progressive rendering)
        std::array<FrameBuffer, 3> m_frameBuffers;
        FramePresentationController m_framePresentation{m_frameBuffers.size()};
        std::atomic<size_t> m_frontBufferIndex{0};
        mutable std::mutex m_bufferMutex;

        // Preview buffer state machine (separate from HQ, double buffer)
        std::array<FrameBuffer, 2> m_previewBuffers;
        std::atomic<size_t> m_previewFrontIndex{0};
        mutable std::mutex m_previewBufferMutex;
        
        // Lock-free queue for preview results
        std::optional<PreviewResultMeta> m_latestPreviewResult;
        std::atomic<bool> m_previewResultReady{false};
    };
}
