#pragma once

#include "../EventLogger.h"
#include "ThreemfThumbnailExtractor.h"

#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace gladius::ui
{
    /**
     * @brief Represents a single async thumbnail load operation
     */
    struct ThumbnailLoadTask
    {
        ThreemfThumbnailExtractor::ThumbnailInfo * info = nullptr; ///< Pointer to info being loaded
        std::future<ThumbnailLoadResult> future;                   ///< Async operation handle
        std::chrono::steady_clock::time_point startTime;           ///< For timeout tracking
    };

    /**
     * @brief Component responsible for background thumbnail loading
     *
     * This class manages asynchronous loading of thumbnails from 3MF files.
     * It uses std::async to offload file I/O and PNG decoding to background threads,
     * while texture creation happens on the main thread (OpenGL requirement).
     *
     * Usage:
     * 1. Call requestLoad() for each thumbnail that needs loading
     * 2. Call update() each frame to poll futures and update states
     * 3. Call processPendingTextures() each frame to create GL textures (main thread only)
     * 4. Call cancelAll() when the welcome screen closes
     */
    class AsyncThumbnailLoader
    {
      public:
        /**
         * @brief Construct a new Async Thumbnail Loader
         *
         * @param logger Event logger for error reporting
         * @param maxConcurrentLoads Maximum number of simultaneous load operations (default: 4)
         */
        explicit AsyncThumbnailLoader(events::SharedLogger logger, size_t maxConcurrentLoads = 4);

        /**
         * @brief Destroy the Async Thumbnail Loader and cancel pending operations
         */
        ~AsyncThumbnailLoader();

        // Non-copyable, non-movable (owns futures)
        AsyncThumbnailLoader(const AsyncThumbnailLoader &) = delete;
        AsyncThumbnailLoader & operator=(const AsyncThumbnailLoader &) = delete;
        AsyncThumbnailLoader(AsyncThumbnailLoader &&) = delete;
        AsyncThumbnailLoader & operator=(AsyncThumbnailLoader &&) = delete;

        /**
         * @brief Queue a thumbnail for background loading
         *
         * If the thumbnail is already loading or ready, this is a no-op.
         * If max concurrent loads is reached, the request is queued.
         *
         * @param info Thumbnail info to load (will be updated with load state)
         */
        void requestLoad(ThreemfThumbnailExtractor::ThumbnailInfo & info);

        /**
         * @brief Poll futures and update thumbnail states
         *
         * Call this each frame. It checks for completed async operations
         * and transitions thumbnails from Loading to DecodedPending state.
         */
        void update();

        /**
         * @brief Create GL textures for decoded thumbnails
         *
         * MUST be called on the main thread where the GL context is current.
         * Processes thumbnails in DecodedPending state and creates textures.
         */
        void processPendingTextures();

        /**
         * @brief Cancel all pending load operations
         *
         * Call this when the welcome screen closes to clean up resources.
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
        events::SharedLogger m_logger;                ///< Logger for error reporting
        size_t m_maxConcurrentLoads;                  ///< Max parallel loads
        std::vector<ThumbnailLoadTask> m_activeTasks; ///< Currently active load tasks

        /// Queued requests - IMPORTANT: Pointers must remain valid until cancelAll() is called.
        /// The WelcomeScreen guarantees this by not modifying m_thumbnailInfos during loading.
        std::vector<ThreemfThumbnailExtractor::ThumbnailInfo *> m_pendingQueue;

        /**
         * @brief Start a new async load operation for a thumbnail
         *
         * @param info Thumbnail info to load
         */
        void startLoad(ThreemfThumbnailExtractor::ThumbnailInfo & info);

        /**
         * @brief Process the pending queue and start new loads if capacity available
         */
        void processQueue();
    };
}
