#pragma once

#include "../ConfigManager.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>

namespace gladius::ui
{

    /**
     * @brief Manages the list of recently opened/modified files
     *
     * Stores recent files in the application configuration with timestamps.
     * Files are sorted by most recently accessed. The list is capped at a
     * maximum size and automatically removes entries for files that no longer exist.
     */
    class RecentFilesManager
    {
      public:
        /// Maximum number of recent files to store
        static constexpr size_t kMaxRecentFiles = 100;

        /**
         * @brief Construct a RecentFilesManager
         * @param configManager Configuration manager for persistence (can be null)
         */
        explicit RecentFilesManager(ConfigManager * configManager);

        ~RecentFilesManager() = default;

        // Non-copyable
        RecentFilesManager(RecentFilesManager const &) = delete;
        RecentFilesManager & operator=(RecentFilesManager const &) = delete;

        /**
         * @brief Add or update a file in the recent files list
         * @param filePath Path to the file to add/update
         *
         * The file is moved to the top of the list with the current timestamp.
         * If the file was already in the list, its timestamp is updated.
         */
        void addFile(std::filesystem::path const & filePath);

        /**
         * @brief Get the list of recent files
         * @param maxCount Maximum number of files to return
         * @return Vector of (path, timestamp) pairs, most recent first
         *
         * Only returns files that still exist on disk.
         */
        std::vector<std::pair<std::filesystem::path, std::time_t>>
        getFiles(size_t maxCount = kMaxRecentFiles) const;

        /**
         * @brief Remove a file from the recent files list
         * @param filePath Path to remove
         */
        void removeFile(std::filesystem::path const & filePath);

        /**
         * @brief Clear all recent files
         */
        void clear();

      private:
        ConfigManager * m_configManager = nullptr;

        static constexpr char const * kConfigSection = "recentFiles";
        static constexpr char const * kConfigKey = "files";
    };

} // namespace gladius::ui
