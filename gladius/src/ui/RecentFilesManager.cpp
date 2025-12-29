#include "RecentFilesManager.h"

namespace gladius::ui
{

    RecentFilesManager::RecentFilesManager(ConfigManager * configManager)
        : m_configManager(configManager)
    {
    }

    void RecentFilesManager::addFile(std::filesystem::path const & filePath)
    {
        if (!m_configManager)
        {
            return;
        }

        // Get current time as Unix timestamp
        auto now = std::chrono::system_clock::now();
        std::time_t const timestamp = std::chrono::system_clock::to_time_t(now);

        // Get existing recent files list
        nlohmann::json recentFiles =
          m_configManager->getValue<nlohmann::json>(kConfigSection, kConfigKey, nlohmann::json::array());

        std::string const filePathStr = filePath.string();

        // Create updated list with current file at the top
        nlohmann::json updatedList = nlohmann::json::array();

        // Add the current file with updated timestamp
        nlohmann::json newEntry;
        newEntry["path"] = filePathStr;
        newEntry["timestamp"] = timestamp;
        updatedList.push_back(newEntry);

        // Add other existing files, skipping the current one (it's already added)
        size_t count = 1; // Start at 1 since we already added the current file

        for (auto & entry : recentFiles)
        {
            if (count >= kMaxRecentFiles)
            {
                break;
            }

            if (entry.is_object() && entry.contains("path") && entry["path"].is_string())
            {
                std::string path = entry["path"].get<std::string>();

                // Skip the current file (we already added it with a new timestamp)
                if (path != filePathStr)
                {
                    updatedList.push_back(entry);
                    ++count;
                }
            }
        }

        // Store updated list and save
        m_configManager->setValue(kConfigSection, kConfigKey, updatedList);
        m_configManager->save();
    }

    std::vector<std::pair<std::filesystem::path, std::time_t>>
    RecentFilesManager::getFiles(size_t maxCount) const
    {
        std::vector<std::pair<std::filesystem::path, std::time_t>> result;

        if (!m_configManager)
        {
            return result;
        }

        // Get recent files list from config
        nlohmann::json recentFiles =
          m_configManager->getValue<nlohmann::json>(kConfigSection, kConfigKey, nlohmann::json::array());

        // Process each entry
        for (auto & entry : recentFiles)
        {
            if (result.size() >= maxCount)
            {
                break;
            }

            if (entry.is_object() && entry.contains("path") && entry["path"].is_string() &&
                entry.contains("timestamp") && entry["timestamp"].is_number())
            {
                std::string path = entry["path"].get<std::string>();
                std::time_t timestamp = entry["timestamp"].get<std::time_t>();

                // Only add if the file still exists
                if (std::filesystem::exists(path))
                {
                    result.emplace_back(std::filesystem::path(path), timestamp);
                }
            }
        }

        return result;
    }

    void RecentFilesManager::removeFile(std::filesystem::path const & filePath)
    {
        if (!m_configManager)
        {
            return;
        }

        nlohmann::json recentFiles =
          m_configManager->getValue<nlohmann::json>(kConfigSection, kConfigKey, nlohmann::json::array());

        std::string const filePathStr = filePath.string();
        nlohmann::json updatedList = nlohmann::json::array();

        for (auto & entry : recentFiles)
        {
            if (entry.is_object() && entry.contains("path") && entry["path"].is_string())
            {
                std::string path = entry["path"].get<std::string>();
                if (path != filePathStr)
                {
                    updatedList.push_back(entry);
                }
            }
        }

        m_configManager->setValue(kConfigSection, kConfigKey, updatedList);
        m_configManager->save();
    }

    void RecentFilesManager::clear()
    {
        if (!m_configManager)
        {
            return;
        }

        m_configManager->setValue(kConfigSection, kConfigKey, nlohmann::json::array());
        m_configManager->save();
    }

} // namespace gladius::ui
