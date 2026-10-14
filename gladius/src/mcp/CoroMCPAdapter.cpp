#include "CoroMCPAdapter.h"
#include "../Application.h"
#include "../Document.h"
#include <filesystem>
#include <iostream>

namespace gladius::mcp
{

    CoroMCPAdapter::CoroMCPAdapter(Application * application,
                                   std::size_t backgroundThreads,
                                   std::size_t computeThreads)
        : m_application(application)
        , m_lastErrorMessage("No error")
    {
        if (!m_application)
        {
            throw std::invalid_argument("Application pointer cannot be null");
        }

        // Create thread pools for different operation types
          m_backgroundPool = coro::thread_pool::make_unique(
          coro::thread_pool::options{.thread_count = static_cast<uint32_t>(backgroundThreads),
                                     .on_thread_start_functor = [](std::size_t) -> void {},
                             .on_thread_stop_functor = [](std::size_t) -> void {}});

          m_computePool = coro::thread_pool::make_unique(
          coro::thread_pool::options{.thread_count = static_cast<uint32_t>(computeThreads),
                                     .on_thread_start_functor = [](std::size_t) -> void {},
                             .on_thread_stop_functor = [](std::size_t) -> void {}});
    }

    CoroMCPAdapter::~CoroMCPAdapter()
    {
        // Shutdown thread pools gracefully
        if (m_backgroundPool)
        {
            m_backgroundPool->shutdown();
        }
        if (m_computePool)
        {
            m_computePool->shutdown();
        }
    }

    auto CoroMCPAdapter::saveDocumentAsync(const std::string & path) -> coro::task<bool>
    {
        // Validate input on current thread
        if (!validatePath(path))
        {
            co_return false;
        }

        // Get document reference (main thread operation)
        auto document = m_application->getCurrentDocument();
        if (!document)
        {
            setError("No active document available");
            co_return false;
        }

        try
        {
            // Switch to background thread for I/O operations
            co_await m_backgroundPool->schedule();

            // This now runs on background thread - no UI blocking!
            // In headless mode, include thumbnail generation to embed a preview in the 3MF.
            try
            {
                if (auto doc = m_application->getCurrentDocument())
                {
                    if (auto log = doc->getSharedLogger())
                    {
                        log->logInfo(
                          std::string{"MCP.saveDocumentAsync: start path='"} + path +
                          (m_application->isHeadlessMode() ? "' headless=1" : "' headless=0"));
                    }
                }
            }
            catch (...)
            {
            }
            bool const writeThumbnail = m_application && m_application->isHeadlessMode();
            try
            {
                if (auto log = document->getSharedLogger())
                {
                    log->logInfo(std::string{"MCP.saveDocumentAsync: saving 3MF (thumb="} +
                                 (writeThumbnail ? "1)" : "0)"));
                }
            }
            catch (...)
            {
            }
            document->saveAs(std::filesystem::path(path), writeThumbnail);

            // Success
            m_lastErrorMessage = writeThumbnail
                                   ? "Document saved successfully (thumbnail generation is best-effort)"
                                   : "Document saved successfully";
            try
            {
                if (auto log = document->getSharedLogger())
                {
                    log->logInfo(std::string{"MCP.saveDocumentAsync: done: "} + m_lastErrorMessage);
                }
            }
            catch (...)
            {
            }
            co_return true;
        }
        catch (const std::exception & e)
        {
            setError("Save operation failed: " + std::string(e.what()));
            try
            {
                if (auto doc = m_application->getCurrentDocument())
                {
                    if (auto log = doc->getSharedLogger())
                    {
                        log->logError(std::string{"MCP.saveDocumentAsync: exception: "} + e.what());
                    }
                }
            }
            catch (...)
            {
            }
            co_return false;
        }
    }

    auto CoroMCPAdapter::saveDocumentWithThumbnailAsync(const std::string & path)
      -> coro::task<bool>
    {
        // Validate input
        if (!validatePath(path))
        {
            co_return false;
        }

        auto document = m_application->getCurrentDocument();
        if (!document)
        {
            setError("No active document available");
            co_return false;
        }

        try
        {
            co_await m_backgroundPool->schedule();
            document->saveAs(std::filesystem::path(path), true);

            m_lastErrorMessage =
              "Document saved successfully (thumbnail generation is best-effort)";
            co_return true;
        }
        catch (const std::exception & e)
        {
            setError("Save with thumbnail failed: " + std::string(e.what()));
            co_return false;
        }
    }

    bool CoroMCPAdapter::saveDocumentAs(const std::string & path)
    {
        try
        {
            // Use coro::sync_wait to bridge async/sync worlds
            // This blocks the current thread but the actual work happens on background threads
            return coro::sync_wait(saveDocumentAsync(path));
        }
        catch (const std::exception & e)
        {
            setError("Synchronous save wrapper failed: " + std::string(e.what()));
            return false;
        }
    }

    auto CoroMCPAdapter::generateThumbnailAsync() -> coro::task<bool>
    {
        auto document = m_application->getCurrentDocument();
        if (!document)
        {
            setError("No active document available for thumbnail generation");
            co_return false;
        }

        try
        {
            // Switch to a worker thread for backend rendering operations.
            co_await m_computePool->schedule();
            auto const image = document->createThumbnailPng();
            if (image.data.empty())
            {
                setError("Thumbnail generation returned no image data");
                co_return false;
            }

            m_lastErrorMessage = "Thumbnail generated successfully";
            co_return true;
        }
        catch (const std::exception & e)
        {
            setError("Thumbnail generation failed: " + std::string(e.what()));
            co_return false;
        }
    }

    bool CoroMCPAdapter::validatePath(const std::string & path)
    {
        if (path.empty())
        {
            setError("File path cannot be empty");
            return false;
        }

        if (!path.ends_with(".3mf"))
        {
            setError("File must have .3mf extension");
            return false;
        }

        // Check if directory exists and is writable
        std::filesystem::path filePath(path);
        auto parentDir = filePath.parent_path();

        if (!parentDir.empty() && !std::filesystem::exists(parentDir))
        {
            try
            {
                std::filesystem::create_directories(parentDir);
            }
            catch (const std::exception & e)
            {
                setError("Cannot create directory: " + std::string(e.what()));
                return false;
            }
        }

        return true;
    }

    bool CoroMCPAdapter::setError(const std::string & message)
    {
        m_lastErrorMessage = message;
        std::cerr << "MCP Error: " << message << std::endl;
        return false;
    }

} // namespace gladius::mcp
