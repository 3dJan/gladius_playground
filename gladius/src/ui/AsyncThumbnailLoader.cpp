#include "AsyncThumbnailLoader.h"

#include <algorithm>
#include <fmt/format.h>

namespace gladius::ui
{
    AsyncThumbnailLoader::AsyncThumbnailLoader(events::SharedLogger logger,
                                               size_t maxConcurrentLoads)
        : m_logger(std::move(logger))
        , m_maxConcurrentLoads(maxConcurrentLoads)
    {
    }

    AsyncThumbnailLoader::~AsyncThumbnailLoader()
    {
        cancelAll();
    }

    void AsyncThumbnailLoader::requestLoad(ThreemfThumbnailExtractor::ThumbnailInfo & info)
    {
        // Only proceed if NotStarted or Failed (retry case)
        if (info.loadState == ThumbnailLoadState::Loading ||
            info.loadState == ThumbnailLoadState::DecodedPending ||
            info.loadState == ThumbnailLoadState::Ready)
        {
            return; // Already loading, decoded, or completed successfully
        }

        // Check if already in queue
        auto it = std::find(m_pendingQueue.begin(), m_pendingQueue.end(), &info);
        if (it != m_pendingQueue.end())
        {
            return;
        }

        // Check if already active
        auto activeIt = std::find_if(m_activeTasks.begin(),
                                     m_activeTasks.end(),
                                     [&info](const ThumbnailLoadTask & task)
                                     { return task.info == &info; });
        if (activeIt != m_activeTasks.end())
        {
            return;
        }

        // Start immediately if capacity available, otherwise queue
        if (m_activeTasks.size() < m_maxConcurrentLoads)
        {
            startLoad(info);
        }
        else
        {
            m_pendingQueue.push_back(&info);
            info.loadState = ThumbnailLoadState::Loading; // Mark as loading even if queued
        }
    }

    void AsyncThumbnailLoader::startLoad(ThreemfThumbnailExtractor::ThumbnailInfo & info)
    {
        info.loadState = ThumbnailLoadState::Loading;

        // Capture path by value for the async operation
        std::filesystem::path filePath = info.filePath;

        ThumbnailLoadTask task;
        task.info = &info;
        task.startTime = std::chrono::steady_clock::now();
        task.future = std::async(std::launch::async,
                                 [filePath]()
                                 {
                                     return ThreemfThumbnailExtractor::extractThumbnailDataOnly(
                                       filePath);
                                 });

        m_activeTasks.push_back(std::move(task));
    }

    void AsyncThumbnailLoader::update()
    {
        // Check completed futures
        auto it = m_activeTasks.begin();
        while (it != m_activeTasks.end())
        {
            // Check if future is ready (non-blocking)
            if (it->future.valid() &&
                it->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                try
                {
                    ThumbnailLoadResult result = it->future.get();

                    if (result.success && it->info)
                    {
                        // Store decoded pixels in the info struct
                        it->info->decodedPixels = std::move(result.decodedPixels);
                        it->info->thumbnailWidth = result.width;
                        it->info->thumbnailHeight = result.height;
                        it->info->fileInfo.fileSize = result.fileSize;
                        for (const auto & [key, value] : result.metadata)
                        {
                            it->info->fileInfo.addMetadata(key, value);
                        }
                        it->info->hasThumbnail = true;
                        it->info->thumbnailLoaded = true;
                        it->info->loadState = ThumbnailLoadState::DecodedPending;
                    }
                    else if (it->info)
                    {
                        // Mark as failed but still update file size and metadata if available
                        if (result.fileSize > 0)
                        {
                            it->info->fileInfo.fileSize = result.fileSize;
                        }
                        for (const auto & [key, value] : result.metadata)
                        {
                            it->info->fileInfo.addMetadata(key, value);
                        }
                        it->info->loadState = ThumbnailLoadState::Failed;
                        it->info->hasThumbnail = false;
                        it->info->thumbnailLoaded = true;
                    }
                }
                catch (const std::exception & e)
                {
                    if (it->info)
                    {
                        it->info->loadState = ThumbnailLoadState::Failed;
                        it->info->hasThumbnail = false;
                        it->info->thumbnailLoaded = true;
                    }
                    if (m_logger)
                    {
                        m_logger->addEvent({fmt::format("Async thumbnail load failed: {}", e.what()),
                                            events::Severity::Warning});
                    }
                }

                it = m_activeTasks.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Start queued loads if capacity available
        processQueue();
    }

    void AsyncThumbnailLoader::processPendingTextures()
    {
        // TODO: Move texture creation logic here when we refactor ownership.
        // Currently, WelcomeScreen owns the ThumbnailInfo containers and calls
        // ThreemfThumbnailExtractor::createTextureFromPixels() directly for any
        // thumbnails in DecodedPending state.
    }

    void AsyncThumbnailLoader::cancelAll()
    {
        // Reset state for all queued items
        for (auto * info : m_pendingQueue)
        {
            if (info)
            {
                info->loadState = ThumbnailLoadState::NotStarted;
            }
        }
        m_pendingQueue.clear();

        // Reset state for active tasks (futures will be destroyed)
        for (auto & task : m_activeTasks)
        {
            if (task.info)
            {
                task.info->loadState = ThumbnailLoadState::NotStarted;
            }
        }
        m_activeTasks.clear();
    }

    bool AsyncThumbnailLoader::hasPendingWork() const noexcept
    {
        return !m_activeTasks.empty() || !m_pendingQueue.empty();
    }

    void AsyncThumbnailLoader::processQueue()
    {
        while (m_activeTasks.size() < m_maxConcurrentLoads && !m_pendingQueue.empty())
        {
            auto * info = m_pendingQueue.front();
            m_pendingQueue.erase(m_pendingQueue.begin());

            if (info && info->loadState == ThumbnailLoadState::Loading)
            {
                // Actually start the load now
                startLoad(*info);
            }
        }
    }
}
