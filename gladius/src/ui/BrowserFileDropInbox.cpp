#include "BrowserFileDropInbox.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>
#include <vector>

namespace gladius::ui
{
    BrowserFileDropInbox::BrowserFileDropInbox(std::filesystem::path directory)
        : m_directory(std::move(directory).lexically_normal())
    {
    }

    bool BrowserFileDropInbox::ensureDirectory() const
    {
        std::error_code error;
        std::filesystem::create_directories(m_directory, error);
        if (error)
        {
            return false;
        }

        return std::filesystem::is_directory(m_directory, error) && !error;
    }

    std::optional<std::filesystem::path> BrowserFileDropInbox::claimNext()
    {
        if (!ensureDirectory())
        {
            return std::nullopt;
        }

        std::vector<std::filesystem::path> candidates;
        std::error_code error;
        std::filesystem::directory_iterator entries{
          m_directory, std::filesystem::directory_options::skip_permission_denied, error};
        if (error)
        {
            return std::nullopt;
        }

        for (auto const & entry : entries)
        {
            if (isCompleted3mfFile(entry))
            {
                candidates.push_back(entry.path().lexically_normal());
            }
        }

        std::sort(candidates.begin(), candidates.end());
        for (auto const & candidate : candidates)
        {
            if (m_claimedPaths.insert(candidate).second)
            {
                return candidate;
            }
        }

        return std::nullopt;
    }

    bool BrowserFileDropInbox::isManagedPath(
      std::filesystem::path const & filePath) const noexcept
    {
        auto const relativePath = filePath.lexically_normal().lexically_relative(m_directory);
        if (relativePath.empty() || relativePath == ".")
        {
            return false;
        }

        auto const firstComponent = relativePath.begin();
        return firstComponent != relativePath.end() && firstComponent->string() != "..";
    }

    bool BrowserFileDropInbox::isCompleted3mfFile(
      std::filesystem::directory_entry const & entry) const
    {
        std::error_code error;
        if (!entry.is_regular_file(error) || error)
        {
            return false;
        }

        auto extension = entry.path().extension().string();
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return extension == ".3mf";
    }
}
