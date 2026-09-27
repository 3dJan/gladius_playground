#pragma once

#include "../EventLogger.h"
#include "ThreemfThumbnailExtractor.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <vector>

namespace gladius::ui
{
  using ThumbnailLoadRequestId = std::uint64_t;

  enum class ThumbnailLoadPriority
  {
    Normal,
    High
  };

  using ThumbnailLoadFunction =
    std::function<ThumbnailLoadResult(std::filesystem::path const &)>;

  struct ThumbnailLoadRequest
  {
    ThumbnailLoadRequestId requestId = 0;
    std::filesystem::path filePath;
  };

    /**
     * @brief Represents a single async thumbnail load operation
     */
    struct ThumbnailLoadTask
    {
    ThumbnailLoadRequestId requestId = 0;
    std::filesystem::path filePath;
    std::future<ThumbnailLoadResult> future;
    std::optional<ThumbnailLoadResult> immediateResult;
    bool cancelled = false;
  };

  struct ThumbnailLoadCompletion
  {
    ThumbnailLoadRequestId requestId = 0;
    ThumbnailLoadResult result;
    };

    /**
     * @brief Component responsible for background thumbnail loading
     *
      * This class manages asynchronous loading of thumbnails from 3MF files.
      * Requests own their paths and workers return values; no worker retains a pointer
      * to UI-owned thumbnail state. Texture creation remains on the UI thread.
     *
     * Usage:
     * 1. Call requestLoad() for each thumbnail that needs loading
      * 2. Call update() each frame to poll futures and apply returned completions
     * 3. Call processPendingTextures() each frame to create GL textures (main thread only)
      * 4. Call cancelAll() to invalidate outstanding requests; it does not wait for workers
     */
    class AsyncThumbnailLoader
    {
      public:
        /**
         * @brief Construct a new Async Thumbnail Loader
         *
         * @param logger Event logger for error reporting
         * @param maxConcurrentLoads Maximum number of simultaneous load operations (default: 4)
         * @param loadFunction Optional extraction function, primarily useful for tests
         */
        explicit AsyncThumbnailLoader(events::SharedLogger logger,
                                      size_t maxConcurrentLoads = 4,
                                      ThumbnailLoadFunction loadFunction = {});

        /**
         * @brief Destroy the loader and wait for any uninterruptible active extraction
         */
        ~AsyncThumbnailLoader();

        // Non-copyable and non-movable because the loader owns active futures.
        AsyncThumbnailLoader(AsyncThumbnailLoader const &) = delete;
        AsyncThumbnailLoader & operator=(AsyncThumbnailLoader const &) = delete;
        AsyncThumbnailLoader(AsyncThumbnailLoader &&) = delete;
        AsyncThumbnailLoader & operator=(AsyncThumbnailLoader &&) = delete;

        /**
         * @brief Queue a thumbnail for loading
         *
         * If the thumbnail is already loading or ready, this is a no-op.
         * If max concurrent loads is reached, the owned path is queued.
         *
         * @param info Thumbnail info used to create an owned request; only its state and
         *             request ID are updated on the calling thread
         * @param priority High-priority requests are dequeued before normal requests
         * @return The request ID, or the existing ID if the thumbnail is already loading
         */
        ThumbnailLoadRequestId requestLoad(
          ThreemfThumbnailExtractor::ThumbnailInfo & info,
          ThumbnailLoadPriority priority = ThumbnailLoadPriority::Normal);

        /**
         * @brief Poll loading operations and return completed value results
         *
         * Call this on the UI thread. The caller applies each result to a matching
         * ThumbnailInfo and creates its GPU texture there.
         */
        std::vector<ThumbnailLoadCompletion> update();

        /**
         * @brief Create GL textures for decoded thumbnails
         *
         * MUST be called on the main thread where the GL context is current.
         * Processes thumbnails in DecodedPending state and creates textures.
         */
        void processPendingTextures();

        /**
         * @brief Invalidate one request without waiting for an active worker
         *
         * Queued work is removed immediately. An active extraction is allowed to finish,
         * but its result is discarded.
         */
        void cancelRequest(ThumbnailLoadRequestId requestId);

        /**
         * @brief Invalidate all queued and active requests without waiting for workers
         */
        void cancelAll();

        /**
         * @brief Check if there is pending work
         *
         * @return true if there are active or queued load operations
         * @return false if all work is complete
         */
        [[nodiscard]] bool hasPendingWork() const noexcept;

      private:
        events::SharedLogger m_logger;
        size_t m_maxConcurrentLoads;
        ThumbnailLoadFunction m_loadFunction;
        ThumbnailLoadRequestId m_nextRequestId = 1;
        std::vector<ThumbnailLoadTask> m_activeTasks;
        std::deque<ThumbnailLoadRequest> m_highPriorityQueue;
        std::deque<ThumbnailLoadRequest> m_pendingQueue;

        /**
         * @brief Start a new async load operation for a thumbnail
         *
         * @param request Owned request data
         */
        void startLoad(ThumbnailLoadRequest request);

        /**
         * @brief Process the pending queue and start new loads if capacity available
         */
        void processQueue();

        [[nodiscard]] size_t getConcurrencyLimit() const noexcept;
    };
}
