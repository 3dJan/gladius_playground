#include "AsyncThumbnailLoader.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <utility>

namespace gladius::ui
{
    AsyncThumbnailLoader::AsyncThumbnailLoader(events::SharedLogger logger,
                                               size_t maxConcurrentLoads,
                                               ThumbnailLoadFunction loadFunction)
        : m_logger(std::move(logger))
        , m_maxConcurrentLoads(std::max<size_t>(1, maxConcurrentLoads))
        , m_loadFunction(std::move(loadFunction))
    {
        if (!m_loadFunction)
        {
            m_loadFunction = [](std::filesystem::path const & filePath)
            { return ThreemfThumbnailExtractor::extractThumbnailDataOnly(filePath); };
        }
    }

    AsyncThumbnailLoader::~AsyncThumbnailLoader()
    {
        cancelAll();
        for (auto & task : m_activeTasks)
        {
            if (task.future.valid())
            {
                task.future.wait();
            }
        }
        m_activeTasks.clear();
    }

    ThumbnailLoadRequestId AsyncThumbnailLoader::requestLoad(
      ThreemfThumbnailExtractor::ThumbnailInfo & info,
      ThumbnailLoadPriority priority)
    {
        // Only proceed if NotStarted or Failed (retry case)
        if (info.loadState == ThumbnailLoadState::Loading ||
            info.loadState == ThumbnailLoadState::DecodedPending ||
            info.loadState == ThumbnailLoadState::Ready)
        {
            return info.loadRequestId;
        }

        ThumbnailLoadRequestId requestId = m_nextRequestId++;
        if (requestId == 0)
        {
            requestId = m_nextRequestId++;
        }

        info.loadRequestId = requestId;
        info.loadState = ThumbnailLoadState::Loading;

        ThumbnailLoadRequest request{requestId, info.filePath};
        if (priority == ThumbnailLoadPriority::High)
        {
            m_highPriorityQueue.push_back(std::move(request));
        }
        else
        {
            m_pendingQueue.push_back(std::move(request));
        }

        processQueue();
        return requestId;
    }

    void AsyncThumbnailLoader::startLoad(ThumbnailLoadRequest request)
    {
        ThumbnailLoadTask task;
        task.requestId = request.requestId;
        task.filePath = std::move(request.filePath);
#ifndef __EMSCRIPTEN__
        auto filePath = task.filePath;
        auto loadFunction = m_loadFunction;
        try
        {
            task.future = std::async(std::launch::async,
                                     [filePath = std::move(filePath),
                                      loadFunction = std::move(loadFunction)]()
                                     { return loadFunction(filePath); });
        }
        catch (std::exception const & e)
        {
            ThumbnailLoadResult result;
            result.errorMessage = e.what();
            task.immediateResult = std::move(result);
            if (m_logger)
            {
                m_logger->addEvent({fmt::format("Could not start thumbnail load for {}: {}",
                                                task.filePath.string(),
                                                e.what()),
                                    events::Severity::Warning});
            }
        }
        catch (...)
        {
            ThumbnailLoadResult result;
            result.errorMessage = "Could not start thumbnail load";
            task.immediateResult = std::move(result);
        }
#endif

        m_activeTasks.push_back(std::move(task));
    }

    std::vector<ThumbnailLoadCompletion> AsyncThumbnailLoader::update()
    {
        std::vector<ThumbnailLoadCompletion> completions;
        auto it = m_activeTasks.begin();
        while (it != m_activeTasks.end())
        {
#ifdef __EMSCRIPTEN__
            if (it->cancelled)
            {
                it = m_activeTasks.erase(it);
                continue;
            }

            ThumbnailLoadResult result;
            try
            {
                result = m_loadFunction(it->filePath);
            }
            catch (std::exception const & e)
            {
                result.errorMessage = e.what();
            }
            catch (...)
            {
                result.errorMessage = "Unknown error during thumbnail load";
            }

            completions.push_back({it->requestId, std::move(result)});
            it = m_activeTasks.erase(it);
#else
            bool isReady = it->immediateResult.has_value();
            if (!isReady && !it->future.valid())
            {
                isReady = true;
            }
            else if (!isReady)
            {
                isReady = it->future.wait_for(std::chrono::milliseconds(0)) ==
                          std::future_status::ready;
            }

            if (!isReady)
            {
                ++it;
                continue;
            }

            ThumbnailLoadResult result;
            try
            {
                if (it->immediateResult)
                {
                    result = std::move(*it->immediateResult);
                }
                else if (it->future.valid())
                {
                    result = it->future.get();
                }
                else
                {
                    result.errorMessage = "Thumbnail load did not produce a result";
                }
            }
            catch (std::exception const & e)
            {
                result.errorMessage = e.what();
                if (m_logger)
                {
                    m_logger->addEvent({fmt::format("Async thumbnail load failed for {}: {}",
                                                    it->filePath.string(),
                                                    e.what()),
                                        events::Severity::Warning});
                }
            }
            catch (...)
            {
                result.errorMessage = "Unknown error during thumbnail load";
            }

            if (!it->cancelled)
            {
                completions.push_back({it->requestId, std::move(result)});
            }
            it = m_activeTasks.erase(it);
#endif
        }

        processQueue();
        return completions;
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
        m_highPriorityQueue.clear();
        m_pendingQueue.clear();

        for (auto & task : m_activeTasks)
        {
            task.cancelled = true;
        }
    }

    void AsyncThumbnailLoader::cancelRequest(ThumbnailLoadRequestId requestId)
    {
        if (requestId == 0)
        {
            return;
        }

        auto removeQueuedRequest = [requestId](std::deque<ThumbnailLoadRequest> & queue)
        {
            queue.erase(std::remove_if(queue.begin(),
                                       queue.end(),
                                       [requestId](ThumbnailLoadRequest const & request)
                                       { return request.requestId == requestId; }),
                        queue.end());
        };
        removeQueuedRequest(m_highPriorityQueue);
        removeQueuedRequest(m_pendingQueue);

        for (auto & task : m_activeTasks)
        {
            if (task.requestId == requestId)
            {
                task.cancelled = true;
            }
        }

        processQueue();
    }

    bool AsyncThumbnailLoader::hasPendingWork() const noexcept
    {
        return !m_highPriorityQueue.empty() || !m_pendingQueue.empty() ||
               std::any_of(m_activeTasks.begin(),
                           m_activeTasks.end(),
                           [](ThumbnailLoadTask const & task) { return !task.cancelled; });
    }

    void AsyncThumbnailLoader::processQueue()
    {
        while (m_activeTasks.size() < getConcurrencyLimit() &&
               (!m_highPriorityQueue.empty() || !m_pendingQueue.empty()))
        {
            auto & queue = m_highPriorityQueue.empty() ? m_pendingQueue : m_highPriorityQueue;
            ThumbnailLoadRequest request = std::move(queue.front());
            queue.pop_front();
            startLoad(std::move(request));
        }
    }

    size_t AsyncThumbnailLoader::getConcurrencyLimit() const noexcept
    {
#ifdef __EMSCRIPTEN__
        return 1;
#else
        return m_maxConcurrentLoads;
#endif
    }
}
