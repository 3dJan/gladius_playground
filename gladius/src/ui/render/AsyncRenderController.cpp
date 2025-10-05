#include "AsyncRenderController.h"

#include <ComputeContext.h>
#include <ConfigManager.h>
#include <GLImageBuffer.h>
#include <Profiling.h>
#include <coro/queue.hpp>
#include <coro/sync_wait.hpp>

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#include <OpenCL/opencl.hpp>
#else
#include <CL/opencl.h>
#include <CL/opencl.hpp>
#endif

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace gladius::ui::async_rendering
{
    namespace
    {
        constexpr char const * IMPLEMENTATION_KEY = "impl";
        constexpr char const * ENABLED_KEY = "enabled";
        constexpr char const * BUFFERING_KEY = "buffering";
        constexpr char const * SECTION = "asyncRendering";

        struct AsyncDefaults
        {
            static constexpr bool kEnabled = true;
            static constexpr char const * kImplementation = "coroutines";
            static constexpr char const * kBufferingMode = "double";
        };

        struct ControllerStateData
        {
            std::shared_ptr<coro::thread_pool> workerPool;
            coro::queue<RenderJob> jobQueue;
            std::atomic<bool> shutdownRequested{false};
            std::atomic<uint64_t> latestEpoch{0};
            AsyncRenderController::JobExecutor jobExecutor;

            std::mutex resultMutex;
            std::optional<FrameResultMeta> pendingResult;
            std::atomic<bool> hasPendingResult{false};

            std::mutex lifecycleMutex;
            std::condition_variable lifecycleCv;
            bool workerActive{false};

            explicit ControllerStateData(std::shared_ptr<coro::thread_pool> pool)
                : workerPool(std::move(pool))
            {
            }
        };
    }

    struct AsyncRenderController::ControllerState
    {
        explicit ControllerState(std::shared_ptr<coro::thread_pool> pool)
            : data(std::move(pool))
        {
        }

        ControllerStateData data;
    };

    AsyncRenderFeatureConfig
      loadAsyncRenderFeatureConfig(gladius::ConfigManager const * configManager)
    {
        AsyncRenderFeatureConfig config{};
        if (configManager == nullptr)
        {
            return config;
        }

        config.enabled = configManager->getValue<bool>(SECTION, ENABLED_KEY, AsyncDefaults::kEnabled);
        config.implementation = configManager->getValue<std::string>(SECTION,
                                                                     IMPLEMENTATION_KEY,
                                                                     AsyncDefaults::kImplementation);
        config.bufferingMode = configManager->getValue<std::string>(SECTION,
                                                                    BUFFERING_KEY,
                                                                    AsyncDefaults::kBufferingMode);
        return config;
    }

    AsyncRenderController::AsyncRenderController()
        : AsyncRenderController(coro::thread_pool::make_shared(coro::thread_pool::options{.thread_count = 2}))
    {
    }

    AsyncRenderController::AsyncRenderController(std::shared_ptr<coro::thread_pool> workerPool)
    {
        if (workerPool == nullptr)
        {
            workerPool = coro::thread_pool::make_shared(coro::thread_pool::options{.thread_count = 2});
        }

        m_state = std::make_shared<ControllerState>(std::move(workerPool));
    }

    AsyncRenderController::~AsyncRenderController()
    {
        stop();

        // Clean up OpenCL resources
        m_stagingBuffer.reset();
        m_workerQueue.reset();
    }

    void AsyncRenderController::start()
    {
        ProfileFunction

        if (!m_state)
        {
            DebugText("NoState", 7);
            return;
        }

        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            DebugText("AlreadyRunning", 14);
            return;
        }

        m_state->data.shutdownRequested.store(false, std::memory_order_release);
        m_state->data.latestEpoch.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> resultLock(m_state->data.resultMutex);
            m_state->data.pendingResult.reset();
            m_state->data.hasPendingResult.store(false, std::memory_order_release);
        }

        DebugText("SpawningWorker", 14);
        auto const spawned = m_state->data.workerPool->spawn(workerLoop(m_state));
        if (!spawned)
        {
            DebugText("SpawnFailed", 11);
            m_running.store(false, std::memory_order_release);
            m_state->data.shutdownRequested.store(true, std::memory_order_release);
        }
        else
        {
            DebugText("SpawnSuccess", 12);
        }
    }

    void AsyncRenderController::stop()
    {
        if (!m_state)
        {
            return;
        }

        bool const wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
        if (!wasRunning)
        {
            return;
        }

        m_state->data.shutdownRequested.store(true, std::memory_order_release);
        coro::sync_wait(m_state->data.jobQueue.shutdown());

        std::unique_lock<std::mutex> lock(m_state->data.lifecycleMutex);
        m_state->data.lifecycleCv.wait(lock, [data = &m_state->data]() { return !data->workerActive; });
    }

    void AsyncRenderController::enqueueJob(RenderJob job)
    {
        if (!m_state || !isRunning())
        {
            return;
        }

        setLatestEpoch(job.epoch);
        auto result = coro::sync_wait(m_state->data.jobQueue.push(std::move(job)));
        if (result != coro::queue_produce_result::produced)
        {
            // Queue rejected the job (likely shutting down). There is nothing else to do for now.
        }
    }

    void AsyncRenderController::setLatestEpoch(uint64_t epoch)
    {
        if (!m_state)
        {
            return;
        }

        auto & latestEpoch = m_state->data.latestEpoch;
        auto current = latestEpoch.load(std::memory_order_acquire);
        while (epoch > current &&
               !latestEpoch.compare_exchange_weak(current,
                                                  epoch,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
        {
        }
    }

    void AsyncRenderController::setJobExecutor(JobExecutor executor)
    {
        if (!m_state)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_state->data.resultMutex);
        m_state->data.jobExecutor = std::move(executor);
    }

    std::optional<FrameResultMeta> AsyncRenderController::tryConsumeResult()
    {
        if (!m_state)
        {
            return std::nullopt;
        }

        auto & data = m_state->data;
        if (!data.hasPendingResult.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(data.resultMutex);
        if (!data.pendingResult.has_value())
        {
            data.hasPendingResult.store(false, std::memory_order_release);
            return std::nullopt;
        }

        FrameResultMeta result = std::move(*data.pendingResult);
        data.pendingResult.reset();
        data.hasPendingResult.store(false, std::memory_order_release);
        return result;
    }

    bool AsyncRenderController::isRunning() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    std::shared_ptr<coro::thread_pool> AsyncRenderController::workerPool() const noexcept
    {
        if (!m_state)
        {
            return nullptr;
        }
        return m_state->data.workerPool;
    }

    auto AsyncRenderController::workerLoop(std::shared_ptr<ControllerState> state) -> coro::task<void>
    {
        ZoneScopedN("workerLoop");
        DebugText("WorkerStarting", 14);
        tracy::SetThreadName("AsyncRenderWorker");
        
        if (!state)
        {
            DebugText("NoState", 7);
            co_return;
        }

        auto * data = &state->data;
        
        DebugText("SchedulingToPool", 16);
        co_await data->workerPool->schedule();
        
        DebugText("Scheduled", 9);

        {
            std::lock_guard<std::mutex> guard(data->lifecycleMutex);
            data->workerActive = true;
            data->lifecycleCv.notify_all();
        }
        
        DebugText("EnteringMainLoop", 16);

        while (!data->shutdownRequested.load(std::memory_order_acquire))
        {
            ZoneScopedN("WorkerLoop_Iteration");
            auto jobResult = co_await data->jobQueue.pop();
            if (!jobResult.has_value())
            {
                if (jobResult.error() == coro::queue_consume_result::stopped)
                {
                    break;
                }

                continue;
            }

            RenderJob const & job = jobResult.value();

            auto cancellationCheck = [data, jobEpoch = job.epoch]() -> bool
            {
                return data->shutdownRequested.load(std::memory_order_acquire) ||
                       data->latestEpoch.load(std::memory_order_acquire) > jobEpoch;
            };

            if (cancellationCheck())
            {
                continue;
            }

            if (!data->jobExecutor)
            {
                continue;
            }

            FrameResultMeta result{};
            try
            {
                std::cout << "[WORKER] About to execute job executor..." << std::endl;
                result = data->jobExecutor(job, cancellationCheck);
                std::cout << "[WORKER] Job executor returned: cancelled=" << result.cancelled 
                          << ", completedLine=" << result.completedLine 
                          << ", completedFrame=" << result.completedFrame << std::endl;
            }
            catch (...)
            {
                std::cout << "[WORKER] Job executor threw exception!" << std::endl;
                result.epoch = job.epoch;
                result.frameId = job.frameHint;
                result.cancelled = true;
            }

            {
                std::lock_guard<std::mutex> guard(data->resultMutex);
                std::cout << "[WORKER] Storing result in pendingResult" << std::endl;
                data->pendingResult = std::move(result);
                data->hasPendingResult.store(true, std::memory_order_release);
                std::cout << "[WORKER] Result stored, hasPendingResult=true" << std::endl;
            }
        }

        {
            std::lock_guard<std::mutex> guard(data->lifecycleMutex);
            data->workerActive = false;
            data->lifecycleCv.notify_all();
        }

        co_return;
    }

    void AsyncRenderController::initializeAsyncResources(ComputeContext & context,
                                                        size_t width,
                                                        size_t height)
    {
        ProfileFunction
        // Clean up old resources if dimensions changed
        if (m_stagingWidth != width || m_stagingHeight != height || !m_workerQueue || !m_stagingBuffer)
        {
            m_stagingBuffer.reset();
            m_workerQueue.reset();

            // Create dedicated worker command queue (no GL interop needed)
            m_workerQueue = std::make_unique<cl::CommandQueue>(context.createQueue());

            // Create CL-only staging buffer for async rendering
            static cl::ImageFormat const format = {CL_RGBA, CL_FLOAT};
            m_stagingBuffer = context.createImage2DChecked(format,
                                                          width,
                                                          height,
                                                          CL_MEM_READ_WRITE,
                                                          0,
                                                          nullptr,
                                                          "AsyncRenderController::stagingBuffer");

            m_stagingWidth = width;
            m_stagingHeight = height;

            // Initialize frame buffers with GLImageBuffer instances
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            for (size_t i = 0; i < m_frameBuffers.size(); ++i)
            {
                auto & fb = m_frameBuffers[i];
                fb.image = std::make_shared<GLImageBuffer>(context, width, height);
                fb.image->allocateOnDevice();
                fb.state.store(FrameState::Idle, std::memory_order_release);
                fb.frameId.store(0, std::memory_order_release);
                fb.epoch.store(0, std::memory_order_release);
                fb.readyTimestampNs.store(0, std::memory_order_release);
                fb.publishTimestampNs.store(0, std::memory_order_release);
            }

            // First buffer starts as Front
            m_frameBuffers[0].state.store(FrameState::Front, std::memory_order_release);
            m_frontBufferIndex.store(0, std::memory_order_release);
        }
    }

    cl::CommandQueue * AsyncRenderController::workerQueue() noexcept
    {
        return m_workerQueue.get();
    }

    cl::Image2D * AsyncRenderController::stagingBuffer() noexcept
    {
        return m_stagingBuffer.get();
    }

    void AsyncRenderController::downloadStagingBufferAsync(std::vector<cl_float4> & outPixels)
    {
        ProfileFunction

        if (!m_workerQueue || !m_stagingBuffer)
        {
            return;
        }

        size_t const expectedSize = m_stagingWidth * m_stagingHeight;
        if (outPixels.size() != expectedSize)
        {
            outPixels.resize(expectedSize);
        }

        // Async read from staging buffer to CPU memory (UI thread uses UI queue for this)
        // Note: This is intentionally blocking (CL_TRUE) to ensure pixels are available before upload
        m_workerQueue->enqueueReadImage(*m_stagingBuffer,
                                       CL_TRUE,
                                       {},
                                       {m_stagingWidth, m_stagingHeight, 1u},
                                       0,
                                       0,
                                       outPixels.data());
        m_workerQueue->finish();
    }

    FrameBuffer * AsyncRenderController::findBufferInState(FrameState state) noexcept
    {
        for (auto & buffer : m_frameBuffers)
        {
            if (buffer.state.load(std::memory_order_acquire) == state)
            {
                return &buffer;
            }
        }
        return nullptr;
    }

    bool AsyncRenderController::tryTransitionBuffer(FrameBuffer * buffer,
                                                   FrameState expectedState,
                                                   FrameState newState) noexcept
    {
        ProfileFunction

        if (!buffer)
        {
            return false;
        }

        FrameState expected = expectedState;
        return buffer->state.compare_exchange_strong(expected,
                                                    newState,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
    }

    FrameBuffer * AsyncRenderController::frontBuffer() noexcept
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        auto const index = m_frontBufferIndex.load(std::memory_order_acquire);
        return &m_frameBuffers[index];
    }

    FrameBuffer * AsyncRenderController::acquireWriteBuffer(uint64_t epoch) noexcept
    {
        ProfileFunction
        std::lock_guard<std::mutex> lock(m_bufferMutex);

        // Log current buffer states for debugging
        std::cout << "[acquireWriteBuffer] Looking for Idle buffer, epoch=" << epoch << std::endl;
        for (size_t i = 0; i < m_frameBuffers.size(); ++i)
        {
            auto const state = m_frameBuffers[i].state.load(std::memory_order_acquire);
            auto const bufEpoch = m_frameBuffers[i].epoch.load(std::memory_order_acquire);
            std::cout << "  Buffer[" << i << "]: state=" << static_cast<int>(state) 
                      << ", epoch=" << bufEpoch << std::endl;
        }

        // Find an Idle buffer to write to
        for (auto & buffer : m_frameBuffers)
        {
            if (tryTransitionBuffer(&buffer, FrameState::Idle, FrameState::Writing))
            {
                buffer.epoch.store(epoch, std::memory_order_release);
                std::cout << "[acquireWriteBuffer] SUCCESS - acquired buffer" << std::endl;
                return &buffer;
            }
        }

        // No Idle buffer available - this shouldn't happen with triple buffering
        // but can happen with double buffering if UI is slow
        std::cout << "[acquireWriteBuffer] FAILED - no Idle buffer available!" << std::endl;
        return nullptr;
    }

    void AsyncRenderController::publishFrame(FrameBuffer * buffer,
                                            uint64_t frameId,
                                            uint64_t epoch) noexcept
    {
        ProfileFunction
        if (!buffer)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_bufferMutex);

        buffer->frameId.store(frameId, std::memory_order_release);
        buffer->epoch.store(epoch, std::memory_order_release);

        // Get current timestamp (nanoseconds since epoch)
        auto const now = std::chrono::high_resolution_clock::now();
        auto const ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        buffer->readyTimestampNs.store(static_cast<uint64_t>(ns), std::memory_order_release);

        // Transition Writing → Ready
        FrameState expected = FrameState::Writing;
        buffer->state.compare_exchange_strong(expected,
                                             FrameState::Ready,
                                             std::memory_order_release,
                                             std::memory_order_acquire);
    }

    FrameBuffer * AsyncRenderController::promoteReadyToFront() noexcept
    {
        ProfileFunction
        std::lock_guard<std::mutex> lock(m_bufferMutex);

        // Find the newest Ready buffer (highest frameId)
        FrameBuffer * newestReady = nullptr;
        uint64_t highestFrameId = 0;

        for (auto & buffer : m_frameBuffers)
        {
            if (buffer.state.load(std::memory_order_acquire) == FrameState::Ready)
            {
                auto const frameId = buffer.frameId.load(std::memory_order_acquire);
                if (frameId > highestFrameId)
                {
                    highestFrameId = frameId;
                    newestReady = &buffer;
                }
            }
        }

        if (!newestReady)
        {
            return nullptr; // No Ready buffer available
        }

        // Transition Ready → Resampling
        if (!tryTransitionBuffer(newestReady, FrameState::Ready, FrameState::Resampling))
        {
            return nullptr; // Lost race
        }

        // After resampling completes, we'll transition Resampling → Front
        // and old Front → Idle in a separate call
        return newestReady;
    }

    void AsyncRenderController::releaseStaleBuffers(uint64_t oldEpoch) noexcept
    {
        if (!m_state)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_bufferMutex);
        
        std::cout << "[releaseStaleBuffers] Checking for stale buffers from epoch " << oldEpoch << std::endl;
        
        for (auto & buffer : m_frameBuffers)
        {
            auto const state = buffer.state.load(std::memory_order_acquire);
            auto const bufEpoch = buffer.epoch.load(std::memory_order_acquire);
            
            // Release Writing buffers from old epochs
            if (state == FrameState::Writing && bufEpoch <= oldEpoch)
            {
                std::cout << "  Found stale Writing buffer with epoch " << bufEpoch 
                          << ", releasing to Idle" << std::endl;
                [[maybe_unused]] bool const released =
                  tryTransitionBuffer(&buffer, FrameState::Writing, FrameState::Idle);
            }
        }
    }
}

